#!/usr/bin/env python3
"""Audit reachable source and bind local evidence to a frozen private candidate.

`source` inventories every reachable ref, tree and blob plus a reproducible
source archive, then runs the repository's pinned secret scanner. `report`
binds candidate/source/upstream/profile and service image identities to the
observability, recovery, load, isolation and image audit reports, flagging any
evidence from a different source or image instead of claiming it applies.
Automated inventory does not establish rights; publication stays blocked.
"""

import argparse
import functools
import io
import json
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tarfile
import time
import urllib.request

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
import candidate
import oci

CLASSIFICATION = HERE / "classification.json"
WORKFLOW = ROOT / ".github/workflows/contribution.yml"
SCANNER = re.compile(r"(https://github\.com/gitleaks/gitleaks/releases/download/v([0-9.]+)/gitleaks_[0-9.]+_linux_x64\.tar\.gz)"
                     r".*?echo \"([0-9a-f]{64})  ", re.S)
# Gates this local tooling cannot close; each stays open in every readiness report.
UNRESOLVED = [
    "Actual Linux SDK/emulator run of this exact candidate (Agent 1, LUNE-323 two-save rehearsal)",
    "Final installation, compatibility and recovery instructions (Agent 1, LUNE-292)",
    "Approved VPS image and hosted authority/promotion (excluded from this local scope)",
    "Complete English matrix: Europe has no compatible target (LUNE-301); physical DS-family hardware untested",
    "Compiler/SDK use and patch literal/resource redistribution rights (owner review)",
    "Owner review of every unclassified or flagged inventory finding",
    "Publication: no GitHub release, visibility change, registry push, upload or deployment",
]


sha256 = candidate.sha256
MAX_BLOB = 256 * 1024 * 1024


def git(*arguments, root=ROOT, text=True):
    return subprocess.run(["git", "-C", str(root), *arguments], check=True, capture_output=True, text=text).stdout


def classify(path, rules):
    """Explicit path rules only; a file extension never decides rights or category."""
    if oci.prohibited(path):
        return "forbidden"
    for rule in rules:
        if path == rule["path"] or path.startswith(rule["path"].rstrip("/") + "/"):
            return rule["category"]
    return "unclassified"


def blobs(root=ROOT):
    """Every (blob, path) reachable from any ref, including other branches and remote-tracking refs."""
    listing = git("rev-list", "--objects", "--all", root=root)
    objects = [line.split(" ", 1) for line in listing.splitlines() if " " in line]
    checked = subprocess.run(["git", "-C", str(root), "cat-file", "--batch-check=%(objectname) %(objecttype) %(objectsize)"],
                             input="\n".join(oid for oid, _ in objects) + "\n", capture_output=True, text=True, check=True)
    types = {line.split()[0]: (line.split()[1], int(line.split()[2])) for line in checked.stdout.splitlines()}
    return sorted((oid, path, types[oid][1]) for oid, path in objects if types[oid][0] == "blob")


