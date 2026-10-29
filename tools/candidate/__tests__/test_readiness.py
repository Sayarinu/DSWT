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
IMAGES = {"gateway": "sha256:" + "1" * 64, "backend": "sha256:" + "2" * 64}


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


class BindingTests(unittest.TestCase):
    expected = {"source_commit": COMMIT, "images": IMAGES}

    def test_only_same_source_and_image_evidence_applies(self):
        applicable = {"status": "pass", "source_commit": COMMIT, "source_dirty": False, "stack": {"images": IMAGES}}
        self.assertEqual(readiness.evidence_binding("load", applicable, self.expected)["status"], "applicable")
        cases = {
            "dirty tree": dict(applicable, source_dirty=True),
            "differs from candidate source": dict(applicable, source_commit="d" * 40),
            "gateway image": dict(applicable, stack={"images": dict(IMAGES, gateway="sha256:" + "9" * 64)}),
            "did not pass": dict(applicable, status="fail"),
            "did not pass ": dict(applicable, verdict={"status": "fail"}),
        }
        for reason, report in cases.items():
            with self.subTest(reason=reason):
                result = readiness.evidence_binding("load", report, self.expected)
                self.assertEqual(result["status"], "stale")
                self.assertTrue(any(reason.strip() in item for item in result["reasons"]), result["reasons"])

    def test_every_report_keeps_unresolved_gates_and_blocks_publication(self):
        self.assertTrue(any("LUNE-323" in gate for gate in readiness.UNRESOLVED))
        self.assertTrue(any("rights" in gate for gate in readiness.UNRESOLVED))
        self.assertTrue(any("Publication" in gate for gate in readiness.UNRESOLVED))


if __name__ == "__main__":
    unittest.main()
