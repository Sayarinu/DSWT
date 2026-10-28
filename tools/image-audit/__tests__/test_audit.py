import copy
import gzip
import io
import json
from pathlib import Path
import sys
import tempfile
import tarfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import audit
import oci


class ContextTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.tracked = {"LICENSE", "NOTICE", "server/Dockerfile", "server/Dockerfile.dockerignore", "server/src/main.cpp"}
        for name in self.tracked:
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("reviewed source " + name)
        self.rules = self.root / "server/Dockerfile.dockerignore"
        self.rules.write_text("**\n" + "".join("!" + p + "\n" for p in sorted(self.tracked)))

    def test_nested_private_files_are_not_staged_even_with_source_extensions(self):
        for name in ("game/private.nds", "server/src/unexpected.cpp", "server/.env", "server/private.txt"):
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(oci.PRIVATE_SENTINEL)
        result = audit.stage_context(self.root, self.root / "output", "server", self.tracked)
        self.assertEqual(set(result), self.tracked)
        self.assertEqual({p.relative_to(self.root / "output").as_posix() for p in (self.root / "output").rglob("*") if p.is_file()}, self.tracked)
        self.assertEqual((self.root / "output/server/src").stat().st_mode & 0o777, 0o755)
        self.assertEqual((self.root / "output/server/src/main.cpp").stat().st_mode & 0o777, 0o644)

    def test_new_tracked_source_requires_an_explicit_allowlist_update(self):
        (self.root / "server/src/new.cpp").write_text("new code")
        with self.assertRaisesRegex(ValueError, "Update server/Dockerfile.dockerignore"):
            audit.context_files(self.root, "server", self.tracked | {"server/src/new.cpp"})

    def test_parent_or_wildcard_allowance_cannot_mask_private_descendants(self):
        for rule in ("!server/", "!server/**", "!server/src/*.cpp"):
            with self.subTest(rule=rule):
                self.rules.write_text("**\n" + rule + "\n")
                with self.assertRaisesRegex(ValueError, "literal"):
                    audit.context_files(self.root, "server", self.tracked)

    def test_symlinks_and_private_marker_in_approved_source_are_rejected(self):
        source = self.root / "server/src/main.cpp"
        source.write_bytes(oci.PRIVATE_SENTINEL)
        with self.assertRaisesRegex(ValueError, "Private sentinel"):
            audit.stage_context(self.root, self.root / "output", "server", self.tracked)
        source.unlink()
        source.symlink_to(self.root / "LICENSE")
        with self.assertRaisesRegex(ValueError, "unlinked"):
            audit.context_files(self.root, "server", self.tracked)

    def test_real_repository_allowlists_cover_all_tracked_service_files(self):
        tracked = set(audit.git("ls-files").splitlines())
        for service in audit.SERVICES:
            with self.subTest(service=service):
                audit.context_files(audit.ROOT, service, tracked)

    def test_old_python_is_rejected_before_workspace_or_network_operations(self):
        with patch.object(sys, "version_info", (3, 9)), patch.object(audit, "workspace_path") as workspace:
            with self.assertRaisesRegex(ValueError, "Python 3.12"):
                audit.main()
            workspace.assert_not_called()


class EvidenceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.context = Path(self.temp.name)

    def test_missing_or_changed_canonical_notice_is_rejected(self):
        image = oci.Image()
        for name in ("LICENSE", "NOTICE"):
            (self.context / name).write_bytes(name.encode())
            image.files["usr/share/platinumwt/" + name] = {"type": "file", "data": name.encode()}
        (self.context / "server/migrations").mkdir(parents=True)
        for i in range(20):
            image.files[f"usr/share/doc/package{i}/copyright"] = {"type": "file", "sha256": "1" * 64}
        image.files["var/lib/dpkg/status"] = {"type": "file", "data": b"Package: libc6\nStatus: install ok installed\nVersion: 1\n\n"}
        image.files["usr/share/doc/libc6/copyright"] = {"type": "file", "sha256": "2" * 64}
        self.assertEqual(audit.audit_sources(image, self.context, "server")["verified_source_files"], 2)
        image.files["usr/share/platinumwt/NOTICE"]["data"] = b"different notice"
        with self.assertRaisesRegex(ValueError, "changed packaged"):
            audit.audit_sources(image, self.context, "server")
        del image.files["usr/share/platinumwt/NOTICE"]
        with self.assertRaisesRegex(ValueError, "Missing required"):
            audit.audit_sources(image, self.context, "server")

    def test_pinned_core_and_runtime_archives_are_bound_to_packaged_source(self):
        context = self.context / "context"
        (context / "validator").mkdir(parents=True)
        output = io.BytesIO()
        core = {"LICENSE": b"upstream license", "Directory.Build.props": b"props", "PKHeX.Core/Example.cs": b"core source"}
        with tarfile.open(fileobj=output, mode="w:gz") as archive:
            for name, data in core.items():
                entry = tarfile.TarInfo("PKHeX-revision/" + name)
                entry.size = len(data)
                archive.addfile(entry, io.BytesIO(data))
        reference = self.context / "reference.tar.gz"
        reference.write_bytes(output.getvalue())
        runtime = gzip.compress(b"complete pinned runtime source")
        inputs = {"LICENSE": b"root license", "NOTICE": b"root notice",
                  "validator/upstream.env": f"PKHEX_REVISION=revision\nPKHEX_ARCHIVE_SHA256={oci.sha256(output.getvalue())}\n".encode(),
                  "validator/runtime-sources.tsv": f"grpc-dotnet grpc/grpc-dotnet revision {oci.sha256(runtime)} 1.0\n".encode()}
        image = oci.Image()
        for name, data in inputs.items():
            (context / name).write_bytes(data)
            image.files[oci.SOURCE + name] = {"type": "file", "data": data}
        for name, data in core.items():
            image.files[oci.SOURCE + "validator/.deps/pkhex/" + name] = {"type": "file", "data": data}
        runtime_path = oci.SOURCE + "validator/.deps/runtime-source/grpc-dotnet-revision.tar.gz"
        image.files[runtime_path] = {"type": "file", "data": runtime}
        for name in [f"usr/share/doc/package{i}/copyright" for i in range(20)] + ["usr/share/dotnet/LICENSE.txt", "usr/share/dotnet/ThirdPartyNotices.txt"]:
            image.files[name] = {"type": "file", "sha256": "1" * 64}
        image.files["var/lib/dpkg/status"] = {"type": "file", "data": b"Package: libc6\nStatus: install ok installed\nVersion: 1\n\n"}
        image.files["usr/share/doc/libc6/copyright"] = {"type": "file", "sha256": "2" * 64}
        self.assertEqual(audit.audit_sources(image, context, "validator", reference)["verified_source_files"], 8)
        core_path = oci.SOURCE + "validator/.deps/pkhex/PKHeX.Core/Example.cs"
        image.files[core_path]["data"] = b"changed core"
        with self.assertRaisesRegex(ValueError, "changed packaged"):
            audit.audit_sources(image, context, "validator", reference)
        image.files[core_path]["data"] = core["PKHeX.Core/Example.cs"]
        image.files[runtime_path]["data"] = b"tampered archive"
        with self.assertRaisesRegex(ValueError, "Runtime source archive mismatch"):
            audit.audit_sources(image, context, "validator", reference)
        reference.write_bytes(b"tampered reference")
        with self.assertRaisesRegex(ValueError, "source archive hash mismatch"):
            audit.audit_sources(image, context, "validator", reference)

    def test_missing_required_package_notice_fails_with_many_other_notices(self):
        image = oci.Image()
        for name in ("LICENSE", "NOTICE"):
            (self.context / name).write_bytes(name.encode())
            image.files["usr/share/platinumwt/" + name] = {"type": "file", "data": name.encode()}
        (self.context / "server/migrations").mkdir(parents=True)
        image.files["var/lib/dpkg/status"] = {"type": "file", "data": b"Package: libc6\nStatus: install ok installed\nVersion: 1\n\nPackage: libpq5\nStatus: install ok installed\nVersion: 16\n\n"}
        for name in ["libc6", "postgresql-16"] + ["unrelated" + str(i) for i in range(25)]:
            image.files[f"usr/share/doc/{name}/copyright"] = {"type": "file", "sha256": "1" * 64}
        image.files["usr/share/doc/libpq5"] = {"type": "symlink", "target": "postgresql-16"}
        self.assertEqual(audit.audit_sources(image, self.context, "server")["base_notices"]["usr/share/doc/libpq5/copyright"]["resolved"],
                         "usr/share/doc/postgresql-16/copyright")
        del image.files["usr/share/doc/postgresql-16/copyright"]
        with self.assertRaisesRegex(ValueError, "Missing required image file"):
            audit.audit_sources(image, self.context, "server")

    def scan(self):
        packages = [{"Name": "libc6" if i == 0 else "libpq5" if i == 1 else "p" + str(i), "Version": "1"} for i in range(25)]
        self.expected_os = {(p["Name"], p["Version"]) for p in packages}
        report = {"SchemaVersion": 2, "ArtifactType": "container_image", "Metadata": {"ImageID": "sha256:config"}, "Results": [
            {"Class": "os-pkgs", "Packages": packages, "Licenses": [{"Name": "MIT"}],
             "Vulnerabilities": [{"VulnerabilityID": "CVE-example", "Severity": "HIGH"}]}]}
        sbom = {"bomFormat": "CycloneDX", "components": [{"name": p["Name"], "version": p["Version"]} for p in packages],
                "vulnerabilities": [{"id": "CVE-example"}]}
        return report, sbom

    def test_findings_remain_visible_but_missing_scan_coverage_fails(self):
        report, sbom = self.scan()
        result = audit.scanner_coverage(report, sbom, "server", self.context, "sha256:config", self.expected_os)
        self.assertEqual(result["vulnerabilities_by_severity"], {"HIGH": 1})
        self.assertFalse(result["publication_permitted"])
        for missing in ("Packages", "Licenses"):
            altered = copy.deepcopy(report)
            del altered["Results"][0][missing]
            with self.assertRaises(ValueError):
                audit.scanner_coverage(altered, sbom, "server", self.context, "sha256:config", self.expected_os)
        for missing in ("components", "vulnerabilities"):
            altered = copy.deepcopy(sbom)
            del altered[missing]
            with self.assertRaises(ValueError):
                audit.scanner_coverage(report, altered, "server", self.context, "sha256:config", self.expected_os)
        with self.assertRaisesRegex(ValueError, "configuration"):
            audit.scanner_coverage(report, sbom, "server", self.context, "sha256:other", self.expected_os)

    def test_one_missing_runtime_os_package_fails_despite_many_other_packages(self):
        report, sbom = self.scan()
        report["Results"][0]["Packages"] = [p for p in report["Results"][0]["Packages"] if p["Name"] != "libpq5"]
        self.assertGreater(len(report["Results"][0]["Packages"]), 20)
        with self.assertRaisesRegex(ValueError, "libpq5"):
            audit.scanner_coverage(report, sbom, "server", self.context, "sha256:config", self.expected_os)

    def test_buildkit_output_metadata_must_identify_the_image_index(self):
        path = self.context / "server.build.json"
        build = {"containerimage.digest": "sha256:index", "containerimage.descriptor": {
            "digest": "sha256:index", "mediaType": "application/vnd.oci.image.index.v1+json"}}
        path.write_text(json.dumps(build))
        metadata = {"index_digest": "sha256:index", "config_digest": "sha256:config"}
        audit.build_digest_bindings(self.context, "server", metadata)
        build["containerimage.digest"] = "sha256:config"
        path.write_text(json.dumps(build))
        with self.assertRaisesRegex(ValueError, "BuildKit"):
            audit.build_digest_bindings(self.context, "server", metadata)

    def test_single_field_report_mutations_do_not_pass_internal_evidence_bindings(self):
        database = self.context / "cache/db"
        database.mkdir(parents=True)
        (database / "trivy.db").write_bytes(b"fixed advisory bytes")
        (database / "metadata.json").write_text('{"UpdatedAt":"2026-10-28"}')
        image = oci.Image()
        prefix = oci.SOURCE + "validator/.deps/pkhex/PKHeX.Core/Resources/"
        resources = {}
        for i in range(1731):
            value = {"size": 1, "sha256": oci.sha256(bytes([i % 256]))}
            image.files[prefix + f"text/{i}.txt"] = dict(value, type="file")
            resources[f"PKHeX.Core.Resources.text.{i}.txt"] = value
        image.files["app/PKHeX.Core.dll"] = {"sha256": "assembly hash"}
        audit.write_json(self.context / "validator.resources.json", {"count": 1731, "assembly_sha256": "assembly hash", "resources": resources})
        report = {"source_commit": "commit", "advisory_database": {name: audit.digest_file(database / name) for name in ("trivy.db", "metadata.json")},
                  "advisory_metadata": {"UpdatedAt": "2026-10-28"}, "images": {"validator": {"embedded_resources": {"count": 1731, "assembly_sha256": "assembly hash"}}}}
        metadata = {"config": {"Labels": {"org.opencontainers.image.revision": "commit"}}}
        audit.evidence_bindings(self.context, report, "validator", image, metadata)
        for field in ("commit", "database", "metadata", "resources"):
            altered = copy.deepcopy(report)
            if field == "commit":
                altered["source_commit"] = "different commit"
            elif field == "database":
                altered["advisory_database"]["trivy.db"] = "different hash"
            elif field == "metadata":
                altered["advisory_metadata"]["UpdatedAt"] = "different date"
            else:
                altered["images"]["validator"]["embedded_resources"]["count"] = 0
            with self.subTest(field=field), self.assertRaises(ValueError):
                audit.evidence_bindings(self.context, altered, "validator", image, metadata)

    def test_dotnet_runtime_coverage_cannot_be_replaced_by_source_lockfile_results(self):
        (self.context / "validator").mkdir()
        (self.context / "validator/packages.lock.json").write_text(json.dumps({"dependencies": {"net10.0": {
            "Google.Protobuf": {"type": "Direct", "resolved": "3.36.2"}}}}))
        report, sbom = self.scan()
        report["Results"].append({"Target": "source/validator/packages.lock.json", "Packages": [{"Name": "Google.Protobuf", "Version": "3.36.2"}]})
        sbom["components"].append({"name": "Google.Protobuf", "version": "3.36.2"})
        with self.assertRaisesRegex(ValueError, "runtime packages"):
            audit.scanner_coverage(report, sbom, "validator", self.context, "sha256:config", self.expected_os)
        report["Results"][-1]["Target"] = "app/Validator.deps.json"
        result = audit.scanner_coverage(report, sbom, "validator", self.context, "sha256:config", self.expected_os)
        self.assertEqual(result["dotnet_runtime_packages"], 1)


if __name__ == "__main__":
    unittest.main()
