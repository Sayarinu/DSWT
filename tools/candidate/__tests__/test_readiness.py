from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve()
sys.path.insert(0, str(HERE.parents[1]))
import oci
import readiness

RULES = [{"path": "server", "category": "project-source"}, {"path": "LICENSE", "category": "project-license"}]
COMMIT = "c" * 40


class RepositoryFixture(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.git("init", "-q", "-b", "main")
        self.commit({"LICENSE": b"MIT\n", "server/main.cpp": b"int main() {}\n"}, "main")
        self.git("checkout", "-q", "-b", "side")
        # Content only on another branch must still be inventoried.
        self.commit({"server/old.sav": b"save", "docs/note.md": b"x", "server/key.txt": oci.PRIVATE_SENTINEL}, "side")
        self.git("checkout", "-q", "main")

    def git(self, *arguments):
        return subprocess.run(["git", "-C", str(self.root), *arguments], check=True, capture_output=True, text=True).stdout

    def commit(self, files, message):
        for name, data in files.items():
            (self.root / name).parent.mkdir(parents=True, exist_ok=True)
            (self.root / name).write_bytes(data)
        self.git("add", "-A")
        self.git("-c", "user.name=t", "-c", "user.email=t@example.invalid", "commit", "-qm", message)


class InventoryTests(RepositoryFixture):
    def test_every_ref_is_inventoried_and_classified_by_explicit_paths(self):
        result = readiness.inventory(self.root, RULES, {})
        self.assertEqual(result["refs"], 2)
        self.assertEqual(result["forbidden"], ["server/old.sav"])
        self.assertEqual(result["unclassified"], ["docs/note.md"])
        self.assertEqual([item["paths"] for item in result["private_marker_blobs"]], [["server/key.txt"]])
        self.assertFalse(result["private_marker_blobs"][0]["expected"])
        expected = readiness.inventory(self.root, RULES, {"server/key.txt": "reviewed"})
        self.assertTrue(expected["private_marker_blobs"][0]["expected"])
        # Extensions never classify: an unlisted .cpp is unclassified, not "source".
        self.assertEqual(readiness.classify("other/tool.cpp", RULES), "unclassified")
        self.assertEqual(readiness.classify("serverless/a", RULES), "unclassified")

    def test_source_archive_is_reproducible_complete_and_scanned(self):
        commit = self.git("rev-parse", "side").strip()
        output = self.root / "source.tar"
        result = readiness.source_archive(commit, output, self.root, {})
        self.assertTrue(result["reproducible"] and result["complete"])
        self.assertEqual(result["members"], 5)
        self.assertEqual(sorted(item["path"] for item in result["findings"]), ["server/key.txt", "server/old.sav"])
        self.assertEqual(readiness.sha256(output.read_bytes()), result["sha256"])

    def test_scanner_pin_is_read_from_the_ci_workflow(self):
        workflow = self.root / "contribution.yml"
        workflow.write_text("  https://github.com/gitleaks/gitleaks/releases/download/v8.30.1/gitleaks_8.30.1_linux_x64.tar.gz \\\n"
                            '  --output x\n  echo "' + "a" * 64 + '  $x" | sha256sum --check\n')
        url, version, digest = readiness.SCANNER.search(workflow.read_text()).groups()
        self.assertEqual((version, digest), ("8.30.1", "a" * 64))
        real = readiness.SCANNER.search(readiness.WORKFLOW.read_text())
        self.assertIsNotNone(real, "the repository CI must keep a pinned gitleaks download")


IMAGES = {name: "sha256:" + str(index) * 64 for index, name in enumerate(("database", "gateway", "backend", "validator"), 1)}
STACK = {"project": "dswt-local-" + "0" * 32, "workspace": "/w", "database_system_id": "7", "images": IMAGES}


class BindingTests(unittest.TestCase):
    expected = {"source_commit": COMMIT, "stack": STACK, "candidate_sha256": "e" * 64}

    def bind(self, name, report):
        return readiness.evidence_binding(name, report, self.expected)

    def test_only_same_source_stack_and_image_evidence_applies(self):
        load = {"status": "pass", "verdict": {"status": "pass"}, "source_commit": COMMIT, "source_dirty": False,
                "images": IMAGES, "stack": dict(STACK)}
        self.assertEqual(self.bind("load", load)["status"], "applicable")
        cases = {
            "dirty tree": dict(load, source_dirty=True),
            "differs from candidate source": dict(load, source_commit="d" * 40),
            "gateway image": dict(load, images=dict(IMAGES, gateway="sha256:" + "9" * 64)),
            "validator image": dict(load, images={k: v for k, v in IMAGES.items() if k != "validator"}),
            "no service image": dict(load, images=None, stack={}),
            "database_system_id": dict(load, stack=dict(STACK, database_system_id="8")),
            "is not a pass": dict(load, verdict={"status": "fail"}),
            "None": {key: value for key, value in load.items() if key != "status"},
        }
        for reason, report in cases.items():
            with self.subTest(reason=reason):
                result = self.bind("load", report)
                self.assertEqual(result["status"], "stale")
                self.assertTrue(any(reason in item for item in result["reasons"]), result["reasons"])
        self.assertEqual(self.bind("unknown", load)["status"], "stale")

    def test_each_tool_report_shape_is_checked(self):
        audit = {"status": "complete", "source_commit": COMMIT, "source_dirty": False,
                 "images": {"server": {"config_digest": "x"}, "validator": {"config_digest": "y"}}}
        result = self.bind("image-audit", audit)
        self.assertEqual(result["status"], "applicable")
        self.assertTrue(result["notes"])
        self.assertEqual(self.bind("image-audit", dict(audit, images={"server": {}}))["status"], "stale")
        isolation = {"status": "pass", "source_commit": COMMIT, "source_dirty": False, "database_system_id": "7"}
        self.assertEqual(self.bind("isolation", isolation)["status"], "applicable")
        self.assertEqual(self.bind("isolation", dict(isolation, database_system_id="8"))["status"], "stale")
        source = {"kind": "dswt-source-audit", "source_commit": COMMIT, "source_dirty": False,
                  "inventory": {"forbidden": [], "unclassified": [], "private_marker_blobs": [{"expected": True}]},
                  "source_archive": {"reproducible": True, "complete": True, "findings": []},
                  "secrets": {"git": {"findings": 0}}}
        self.assertEqual(self.bind("source-audit", source)["status"], "applicable")
        for changed in (dict(source, secrets={"git": {"findings": 1}}),
                        dict(source, inventory=dict(source["inventory"], unclassified=["x"])),
                        dict(source, inventory=dict(source["inventory"], private_marker_blobs=[{"expected": False}])),
                        dict(source, source_archive=dict(source["source_archive"], complete=False))):
            self.assertEqual(self.bind("source-audit", changed)["status"], "stale")
        rehearsal = {"status": "pass", "source_commit": COMMIT, "source_dirty": False, "candidate_sha256": "e" * 64}
        self.assertEqual(self.bind("rehearsal", rehearsal)["status"], "applicable")
        self.assertEqual(self.bind("rehearsal", dict(rehearsal, candidate_sha256="f" * 64))["status"], "stale")

    def test_every_report_keeps_unresolved_gates_and_blocks_publication(self):
        self.assertTrue(any("LUNE-323" in gate for gate in readiness.UNRESOLVED))
        self.assertTrue(any("rights" in gate for gate in readiness.UNRESOLVED))
        self.assertTrue(any("Publication" in gate for gate in readiness.UNRESOLVED))
        self.assertEqual(set(readiness.REQUIRED), {"observability", "recovery", "load", "isolation", "image-audit", "source-audit"})


if __name__ == "__main__":
    unittest.main()
