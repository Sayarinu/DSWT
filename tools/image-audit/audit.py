#!/usr/bin/env python3
"""Build and inspect private local OCI images. Never publish or reset a workspace."""

import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tarfile
import urllib.request
import uuid

import oci


ROOT = Path(__file__).resolve().parents[2]
PINS = Path(__file__).with_name("pins.json")
SERVICES = ("server", "validator")


def digest_file(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def write_json(path, data):
    with path.open("x") as output:
        json.dump(data, output, indent=2, sort_keys=True)
        output.write("\n")


def command(args, **kwargs):
    return subprocess.run(args, text=True, check=True, **kwargs)


def git(*args):
    return command(["git", "-C", str(ROOT), *args], stdout=subprocess.PIPE).stdout.strip()


def context_files(root, service, tracked):
    rules = (root / service / "Dockerfile.dockerignore").read_text().splitlines()
    if [r for r in rules if r and not r.startswith("#")][0] != "**":
        raise ValueError("Service context must deny all files first")
    included = set()
    for rule in rules:
        if not rule or rule.startswith("#") or rule == "**":
            continue
        if not rule.startswith("!") or rule.endswith("/") or any(c in rule for c in "*?[]"):
            raise ValueError("Use literal service context allowances")
        path = oci.safe_path(rule[1:].rstrip("/"))
        if not rule.endswith("/"):
            included.add(path)
    expected = {p for p in tracked if p.startswith(service + "/") and (root / p).exists()}
    expected.update(("LICENSE", "NOTICE"))
    if service == "validator":
        expected.add("server/proto/trade.proto")
    if included != expected:
        raise ValueError(f"Update {service}/Dockerfile.dockerignore for reviewed tracked files: "
                         f"missing={sorted(expected - included)}, unexpected={sorted(included - expected)}")
    for name in included:
        path = root / name
        info = path.lstat()
        if (path.resolve() != path.absolute() or not stat.S_ISREG(info.st_mode) or info.st_nlink != 1
                or oci.prohibited(name)):
            raise ValueError("Build inputs must be ordinary unlinked source files: " + name)
    return sorted(included)


def stage_context(root, destination, service, tracked):
    inventory = {}
    for name in context_files(root, service, tracked):
        source, target = root / name, destination / name
        target.parent.mkdir(parents=True, exist_ok=True)
        data = source.read_bytes()
        if oci.PRIVATE_SENTINEL in data:
            raise ValueError("Private sentinel in approved source input")
        target.write_bytes(data)
        # The private workspace protects host inputs; image notices stay readable.
        target.chmod(0o755 if source.stat().st_mode & 0o111 else 0o644)
        inventory[name] = {"sha256": oci.sha256(data), "size": len(data)}
    for path in destination.rglob("*"):
        if path.is_dir():
            path.chmod(0o755)
    return inventory


def audit_sources(image, context, service, pkhex_archive=None):
    expected = {}
    if service == "server":
        for name in ("LICENSE", "NOTICE"):
            expected["usr/share/platinumwt/" + name] = (context / name).read_bytes()
        for path in (context / "server/migrations").iterdir():
            expected["usr/local/share/platinumwt/migrations/" + path.name] = path.read_bytes()
    else:
        for path in context.rglob("*"):
            if path.is_file():
                expected[oci.SOURCE + path.relative_to(context).as_posix()] = path.read_bytes()
        pin = dict(line.split("=", 1) for line in (context / "validator/upstream.env").read_text().splitlines())
        if digest_file(pkhex_archive) != pin["PKHEX_ARCHIVE_SHA256"]:
            raise ValueError("Pinned PKHeX source archive hash mismatch")
        prefix = "PKHeX-" + pin["PKHEX_REVISION"] + "/"
        with tarfile.open(pkhex_archive, "r:gz") as archive:
            for entry in archive:
                path = oci.safe_path(entry.name)
                if entry.isdir() and path == prefix.rstrip("/"):
                    continue
                if not path.startswith(prefix):
                    raise ValueError("Unexpected PKHeX source archive root")
                relative = path[len(prefix):]
                if not (relative in ("LICENSE", "Directory.Build.props") or relative.startswith("PKHeX.Core/")):
                    continue
                if entry.isdir():
                    continue
                if not entry.isfile():
                    raise ValueError("Linked PKHeX source file")
                expected[oci.SOURCE + "validator/.deps/pkhex/" + relative] = archive.extractfile(entry).read()
        for line in (context / "validator/runtime-sources.tsv").read_text().splitlines():
            if not line or line.startswith("#"):
                continue
            name, _, revision, digest, _ = line.split()
            path = oci.SOURCE + f"validator/.deps/runtime-source/{name}-{revision}.tar.gz"
            if oci.sha256(image.data(path)) != digest:
                raise ValueError("Runtime source archive mismatch: " + name)
            expected[path] = image.data(path)
        packaged = {p for p, item in image.files.items() if p.startswith(oci.SOURCE) and item["type"] != "directory"}
        if packaged != set(expected):
            raise ValueError("Packaged corresponding source differs from exact approved inputs")
    for path, data in expected.items():
        if image.data(path) != data:
            raise ValueError("Missing or changed packaged source/notice: " + path)
    notices = {}
    for package, _ in installed_packages(image):
        path = "usr/share/doc/" + package + "/copyright"
        resolved, entry = image.resolve_file(path)
        notices[path] = {"resolved": resolved, "sha256": entry["sha256"]}
    if service == "validator":
        for path in ("usr/share/dotnet/LICENSE.txt", "usr/share/dotnet/ThirdPartyNotices.txt"):
            if image.files.get(path, {}).get("type") != "file":
                raise ValueError("Missing .NET runtime notice: " + path)
            notices[path] = {"resolved": path, "sha256": image.files[path]["sha256"]}
    return {"verified_source_files": len(expected), "source_inventory": {p: oci.sha256(data) for p, data in sorted(expected.items())},
            "base_notices": notices}


def installed_packages(image):
    packages = set()
    for paragraph in image.data("var/lib/dpkg/status").decode().split("\n\n"):
        fields = dict(line.split(": ", 1) for line in paragraph.splitlines() if ": " in line and not line.startswith(" "))
        if fields.get("Status", "").endswith(" ok installed"):
            packages.add((fields["Package"], fields["Version"]))
    if not any(name == "libc6" for name, _ in packages):
        raise ValueError("Exported image has no complete installed dpkg inventory")
    return packages


def resource_evidence(image, resources):
    prefix = oci.SOURCE + "validator/.deps/pkhex/PKHeX.Core/Resources/"
    expected = {"PKHeX.Core.Resources." + path[len(prefix):].replace("/", "."):
                {"sha256": entry["sha256"], "size": entry["size"]}
                for path, entry in image.files.items() if path.startswith(prefix) and entry["type"] == "file"}
    assembly = image.files["app/PKHeX.Core.dll"]["sha256"]
    if (len(expected) != 1731 or resources.get("resources") != expected or resources.get("count") != 1731
            or resources.get("assembly_sha256") != assembly):
        raise ValueError("Embedded resource evidence does not match the exported assembly/source")
    return {"count": 1731, "assembly_sha256": assembly}


def evidence_bindings(workspace, report, service, image, metadata):
    if metadata["config"].get("Labels", {}).get("org.opencontainers.image.revision") != report["source_commit"]:
        raise ValueError("Source commit does not match OCI revision label")
    database = {name: digest_file(workspace / "cache/db" / name) for name in ("trivy.db", "metadata.json")}
    if database != report["advisory_database"] or json.loads((workspace / "cache/db/metadata.json").read_text()) != report["advisory_metadata"]:
        raise ValueError("Advisory database summary does not match retained bytes")
    if service == "validator":
        resources = json.loads((workspace / "validator.resources.json").read_text())
        if resource_evidence(image, resources) != report["images"][service]["embedded_resources"]:
            raise ValueError("Embedded resource summary mismatch")


def build_digest_bindings(workspace, service, metadata):
    build = json.loads((workspace / (service + ".build.json")).read_text())
    descriptor = build.get("containerimage.descriptor", {})
    if (build.get("containerimage.digest") != metadata["index_digest"] or descriptor.get("digest") != metadata["index_digest"]
            or descriptor.get("mediaType") != "application/vnd.oci.image.index.v1+json"
            or ("containerimage.config.digest" in build and build["containerimage.config.digest"] != metadata["config_digest"])):
        raise ValueError("Exported image digests differ from BuildKit output metadata")


def scanner_coverage(report, sbom, service, context, config_digest, expected_os):
    if report.get("SchemaVersion") != 2 or report.get("ArtifactType") != "container_image":
        raise ValueError("Unsupported or empty scanner result")
    if report.get("Metadata", {}).get("ImageID") != config_digest:
        raise ValueError("Scanner image ID does not bind the audited OCI configuration")
    results = report.get("Results", [])
    os_packages = [p for r in results if r.get("Class") == "os-pkgs" for p in r.get("Packages", [])]
    actual_os = {(p["Name"], p["Version"]) for p in os_packages}
    if not expected_os or actual_os != expected_os:
        raise ValueError("Scanner OS inventory differs from installed dpkg packages: "
                         f"missing={sorted(expected_os - actual_os)}, unexpected={sorted(actual_os - expected_os)}")
    runtime = [p for r in results if r.get("Target", "").lstrip("/") == "app/Validator.deps.json"
               for p in r.get("Packages", [])]
    if service == "validator":
        actual = {(p["Name"], p["Version"]) for p in runtime}
        locked = json.loads((context / "validator/packages.lock.json").read_text())["dependencies"]["net10.0"]
        required = {(name, value["resolved"]) for name, value in locked.items()
                    if value["type"] != "Project" and name != "Grpc.Tools"}
        if not required.issubset(actual):
            raise ValueError("Scanner missed locked .NET runtime packages: " + str(sorted(required - actual)))
    components = {(p.get("name"), p.get("version")) for p in sbom.get("components", [])}
    if sbom.get("bomFormat") != "CycloneDX" or not components:
        raise ValueError("Missing CycloneDX component coverage")
    for package in os_packages + runtime:
        if (package["Name"], package["Version"]) not in components:
            raise ValueError("SBOM omitted a scanned OS/runtime package: " + package["Name"])
    licenses = [license for r in results for license in r.get("Licenses", [])]
    if not licenses:
        raise ValueError("License inventory is empty; do not interpret it as clearance")
    vulnerabilities = [v for r in results for v in r.get("Vulnerabilities", [])]
    if {v["VulnerabilityID"] for v in vulnerabilities} != {v["id"] for v in sbom.get("vulnerabilities", [])}:
        raise ValueError("SBOM vulnerability coverage differs from the explicit vulnerability scan")
    return {"os_packages": len(os_packages), "dotnet_runtime_packages": len(runtime),
            "sbom_components": len(components), "license_records": len(licenses),
            "vulnerabilities_by_severity": dict(Counter(v["Severity"] for v in vulnerabilities)),
            "license_limitation": ".NET deps.json has no license detection; exact retained notices/source are verified separately",
            "publication_permitted": False}


class Runner:
    def __init__(self, workspace, platform):
        self.workspace, self.platform = workspace, platform
        self.pins = json.loads(PINS.read_text())
        self.name = "platinumwt-audit-" + uuid.uuid4().hex
        self.env = {k: v for k, v in os.environ.items() if not k.startswith(("DOCKER_", "BUILDX_", "BUILDKIT_"))}
        endpoint = os.environ.get("DOCKER_HOST") if not os.environ.get("DOCKER_CONTEXT") else None
        if not endpoint:
            context = json.loads(command(["docker", "context", "inspect"], stdout=subprocess.PIPE).stdout)
            endpoint = context[0]["Endpoints"]["docker"]["Host"]
        if not endpoint.startswith("unix:///"):
            raise ValueError("Image audit requires a local Unix Docker socket")
        config = workspace / "docker-config"
        config.mkdir()
        write_json(config / "config.json", {})  # No package credentials or helper configuration.
        plugin = Path(os.environ.get("DOCKER_CONFIG", Path.home() / ".docker")) / "cli-plugins/docker-buildx"
        if plugin.is_file():
            (config / "cli-plugins").mkdir()
            (config / "cli-plugins/docker-buildx").symlink_to(plugin.resolve())
        self.docker = ["docker", "--config", str(config), "--host", endpoint]
        self.owned_builder = False
        self.test_image = self.name + "-validator-tests"

    def run(self, args, log, *, capture=False):
        print(log, flush=True)
        with (self.workspace / (log + ".log")).open("x") as output:
            result = command(self.docker + args, env=self.env, stdout=subprocess.PIPE if capture else output, stderr=output)
            return result.stdout if capture else None

    def build(self, service, *, test=False):
        target = service + ("-tests" if test else "")
        args = ["buildx", "build", "--builder", self.name, "--platform", self.platform,
                "--file", str(self.workspace / "contexts" / service / service / "Dockerfile"),
                "--provenance=mode=max", "--sbom=false", "--progress=plain",
                "--label", "org.opencontainers.image.revision=" + git("rev-parse", "HEAD")]
        if test:
            args += ["--target", "validator-tests", "--load", "--tag", self.test_image]
        else:
            args += ["--tag", self.name + "-" + service + ":local",
                     "--output", f"type=oci,dest={self.workspace / (service + '.oci.tar')},compression=gzip,force-compression=true",
                     "--metadata-file", str(self.workspace / (service + ".build.json"))]
        self.run(args + [str(self.workspace / "contexts" / service)], "build-" + target)

    def prove_context(self, service, expected):
        context = self.workspace / "context-probes" / service
        shutil.copytree(self.workspace / "contexts" / service, context)
        sentinels = ["game/private.nds", service + "/.env", service + "/unlisted.txt",
                     "server/src/unexpected_private.cpp" if service == "server" else "validator/UnexpectedPrivate.cs",
                     "server/tests/unit/unexpected_private.cpp" if service == "server" else "validator/tests/UnexpectedPrivate.cs"]
        for name in sentinels:
            path = context / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(oci.PRIVATE_SENTINEL)
        (context / "Dockerfile").write_text("FROM scratch\nCOPY . /context/\n")
        shutil.copyfile(context / service / "Dockerfile.dockerignore", context / "Dockerfile.dockerignore")
        exported = self.workspace / (service + ".context.tar")
        self.run(["buildx", "build", "--builder", self.name, "--platform", self.platform, "--provenance=false",
                  "--output", "type=tar,dest=" + str(exported), str(context)], "prove-context-" + service)
        actual = {}
        with tarfile.open(exported, "r:") as archive:
            for entry in archive:
                if entry.isdir():
                    continue
                path = oci.safe_path(entry.name)
                if not entry.isfile() or not path.startswith("context/") or path[8:] in actual:
                    raise ValueError("Unexpected file in real Docker context export")
                data = archive.extractfile(entry).read()
                actual[path[8:]] = {"sha256": oci.sha256(data), "size": len(data)}
        if actual != expected:
            raise ValueError("Real Docker context leaked or omitted files; inspect " + str(exported))
        return {"verified_files": len(actual), "excluded_sentinels": sentinels}

    def scanner(self, arguments, log, *, offline=True, output=None):
        args = ["run", "--rm", "--name", self.name + "-scanner", "--platform", self.platform,
                "--user", f"{os.getuid()}:{os.getgid()}", "--cpus", "2", "--memory", "2g", "--pids-limit", "128",
                "--read-only", "--cap-drop", "ALL", "--security-opt", "no-new-privileges",
                "--network", "none" if offline else "bridge", "--tmpfs", "/tmp:rw,nosuid,nodev,size=768m"]
        if offline:
            args += ["--mount", f"type=bind,src={self.workspace / 'cache/db'},dst=/tmp/trivy/db,readonly",
                     "--mount", f"type=bind,src={self.workspace},dst=/audit,readonly"]
        else:
            args += ["--mount", f"type=bind,src={self.workspace / 'cache'},dst=/tmp/trivy"]
        args += [self.pins["trivy"]["image"], "--cache-dir", "/tmp/trivy", *arguments]
        result = self.run(args, log, capture=output is not None)
        if output:
            (self.workspace / output).write_text(result)

    def execute(self, report):
        buildkit_config = self.workspace / "buildkitd.toml"
        buildkit_config.write_text('[worker.oci]\n  max-parallelism = 2\n')
        self.owned_builder = True
        self.run(["buildx", "create", "--name", self.name, "--driver", "docker-container",
                  "--driver-opt", "image=" + self.pins["buildkit"]["image"],
                  "--driver-opt", "memory=4g", "--driver-opt", "cpu-period=100000", "--driver-opt", "cpu-quota=200000",
                  "--buildkitd-config", str(buildkit_config), "--bootstrap"], "create-builder")
        report["context_proofs"] = {service: self.prove_context(service, report["contexts"][service]) for service in SERVICES}
        for service in SERVICES:
            self.build(service)
        self.build("validator", test=True)
        self.cleanup()  # Release builder memory before the scanner or resource inspector runs.
        (self.workspace / "cache").mkdir()
        self.scanner(["image", "--download-db-only", "--no-progress", "--db-repository", self.pins["advisory_repository"]],
                     "download-advisories", offline=False)
        report["advisory_database"] = {name: digest_file(self.workspace / "cache/db" / name)
                                       for name in ("trivy.db", "metadata.json")}
        report["advisory_metadata"] = json.loads((self.workspace / "cache/db/metadata.json").read_text())
        for service in SERVICES:
            archive = oci.Archive(self.workspace / (service + ".oci.tar"))
            try:
                image, metadata = archive.inspect(self.platform)
            finally:
                archive.close()
            build_digest_bindings(self.workspace, service, metadata)
            context = self.workspace / "contexts" / service
            metadata["source"] = audit_sources(image, context, service, self.workspace / "pkhex-source.tar.gz")
            write_json(self.workspace / (service + ".filesystem.json"), image.inventory())
            if service == "validator":
                inspection = self.workspace / "inspection"
                for path, entry in image.files.items():
                    if "data" in entry:
                        target = inspection / path
                        target.parent.mkdir(parents=True, exist_ok=True)
                        target.write_bytes(entry["data"])
                result = self.run(["run", "--rm", "--name", self.name + "-resources", "--platform", self.platform,
                                   "--user", f"{os.getuid()}:{os.getgid()}", "--network", "none", "--read-only",
                                   "--cap-drop", "ALL", "--security-opt", "no-new-privileges", "--cpus", "2", "--memory", "512m",
                                   "--pids-limit", "128", "--tmpfs", "/tmp:rw,noexec,nosuid,nodev,size=64m",
                                   "--mount", f"type=bind,src={inspection},dst=/inspection,readonly",
                                   self.test_image, "--audit-image", "/inspection"], "audit-embedded-resources", capture=True)
                resources = json.loads(result)
                write_json(self.workspace / "validator.resources.json", resources)
                metadata["embedded_resources"] = resource_evidence(image, resources)
            shared = ["image", "--input", "/audit/" + service + ".oci.tar", "--scanners", "vuln,license", "--license-full",
                      "--list-all-pkgs", "--skip-db-update", "--skip-java-db-update", "--offline-scan", "--no-progress"]
            self.scanner(shared + ["--format", "json"], "scan-" + service, output=service + ".trivy.json")
            self.scanner(shared + ["--format", "cyclonedx"], "sbom-" + service, output=service + ".cdx.json")
            metadata["scan"] = scanner_coverage(json.loads((self.workspace / (service + ".trivy.json")).read_text()),
                                                 json.loads((self.workspace / (service + ".cdx.json")).read_text()), service, context,
                                                 metadata["config_digest"], installed_packages(image))
            if report["advisory_database"] != {n: digest_file(self.workspace / "cache/db" / n) for n in report["advisory_database"]}:
                raise ValueError("Advisory database changed between offline scans")
            report["images"][service] = metadata
            evidence_bindings(self.workspace, report, service, image, metadata)

    def cleanup(self):
        if self.owned_builder:
            self.run(["buildx", "rm", "--force", self.name], "remove-owned-builder")
            self.owned_builder = False


def workspace_path(value):
    path = Path(value).absolute()
    if path != path.resolve() or not path.is_relative_to(ROOT / "game") or path == ROOT / "game":
        raise ValueError("Use a new private workspace below ignored game/ without symlinks")
    return path


def verify(workspace):
    report = json.loads((workspace / "report.json").read_text())
    if report.get("status") != "complete" or report.get("publication_permitted") is not False:
        raise ValueError("Audit did not complete; preserve and inspect its failure report")
    for name, expected in report["artifacts"].items():
        path = workspace / oci.safe_path(name)
        if path.is_symlink() or not path.is_file() or path.stat().st_size != expected["size"] or digest_file(path) != expected["sha256"]:
            raise ValueError("Audit artifact changed: " + name)
    for service in SERVICES:
        archive = oci.Archive(workspace / (service + ".oci.tar"))
        try:
            image, metadata = archive.inspect(report["platform"])
        finally:
            archive.close()
        recorded = report["images"][service]
        build_digest_bindings(workspace, service, metadata)
        evidence_bindings(workspace, report, service, image, metadata)
        for key in ("layout_index_digest", "index_digest", "manifest_digest", "config_digest", "layers", "provenance", "config"):
            if metadata[key] != recorded[key]:
                raise ValueError("OCI report binding mismatch: " + key)
        if image.inventory() != json.loads((workspace / (service + ".filesystem.json")).read_text()):
            raise ValueError("Final filesystem report mismatch")
        context = workspace / "contexts" / service
        if audit_sources(image, context, service, workspace / "pkhex-source.tar.gz") != recorded["source"]:
            raise ValueError("Source evidence mismatch")
        coverage = scanner_coverage(json.loads((workspace / (service + ".trivy.json")).read_text()),
                                    json.loads((workspace / (service + ".cdx.json")).read_text()), service, context,
                                    metadata["config_digest"], installed_packages(image))
        if coverage != recorded["scan"]:
            raise ValueError("Scanner coverage report mismatch")
    print("Verified OCI bytes, all layers, final files, source/notices and digest-bound reports; publication remains blocked.")


def main():
    if sys.version_info < (3, 12):
        raise ValueError("Python 3.12 or newer is required; use a separately installed python3.12 without changing system Python")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("run", "verify"))
    parser.add_argument("--workspace", required=True)
    parser.add_argument("--platform", choices=("linux/arm64", "linux/amd64"), default="linux/arm64")
    args = parser.parse_args()
    os.umask(0o077)
    workspace = workspace_path(args.workspace)
    if args.command == "verify":
        verify(workspace)
        return
    workspace.parent.mkdir(parents=True, exist_ok=True)
    workspace.mkdir(mode=0o700)
    report = {"version": 1, "status": "failed", "publication_permitted": False, "platform": args.platform,
              "source_commit": git("rev-parse", "HEAD"), "source_dirty": bool(git("status", "--porcelain")),
              "pins": json.loads(PINS.read_text()), "contexts": {}, "images": {}}
    runner = None
    try:
        tracked = set(git("ls-files").splitlines())
        for service in SERVICES:
            report["contexts"][service] = stage_context(ROOT, workspace / "contexts" / service, service, tracked)
        pin = dict(line.split("=", 1) for line in (ROOT / "validator/upstream.env").read_text().splitlines())
        url = "https://codeload.github.com/kwsch/PKHeX/tar.gz/" + pin["PKHEX_REVISION"]
        with urllib.request.urlopen(url, timeout=120) as source, (workspace / "pkhex-source.tar.gz").open("xb") as target:
            shutil.copyfileobj(source, target)
        if digest_file(workspace / "pkhex-source.tar.gz") != pin["PKHEX_ARCHIVE_SHA256"]:
            raise ValueError("PKHeX reference download failed pinned digest check")
        runner = Runner(workspace, args.platform)
        report["owned_builder"] = runner.name
        runner.execute(report)
        report["status"] = "complete"
    except Exception as error:
        report["error"] = str(error)
        raise
    finally:
        try:
            if runner:
                runner.cleanup()
        except Exception as error:
            report["status"], report["cleanup_error"] = "failed", str(error)
            raise
        finally:
            report["artifacts"] = {p.relative_to(workspace).as_posix(): {"size": p.stat().st_size, "sha256": digest_file(p)}
                                   for p in workspace.rglob("*") if p.is_file() and not p.is_symlink()
                                   and not p.is_relative_to(workspace / "docker-config")}
            write_json(workspace / "report.json", report)
    print(json.dumps({name: value["scan"] for name, value in report["images"].items()}, indent=2))
    print("Private audit completed; publication remains blocked. Report: " + str(workspace / "report.json"))


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, subprocess.SubprocessError, tarfile.TarError, KeyError) as error:
        print("Image audit failed: " + str(error), file=sys.stderr)
        sys.exit(1)
