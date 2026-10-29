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
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shutil
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


def sha256(data):
    return hashlib.sha256(data).hexdigest()


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
            body = process.stdout.read(int(header[2]))
            process.stdout.read(1)
            yield oid, body
    finally:
        process.stdin.close()
        process.wait()


def inventory(root=ROOT, rules=None):
    rules = rules if rules is not None else json.loads(CLASSIFICATION.read_text())["rules"]
    refs = [line.split(" ", 1) for line in git("for-each-ref", "--format=%(objectname) %(refname)", root=root).splitlines()]
    entries = blobs(root)
    paths = {}
    for oid, path, size in entries:
        paths.setdefault(path, set()).add(oid)
    markers = sorted({oid for oid, body in contents(sorted({oid for oid, _, _ in entries}), root) if oci.private_marker(body)})
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
        "private_marker_blobs": [{"blob": oid, "paths": sorted(path for path, ids in paths.items() if oid in ids)}
                                 for oid in markers],
    }


def source_archive(commit, output, root=ROOT):
    """A reproducible tracked-source archive, checked member by member against the commit's tree."""
    prefix = f"platinumwt-src-{commit[:12]}/"
    first = subprocess.run(["git", "-C", str(root), "archive", "--format=tar", f"--prefix={prefix}", commit],
                           check=True, capture_output=True).stdout
    second = subprocess.run(["git", "-C", str(root), "archive", "--format=tar", f"--prefix={prefix}", commit],
                            check=True, capture_output=True).stdout
    tree = {line.split("\t", 1)[1]: line.split()[2] for line in git("ls-tree", "-r", "--full-tree", commit, root=root).splitlines()}
    findings, names = [], set()
    with tarfile.open(fileobj=io.BytesIO(first)) as archive:
        for info in archive:
            if info.type == tarfile.XGLTYPE:
                continue
            name = oci.safe_path(info.name)
            if info.isdir():
                continue
            relative = name[len(prefix):]
            names.add(relative)
            if not info.isreg() or oci.prohibited(name):
                findings.append({"path": relative, "finding": "not an ordinary allowed file"})
            elif oci.private_marker(archive.extractfile(info).read()):
                findings.append({"path": relative, "finding": "private marker"})
    output.write_bytes(first)
    return {"file": output.name, "sha256": sha256(first), "size": len(first), "reproducible": first == second,
            "complete": names == set(tree), "members": len(names), "findings": findings}


def scanner(tools, workflow=WORKFLOW):
    """Install the exact gitleaks release pinned by the repository's CI secret scan."""
    url, version, digest = SCANNER.search(workflow.read_text()).groups()
    directory = tools / f"gitleaks-{version}"
    binary = directory / "gitleaks"
    if not binary.exists():
        with urllib.request.urlopen(url, timeout=120) as response:
            archive = response.read(64 * 1024 * 1024)
        candidate.require(sha256(archive) == digest, "gitleaks archive does not match the CI pin")
        directory.mkdir(parents=True, mode=0o700)
        with tarfile.open(fileobj=io.BytesIO(archive)) as source:
            member = source.getmember("gitleaks")
            candidate.require(member.isreg(), "unexpected gitleaks archive")
            binary.write_bytes(source.extractfile(member).read())
        binary.chmod(0o700)
        (directory / "archive.sha256").write_text(digest + "\n")
    candidate.require((directory / "archive.sha256").read_text().strip() == digest, "installed gitleaks is not the CI pin")
    return binary, version, digest


def secrets(binary, directory, root=ROOT):
    """Same policy as CI: default rules, no allow comments or ignore file, every reachable ref of a bare mirror."""
    config, ignore = directory / "gitleaks.toml", directory / "empty-ignore"
    config.write_text("[extend]\nuseDefault = true\n")
    ignore.touch()
    mirror = directory / "history.git"
    subprocess.run(["git", "clone", "--quiet", "--mirror", "--no-local", str(root), str(mirror)], check=True)
    # Include local branches and remote-tracking refs that a mirror of the clone would otherwise omit.
    for line in git("for-each-ref", "--format=%(objectname) %(refname)", "refs/remotes", root=root).splitlines():
        oid, name = line.split(" ", 1)
        subprocess.run(["git", "-C", str(mirror), "update-ref", name, oid], check=True)
    results = {}
    for mode, target in (("git", mirror),):
        report = directory / f"gitleaks-{mode}.json"
        process = subprocess.run([str(binary), mode, "--log-opts=--all", "--redact", "--no-banner", "--ignore-gitleaks-allow",
                                  "--config", str(config), "--gitleaks-ignore-path", str(ignore), "--report-format", "json",
                                  "--report-path", str(report), str(target)], capture_output=True, text=True)
        candidate.require(process.returncode in (0, 1), f"gitleaks {mode} scan failed to run")
        findings = json.loads(report.read_text() or "[]")
        results[mode] = {"exit": process.returncode, "findings": len(findings),
                         "rules": sorted({item.get("RuleID") for item in findings})}
    shutil.rmtree(mirror)
    return results


