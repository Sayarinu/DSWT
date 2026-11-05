#!/usr/bin/env python3
"""Assemble and verify a deterministic private US Rev 0/1 candidate archive.

The archive holds only the bundle's BPS patches and manifest plus explicitly
listed tracked instructions and notices. ROMs, saves, SDKs, build intermediates
and the bundle's raw literal audit files are never included. Publication stays
blocked; the archive is written only below the ignored game/ directory.
"""

import argparse
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import signal
import subprocess
import sys
import tarfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / "tools/bps-patch"))
sys.path.insert(0, str(ROOT / "tools/image-audit"))
import bps
import oci
import patcher

CONTENTS = HERE / "contents.json"
KIND = "dswt-private-candidate"
MANIFEST = "CANDIDATE.json"
MAX_DOCUMENT = 1024 * 1024
MAX_ARCHIVE = 256 * 1024 * 1024
BLOCKERS = patcher.BLOCKERS + [
    "Agent 1's exact-candidate local emulator rehearsal (LUNE-323) and installation documentation (LUNE-292)",
    "Owner review of compiler/SDK use and patch literal/resource rights; automated inventory grants no rights",
]


class CandidateError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise CandidateError(message)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def canonical_json(value):
    return (json.dumps(value, indent=2, sort_keys=True) + "\n").encode()


def member_name(prefix, relative):
    """Every member is one safe ordinary path below the candidate prefix."""
    name = oci.safe_path(f"{prefix}/{relative}")
    require(name == f"{prefix}/{relative}" and not oci.prohibited(name), "Unsafe or prohibited candidate path: " + relative)
    return name


def tracked(path, root=ROOT):
    """Read a committed regular file's blob from HEAD itself, so the working tree cannot race the check."""
    relative = oci.safe_path(path)
    listed = subprocess.run(["git", "-C", str(root), "ls-tree", "-z", "HEAD", "--", relative], capture_output=True)
    fields = listed.stdout.rstrip(b"\0").split(b"\t", 1)
    require(listed.returncode == 0 and len(fields) == 2 and fields[1] == relative.encode()
            and fields[0].split()[:2] == [b"100644", b"blob"], f"{relative} must be a tracked regular file")
    oid = fields[0].split()[2].decode()
    size = int(subprocess.run(["git", "-C", str(root), "cat-file", "-s", oid], capture_output=True, text=True, check=True).stdout)
    require(size <= MAX_DOCUMENT, f"{relative} exceeds {MAX_DOCUMENT} bytes")
    return subprocess.run(["git", "-C", str(root), "cat-file", "blob", oid], capture_output=True, check=True).stdout


def render(members):
    """A canonical ustar stream: sorted names, fixed owners/times/modes, no directories or links."""
    output = io.BytesIO()
    with tarfile.open(fileobj=output, mode="w", format=tarfile.USTAR_FORMAT) as archive:
        for name in sorted(members):
            info = tarfile.TarInfo(name)
            info.size, info.mode, info.mtime = len(members[name]), 0o644, 0
            info.uid = info.gid = 0
            info.uname = info.gname = ""
            archive.addfile(info, io.BytesIO(members[name]))
    return output.getvalue()


def bundle_files(bundle):
    """Load the private BPS bundle; keep its manifest and patches, never its raw literal audits."""
    manifest_bytes = patcher.read_private(bundle / "manifest.json", patcher.MAX_MANIFEST, "Bundle manifest")[0]
    manifest = patcher.load_manifest(manifest_bytes)
    files = {"bundle/manifest.json": manifest_bytes}
    for entry in manifest["entries"]:
        patch = patcher.read_private(bundle / entry["patch"]["file"], bps.MAX_PATCH, entry["patch"]["file"])[0]
        check_patch(entry, patch)
        files["bundle/" + entry["patch"]["file"]] = patch
    return manifest, files


def check_patch(entry, patch):
    report = patcher.walk(patch, entry["patch"]["file"])
    require(sha256(patch) == entry["patch"]["sha256"] and len(patch) == entry["patch"]["size"]
            and report["source"] == {"size": entry["input"]["size"], "crc32": entry["input"]["crc32"]}
            and report["target"] == {"size": entry["output"]["size"], "crc32": entry["output"]["crc32"]}
            and report["actions"] == entry["actions"], f"{entry['patch']['file']} does not match its manifest entry")


def assemble(bundle, output, contents=CONTENTS, root=ROOT):
    source = patcher.source_state(root)
    require(source["dirty"] is False, "Assemble only from a clean, committed source tree")
    manifest, files = bundle_files(Path(bundle))
    require(manifest["source"]["commit"] == source["commit"], "The bundle was generated from a different source commit")
    listing = json.loads(tracked(Path(contents).relative_to(root).as_posix(), root))
    for role in ("instructions", "notices"):
        for path in listing[role]:
            files[f"{role}/{path}"] = tracked(path, root)
    prefix = "dswt-" + manifest["version"]
    inventory = [{"path": relative, "size": len(data), "sha256": sha256(data)} for relative, data in sorted(files.items())]
    record = {
        "schema_version": 1, "kind": KIND, "version": manifest["version"],
        "source": {"commit": source["commit"], "dirty": False},
        "upstream": manifest["upstream"], "connection_profile_sha256": manifest["connection_profile_sha256"],
        "bundle_manifest_sha256": sha256(files["bundle/manifest.json"]),
        "supported_subset": manifest["supported_subset"], "files": inventory,
        "publication_permitted": False, "distribution_blocked": True, "distribution_blockers": BLOCKERS,
    }
    members = {member_name(prefix, relative): data for relative, data in files.items()}
    members[member_name(prefix, MANIFEST)] = canonical_json(record)
    archive = render(members)
    parent, name = patcher._new_output(output, root)
    require(name.endswith(".tar"), "Name the candidate archive *.tar")
    with patcher.Stage(parent, f"{name}.partial") as stage:
        stage.write("candidate.tar", archive, 0o600)
        stage.publish("candidate.tar", name)
    return {"archive": str(parent / name), "sha256": sha256(archive), "size": len(archive), "manifest": record}