def contents(oids, root=ROOT):
    """Stream blob bodies through one cat-file process."""
    process = subprocess.Popen(["git", "-C", str(root), "cat-file", "--batch"], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    try:
        for oid in oids:
            process.stdin.write(oid.encode() + b"\n")
            process.stdin.flush()
            header = process.stdout.readline().split()
            candidate.require(len(header) == 3 and header[0] == oid.encode() and header[1] == b"blob", f"cannot read blob {oid}")
            size = int(header[2])
            candidate.require(size <= MAX_BLOB, f"blob {oid} exceeds {MAX_BLOB} bytes; review it manually")
            body = process.stdout.read(size)
            process.stdout.read(1)
            yield oid, body
    finally:
        process.stdin.close()
        process.stdout.close()
        process.wait()


@functools.cache
def policy():
    return json.loads(CLASSIFICATION.read_text())


def expected_marker(path, body, expected):
    """Only the reviewed sentinel definition is expected; encoded key material never is."""
    return path in expected and not oci.KEY_PREFIX.search(body)


def inventory(root=ROOT, rules=None, expected=None):
    rules = rules if rules is not None else policy()["rules"]
    expected = expected if expected is not None else policy()["expected_markers"]
    refs = [line.split(" ", 1) for line in git("for-each-ref", "--format=%(objectname) %(refname)", root=root).splitlines()]
    entries = blobs(root)
    paths = {}
    for oid, path, size in entries:
        paths.setdefault(path, set()).add(oid)
    bodies = dict(contents(sorted({oid for oid, _, _ in entries}), root))
    markers = sorted(oid for oid, body in bodies.items() if oci.private_marker(body))
    categories = {}
    for path in paths:
        categories.setdefault(classify(path, rules), []).append(path)
    return {
        "refs": len(refs), "ref_names": sorted(name for _, name in refs),
        "commits": int(git("rev-list", "--all", "--count", root=root)),
        "paths": len(paths), "blobs": len({oid for oid, _, _ in entries}),
        "categories": {name: len(items) for name, items in sorted(categories.items())},
        "forbidden": sorted(categories.get("forbidden", [])),
        "unclassified": sorted(categories.get("unclassified", [])),
        # A reviewed self-reference stays listed but separate; any other marker is a finding.
        "private_marker_blobs": [{"blob": oid, "paths": found,
                                  "expected": all(expected_marker(path, bodies[oid], expected) for path in found)}
                                 for oid in markers
                                 for found in [sorted(path for path, ids in paths.items() if oid in ids)]],
    }


def source_archive(commit, output, root=ROOT, expected=None):
    """A reproducible tracked-source archive, checked member by member against the commit's tree."""
    prefix = f"dswt-src-{commit[:12]}/"
    first = subprocess.run(["git", "-C", str(root), "archive", "--format=tar", f"--prefix={prefix}", commit],
                           check=True, capture_output=True).stdout
    second = subprocess.run(["git", "-C", str(root), "archive", "--format=tar", f"--prefix={prefix}", commit],
                            check=True, capture_output=True).stdout
    tree = {}
    for item in git("ls-tree", "-r", "-z", "--full-tree", commit, root=root).split("\0"):
        if item:
            meta, path = item.split("\t", 1)
            tree[path] = meta.split()[2]
    findings, archived = [], {}
    with tarfile.open(fileobj=io.BytesIO(first)) as archive:
        for info in archive:
            if info.type == tarfile.XGLTYPE:
                continue
            name = oci.safe_path(info.name)
            if info.isdir():
                continue
            relative = name[len(prefix):]
            body = archive.extractfile(info).read() if info.isreg() else b""
            # Member bytes must hash to the commit's blob, not merely share its name.
            archived[relative] = subprocess.run(["git", "hash-object", "--stdin"], input=body, capture_output=True,
                                                check=True).stdout.decode().strip() if info.isreg() else None
            if not info.isreg() or oci.prohibited(name):
                findings.append({"path": relative, "finding": "not an ordinary allowed file"})
            elif oci.private_marker(body):
                findings.append({"path": relative, "finding": "private marker", "expected": expected_marker(
                    relative, body, expected if expected is not None else policy()["expected_markers"])})
    output.write_bytes(first)
    return {"file": output.name, "sha256": sha256(first), "size": len(first), "reproducible": first == second,
            "complete": archived == tree, "members": len(archived), "findings": findings}


def scanner(tools, workflow=WORKFLOW):
    """Install the exact gitleaks release pinned by the repository's CI secret scan."""
    match = SCANNER.search(workflow.read_text())
    candidate.require(match is not None, "the CI workflow no longer pins a gitleaks release")
    url, version, digest = match.groups()
    directory = tools / f"gitleaks-{version}"
    if not directory.exists():
        tools.mkdir(parents=True, exist_ok=True)
        with urllib.request.urlopen(url, timeout=120) as response:
            archive = response.read(64 * 1024 * 1024)
        candidate.require(sha256(archive) == digest, "gitleaks archive does not match the CI pin")
        with tarfile.open(fileobj=io.BytesIO(archive)) as source:
            member = source.getmember("gitleaks")
            candidate.require(member.type == tarfile.REGTYPE, "unexpected gitleaks archive")
            binary = source.extractfile(member).read()
        # Install the whole directory by one no-replace rename; an interrupted install leaves nothing.
        with candidate.patcher.Stage(tools, f"gitleaks-{version}.partial") as stage:
            stage.write_tree("install", {"gitleaks": (binary, 0o700), "archive.sha256": (digest.encode() + b"\n", 0o600),
                                         "binary.sha256": (sha256(binary).encode() + b"\n", 0o600)})
            stage.publish("install", directory.name)
    binary = directory / "gitleaks"
    data = candidate.patcher.read_private(binary, 256 * 1024 * 1024, "gitleaks")[0]
    candidate.require((directory / "archive.sha256").read_text().strip() == digest
                      and (directory / "binary.sha256").read_text().strip() == sha256(data),
                      "installed gitleaks does not match the CI pin")
    return binary, version, digest


def secrets(binary, directory, root=ROOT):
    """Same policy as CI: default rules, no allow comments or ignore file, every reachable ref of a bare mirror."""
    config, ignore = directory / "gitleaks.toml", directory / "empty-ignore"
    config.write_text("[extend]\nuseDefault = true\n")
    ignore.touch()
    # A mirror clone copies every ref (refs/*), including other branches and remote-tracking refs.
    mirror = directory / "history.git"
    subprocess.run(["git", "clone", "--quiet", "--mirror", "--no-local", str(root), str(mirror)], check=True)
    candidate.require(git("for-each-ref", "--format=%(objectname) %(refname)", root=mirror)
                      == git("for-each-ref", "--format=%(objectname) %(refname)", root=root), "mirror omitted refs")
    report = directory / "gitleaks-git.json"
    process = subprocess.run([str(binary), "git", "--log-opts=--all", "--redact", "--no-banner", "--ignore-gitleaks-allow",
                              "--config", str(config), "--gitleaks-ignore-path", str(ignore), "--report-format", "json",
                              "--report-path", str(report), str(mirror)], capture_output=True, text=True)
    shutil.rmtree(mirror)
    # Exit 1 means findings only when the fresh report lists them; otherwise the scan itself failed.
    candidate.require(process.returncode in (0, 1) and report.exists(), "gitleaks scan failed to run")
    findings = json.loads(report.read_text() or "[]")
    candidate.require((process.returncode == 1) == bool(findings), "gitleaks exit status and report disagree")
    return {"git": {"exit": process.returncode, "findings": len(findings),
                    "rules": sorted({item.get("RuleID") for item in findings})}}


def audit_source(output, root=ROOT):
    commit = git("rev-parse", "HEAD", root=root).strip()
    candidate.require(not git("status", "--porcelain", root=root).strip(), "Audit only a clean, committed tree")
    parent, name = candidate.patcher._new_output(output, root)
    binary, version, digest = scanner(root / "game/tools")
    with candidate.patcher.Stage(parent, f"{name}.partial") as stage:
        work = stage.path / "audit"
        work.mkdir(mode=0o700)
        report = {"version": 1, "kind": "dswt-source-audit", "source_commit": commit, "source_dirty": False,
                  "audited_at": int(time.time()), "inventory": inventory(root),
                  "source_archive": source_archive(commit, work / f"source-{commit[:12]}.tar", root),
                  "secret_scanner": {"tool": "gitleaks", "version": version, "archive_sha256": digest,
                                     "policy": "CI default rules; allow comments and ignore files disabled"},
                  "secrets": secrets(binary, work, root),
                  "rights": "Inventory and classification do not establish rights or clear redistribution.",
                  "publication_permitted": False}
        (work / "report.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
        stage.publish("audit", name)
    return report


# Each required kind and the statuses its tool writes on success.
REQUIRED = {"observability": ("pass",), "recovery": ("pass",), "load": ("pass",), "isolation": ("pass",),
            "image-audit": ("complete",), "source-audit": (None,)}
OPTIONAL = {"rehearsal": ("pass",)}
STACK_SERVICES = ("database", "gateway", "backend", "validator")


def check_stack(report, expected, reasons):
    """Stack-bound evidence must come from the reference workspace, database and every service image."""
    stack = report.get("stack") or {}
    images = report.get("images") or stack.get("images")
    for key in ("project", "workspace", "database_system_id"):
        value = stack.get(key, report.get(key))
        if value is not None and value != expected["stack"][key]:
            reasons.append(f"{key} differs from the reference stack")
    if not isinstance(images, dict):
        reasons.append("records no service image identities")
        return
    for service in STACK_SERVICES:
        if images.get(service) != expected["stack"]["images"].get(service):
            reasons.append(f"{service} image differs from or is missing in the reference stack")


def evidence_binding(name, report, expected):
    """Applicable only when every recorded identity equals the candidate's; anything missing is stale."""
    kind = name.split(":", 1)[0]
    accepted = REQUIRED.get(kind) or OPTIONAL.get(kind)
    reasons = []
    if accepted is None:
        reasons.append("unknown evidence kind")
        accepted = ()
    status = report.get("status")
    if status not in accepted or (report.get("verdict") or {}).get("status", "pass") != "pass":
        reasons.append(f"report status {status!r} is not a pass for {kind}")
    if report.get("source_commit") != expected["source_commit"]:
        reasons.append(f"source {report.get('source_commit')} differs from candidate source {expected['source_commit']}")
    if report.get("source_dirty", False) is not False:
        reasons.append("generated from a dirty tree")
    notes = []
    if kind in ("observability", "recovery", "load"):
        check_stack(report, expected, reasons)
    elif kind == "isolation":
        if report.get("database_system_id") != expected["stack"]["database_system_id"]:
            reasons.append("database identity differs from the reference stack")
        notes.append("isolation records no image IDs; it binds by source commit and database identity")
    elif kind == "image-audit":
        audited = report.get("images") or {}
        if set(audited) != {"server", "validator"}:
            reasons.append("does not cover both service images")
        notes.append("audited OCI images are rebuilt by the owned builder from the same source; "
                     "their digests are not the compose stack's image IDs")
    elif kind == "source-audit":
        inventory, archive = report.get("inventory") or {}, report.get("source_archive") or {}
        if (report.get("kind") != "dswt-source-audit" or inventory.get("forbidden") or inventory.get("unclassified")
                or any(not item.get("expected") for item in inventory.get("private_marker_blobs", []))
                or not archive.get("reproducible") or not archive.get("complete")
                or any(not item.get("expected") for item in archive.get("findings", []))
                or any(scan.get("findings") for scan in (report.get("secrets") or {}).values())):
            reasons.append("has forbidden, unclassified, marker or secret findings, or an irreproducible archive")
    elif kind == "rehearsal" and report.get("candidate_sha256") != expected["candidate_sha256"]:
        reasons.append("does not name this exact candidate archive")
    return {"evidence": name, "status": "applicable" if not reasons else "stale", "reasons": reasons, "notes": notes,
            "source_commit": report.get("source_commit"), "report_status": status}


def readiness(archive, evidence, stack_manifest, extra_gates=(), root=ROOT, contents=candidate.CONTENTS):
    verified = candidate.verify(archive, contents, root)
    record = verified["manifest"]
    commit = record["source"]["commit"]
    stack = json.loads(Path(stack_manifest).read_text())
    expected = {"source_commit": commit, "stack": stack, "candidate_sha256": verified["sha256"]}
    upstream = json.loads(git("show", f"{commit}:client/upstream.json", root=root))
    profile = sha256((Path(stack["workspace"]) / stack["profile"]).read_bytes())
    identity = {
        "upstream": {"repository": upstream["repository"], "revision": upstream["revision"]} == record["upstream"],
        "connection_profile": profile == record["connection_profile_sha256"],
        "reference_stack_source": stack.get("source_commit") == commit and stack.get("source_dirty") is False,
    }
    bindings = [evidence_binding(name, json.loads(Path(path).read_text()), expected) for name, path in evidence]
    provided = {item["evidence"].split(":", 1)[0] for item in bindings}
    missing = sorted(set(REQUIRED) - provided)
    gates = list(UNRESOLVED) + list(extra_gates)
    if "rehearsal" not in provided:
        gates.insert(0, "No exact-candidate rehearsal report has been supplied")
    return {
        "version": 1, "kind": "dswt-private-readiness",
        "candidate": {"file": Path(archive).name, "sha256": verified["sha256"], "size": verified["size"],
                      "version": record["version"], "files": record["files"]},
        "source": record["source"], "upstream": record["upstream"],
        "connection_profile_sha256": record["connection_profile_sha256"],
        "bundle_manifest_sha256": record["bundle_manifest_sha256"],
        "identity_checks": identity, "service_images": {name: stack["images"].get(name) for name in STACK_SERVICES},
        "evidence": bindings, "missing_evidence": missing,
        "all_evidence_applicable": all(identity.values()) and not missing
                                   and all(item["status"] == "applicable" for item in bindings),
        "unresolved_gates": gates,
        "publication_permitted": False, "distribution_blocked": True,
        "rights": "Automated inventory and audits grant no rights.",
    }


def publish(output, report):
    """Stage and rename the sidecar without replacing anything, like the candidate archive."""
    parent, name = candidate.patcher._new_output(output, ROOT)
    with candidate.patcher.Stage(parent, f"{name}.partial") as stage:
        stage.write("report.json", (json.dumps(report, indent=2, sort_keys=True) + "\n").encode(), 0o600)
        stage.publish("report.json", name)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    source = commands.add_parser("source", help="inventory refs/trees/source archive and run the pinned secret scan")
    source.add_argument("--output", type=Path, required=True, help="new directory below game/")
    bind = commands.add_parser("report", help="write a readiness sidecar for a frozen candidate")
    bind.add_argument("--candidate", type=Path, required=True)
    bind.add_argument("--stack-manifest", type=Path, required=True, help="the reference stack's manifest.json")
    bind.add_argument("--evidence", nargs=2, action="append", default=[], metavar=("KIND[:LABEL]", "REPORT"),
                      help="kinds: " + ", ".join([*REQUIRED, *OPTIONAL]))
    bind.add_argument("--gate", action="append", default=[], help="an additional unresolved gate")
    bind.add_argument("--output", type=Path, required=True, help="new readiness JSON below game/")
    args = parser.parse_args(argv)
    signal.signal(signal.SIGTERM, candidate._terminate)
    try:
        output = args.output
        if args.command == "source":
            report = audit_source(output)
            print(json.dumps({key: report[key] for key in ("source_commit", "secrets")} |
                             {"categories": report["inventory"]["categories"],
                              "archive_sha256": report["source_archive"]["sha256"]}, indent=2))
        else:
            report = readiness(args.candidate, args.evidence, args.stack_manifest, args.gate)
            publish(output, report)
            print(f"Readiness {output}: candidate {report['candidate']['sha256']}, "
                  f"all evidence applicable={report['all_evidence_applicable']}, publication_permitted=false")
        return 0
    except KeyboardInterrupt:
        print("Interrupted; no partial output was published", file=sys.stderr)
        return 130
    except (candidate.CandidateError, candidate.patcher.PatchError, ValueError, KeyError, OSError,
            subprocess.CalledProcessError) as error:
        print(f"Readiness refused: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