def audit_source(output, root=ROOT):
    commit = git("rev-parse", "HEAD", root=root).strip()
    candidate.require(not git("status", "--porcelain", root=root).strip(), "Audit only a clean, committed tree")
    output.mkdir(mode=0o700)
    binary, version, digest = scanner(root / "game/tools")
    report = {"version": 1, "kind": "platinumwt-source-audit", "source_commit": commit, "audited_at": int(time.time()),
              "inventory": inventory(root), "source_archive": source_archive(commit, output / f"source-{commit[:12]}.tar", root),
              "secret_scanner": {"tool": "gitleaks", "version": version, "archive_sha256": digest,
                                 "policy": "CI default rules; allow comments and ignore files disabled"},
              "secrets": secrets(binary, output, root),
              "rights": "Inventory and classification do not establish rights or clear redistribution.",
              "publication_permitted": False}
    (output / "report.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    return report


def evidence_binding(name, report, expected):
    """Classify a report as applicable only when its source and service images equal the candidate's."""
    commit = report.get("source_commit")
    images = report.get("images") or (report.get("stack") or {}).get("images") or {}
    reasons = []
    if report.get("source_dirty"):
        reasons.append("generated from a dirty tree")
    if commit != expected["source_commit"]:
        reasons.append(f"source {commit} differs from candidate source {expected['source_commit']}")
    for service, image in sorted(images.items()):
        if service in expected["images"] and image != expected["images"][service]:
            reasons.append(f"{service} image {image} differs from the candidate stack")
    if report.get("status") not in ("pass", None) or (report.get("verdict") or {}).get("status", "pass") != "pass":
        reasons.append("the report did not pass")
    return {"evidence": name, "status": "applicable" if not reasons else "stale", "reasons": reasons,
            "source_commit": commit, "report_status": report.get("status")}


def readiness(archive, evidence, stack_manifest, extra_gates=()):
    verified = candidate.verify(archive)
    record = verified["manifest"]
    stack = json.loads(Path(stack_manifest).read_text())
    expected = {"source_commit": record["source"]["commit"], "images": stack.get("images", {})}
    bindings = [evidence_binding(name, json.loads(Path(path).read_text()), expected) for name, path in evidence]
    if stack.get("source_commit") != expected["source_commit"] or stack.get("source_dirty"):
        bindings.append({"evidence": "stack", "status": "stale",
                         "reasons": ["the reference stack was built from a different or dirty source"]})
    return {
        "version": 1, "kind": "platinumwt-private-readiness",
        "candidate": {"file": Path(archive).name, "sha256": verified["sha256"], "size": verified["size"],
                      "version": record["version"], "files": record["files"]},
        "source": record["source"], "upstream": record["upstream"],
        "connection_profile_sha256": record["connection_profile_sha256"],
        "bundle_manifest_sha256": record["bundle_manifest_sha256"],
        "service_images": expected["images"], "evidence": bindings,
        "all_evidence_applicable": all(item["status"] == "applicable" for item in bindings),
        "unresolved_gates": UNRESOLVED + list(extra_gates),
        "publication_permitted": False, "distribution_blocked": True,
        "rights": "Automated inventory and audits grant no rights.",
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    source = commands.add_parser("source", help="inventory refs/trees/source archive and run the pinned secret scan")
    source.add_argument("--output", type=Path, required=True, help="new directory below game/")
    bind = commands.add_parser("report", help="write a readiness sidecar for a frozen candidate")
    bind.add_argument("--candidate", type=Path, required=True)
    bind.add_argument("--stack-manifest", type=Path, required=True, help="the reference stack's manifest.json")
    bind.add_argument("--evidence", nargs=2, action="append", default=[], metavar=("NAME", "REPORT"))
    bind.add_argument("--gate", action="append", default=[], help="an additional unresolved gate")
    bind.add_argument("--output", type=Path, required=True, help="new readiness JSON below game/")
    args = parser.parse_args(argv)
    try:
        output = args.output.absolute()
        candidate.require(output.is_relative_to(ROOT / "game") and not os.path.lexists(output),
                          "Write a new output below the ignored game/ directory")
        if args.command == "source":
            report = audit_source(output)
            print(json.dumps({key: report[key] for key in ("source_commit", "secrets")} |
                             {"categories": report["inventory"]["categories"],
                              "archive_sha256": report["source_archive"]["sha256"]}, indent=2))
        else:
            report = readiness(args.candidate, args.evidence, args.stack_manifest, args.gate)
            output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
            print(f"Readiness {output}: candidate {report['candidate']['sha256']}, "
                  f"all evidence applicable={report['all_evidence_applicable']}, publication_permitted=false")
        return 0
    except (candidate.CandidateError, ValueError, OSError, subprocess.CalledProcessError) as error:
        print(f"Readiness refused: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