def verify(path, contents=CONTENTS, root=ROOT):
    """Reject any non-canonical, extra, missing, unsafe, tampered or publication-enabled candidate."""
    try:
        return _verify(path, contents, root)
    except (KeyError, TypeError, AttributeError, json.JSONDecodeError) as error:
        raise CandidateError(f"Malformed candidate manifest: {type(error).__name__}") from None


def _verify(path, contents, root):
    data = patcher.read_private(path, MAX_ARCHIVE, "Candidate archive")[0]
    members = {}
    try:
        with tarfile.open(fileobj=io.BytesIO(data), mode="r:") as archive:
            for info in archive:
                # Plain ustar files only: sparse or PAX members could expand far beyond the archive.
                require(info.type == tarfile.REGTYPE and not info.pax_headers and info.size <= len(data)
                        and info.name not in members, "Candidate members must be unique plain regular files")
                members[info.name] = archive.extractfile(info).read()
    except tarfile.TarError as error:
        raise CandidateError(f"Unreadable candidate archive: {error}") from None
    require(render(members) == data, "Candidate archive is not in canonical form")
    prefixes = {PurePosixPath(name).parts[0] for name in members}
    require(len(prefixes) == 1, "Candidate must have one top-level directory")
    prefix = prefixes.pop()
    for name in members:
        member_name(prefix, name[len(prefix) + 1:])
    record = json.loads(members.get(f"{prefix}/{MANIFEST}", b"null"))
    require(isinstance(record, dict) and record.get("schema_version") == 1 and record.get("kind") == KIND
            and prefix == "dswt-" + str(record.get("version")), "Unsupported candidate manifest")
    require(record.get("publication_permitted") is False and record.get("distribution_blocked") is True,
            "A private candidate must stay blocked from publication and distribution")
    listed = {item["path"]: item for item in record["files"]}
    require(len(listed) == len(record["files"]), "Candidate file list repeats a path")
    require(set(listed) == {name[len(prefix) + 1:] for name in members} - {MANIFEST}, "Candidate file list differs")
    for relative, item in listed.items():
        body = members[f"{prefix}/{relative}"]
        require(len(body) == item["size"] and sha256(body) == item["sha256"], f"{relative} was modified")
        require(relative.startswith(("bundle/", "instructions/", "notices/")), f"{relative} has no candidate role")
        require(relative.startswith("bundle/") or not oci.private_marker(body), f"{relative} contains private markers")
    bundle = patcher.load_manifest(members[f"{prefix}/bundle/manifest.json"])
    require(sha256(members[f"{prefix}/bundle/manifest.json"]) == record["bundle_manifest_sha256"]
            and bundle["source"] == record["source"] and bundle["version"] == record["version"]
            and bundle["connection_profile_sha256"] == record["connection_profile_sha256"]
            and bundle["upstream"] == record["upstream"] and bundle["supported_subset"] == record["supported_subset"]
            and record["distribution_blockers"] == BLOCKERS, "Bundle binding differs")
    expected = {"bundle/manifest.json"} | {"bundle/" + entry["patch"]["file"] for entry in bundle["entries"]}
    require({path for path in listed if path.startswith("bundle/")} == expected, "Bundle members differ from its manifest")
    for entry in bundle["entries"]:
        check_patch(entry, members[f"{prefix}/bundle/{entry['patch']['file']}"])
    allowed = json.loads(tracked(Path(contents).relative_to(root).as_posix(), root))
    documents = {f"{role}/{path}" for role in ("instructions", "notices") for path in allowed[role]}
    require({path for path in listed if not path.startswith("bundle/")} == documents,
            "Candidate documents differ from the committed allowlist")
    return {"sha256": sha256(data), "size": len(data), "manifest": record}


def _terminate(*_):
    raise KeyboardInterrupt


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    build = commands.add_parser("assemble", help="build a new candidate archive from a private BPS bundle")
    build.add_argument("--bundle", type=Path, required=True)
    build.add_argument("--output", type=Path, required=True, help="new *.tar path below game/")
    check = commands.add_parser("verify", help="verify an existing candidate archive without extracting it")
    check.add_argument("archive", type=Path)
    args = parser.parse_args(argv)
    signal.signal(signal.SIGTERM, _terminate)
    try:
        if args.command == "assemble":
            result = assemble(args.bundle, args.output)
            print(f"Private candidate {result['archive']} sha256={result['sha256']}; publication_permitted=false")
        else:
            result = verify(args.archive)
            print(f"Verified private candidate sha256={result['sha256']}; publication_permitted=false")
        return 0
    except KeyboardInterrupt:
        print("Interrupted; no partial candidate was published", file=sys.stderr)
        return 130
    except (CandidateError, patcher.PatchError, ValueError, KeyError, TypeError, OSError,
            subprocess.CalledProcessError) as error:
        print(f"Candidate refused: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
