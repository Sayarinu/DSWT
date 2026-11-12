#!/usr/bin/env python3
"""Stage and publish a GitHub release of the patch archive and player apps.

`draft` runs locally, where the ROMs are: it uploads dswt-VERSION.zip to a new draft
release tagged at the manifest's source commit. The Release patcher workflow then
runs `fetch` to verify that archive, builds the apps, and runs `publish`, which
uploads them with SHA256SUMS and publishes the draft.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import build_app as build  # noqa: E402

VERSION = re.compile(r"[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z.]+)?")
PLATFORMS = ("linux", "macos", "windows")


def gh(*arguments, capture=True):
    result = subprocess.run(["gh", *arguments], capture_output=capture, text=True)
    if result.returncode:
        raise build.BuildError(f"gh {arguments[0]} {arguments[1]} failed: {(result.stderr or '').strip()[-400:]}")
    return result.stdout


def archive_name(version):
    return f"dswt-{version}.zip"


def notes(manifest):
    rows = "\n".join(f"| {entry['name']} | `{entry['input']['sha256']}` |" for entry in manifest["entries"])
    return f"""Wonder Trade for your own copy of Pokémon Platinum.

Download the patcher for your computer, open it, choose a clean dump of your cartridge
and save the patched ROM. Trimmed dumps work; the original file is never changed.
Advanced users can apply the BPS patches in `{archive_name(manifest['version'])}` with any
BPS patcher; read its `INSTALL.md` first.

| Supported cartridge | ROM SHA-256 |
| --- | --- |
{rows}

Other versions and languages are recognised but not supported yet.
The apps are not code-signed: Windows SmartScreen and macOS Gatekeeper ask you to confirm
the first launch. `SHA256SUMS` lists every file in this release.

Source: {manifest['source']['commit']}, upstream {manifest['upstream']['revision']}.
"""


def draft(version, archive, repository):
    manifest, _ = build.read_archive(archive, version)
    if Path(archive).name != archive_name(version):
        raise build.BuildError(f"Name the archive {archive_name(version)}")
    commit = manifest["source"]["commit"]
    gh("api", f"repos/{repository}/commits/{commit}", "--silent")
    gh("release", "create", f"v{version}", str(archive), "--repo", repository, "--draft", "--target", commit,
       "--title", f"DSWT {version}", "--notes", notes(manifest))
    return {"release": f"v{version}", "commit": commit, "draft": True}


def _draft_assets(version, repository):
    release = json.loads(gh("release", "view", f"v{version}", "--repo", repository, "--json", "isDraft,assets"))
    if not release["isDraft"]:
        raise build.BuildError(f"v{version} is already published")
    return {asset["name"] for asset in release["assets"]}


def fetch(version, repository, output):
    if _draft_assets(version, repository) != {archive_name(version)}:
        raise build.BuildError(f"The draft must hold exactly {archive_name(version)}")
    Path(output).mkdir(parents=True, exist_ok=True)
    gh("release", "download", f"v{version}", "--repo", repository, "--pattern", archive_name(version), "--dir", str(output))
    manifest, _ = build.read_archive(Path(output) / archive_name(version), version)
    return {"version": version, "variants": [entry["variant"] for entry in manifest["entries"]]}


def publish(version, repository, directory):
    directory = Path(directory)
    expected = {archive_name(version), *(f"dswt-patcher-{version}-{name}.zip" for name in PLATFORMS)}
    found = {path.name for path in directory.iterdir() if path.is_file()}
    if found != expected:
        raise build.BuildError(f"Release files differ from the expected set: {sorted(found ^ expected)}")
    build.read_archive(directory / archive_name(version), version)
    if _draft_assets(version, repository) != {archive_name(version)}:
        raise build.BuildError(f"The draft must still hold only {archive_name(version)}")
    remote = directory / archive_name(version)
    sums = "".join(f"{hashlib.sha256((directory / name).read_bytes()).hexdigest()}  {name}\n" for name in sorted(expected))
    (directory / "SHA256SUMS").write_text(sums)
    apps = [str(directory / name) for name in sorted(expected - {remote.name})]
    gh("release", "upload", f"v{version}", *apps, str(directory / "SHA256SUMS"), "--repo", repository)
    gh("release", "edit", f"v{version}", "--repo", repository, "--draft=false", "--latest")
    return {"release": f"v{version}", "published": True, "files": sorted(expected | {"SHA256SUMS"})}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", default="Sayarinu/DSWT")
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("draft", "fetch", "publish"):
        command = commands.add_parser(name)
        command.add_argument("--version", required=True)
    commands.choices["draft"].add_argument("--archive", required=True, type=Path)
    commands.choices["fetch"].add_argument("--output", required=True, type=Path)
    commands.choices["publish"].add_argument("--directory", required=True, type=Path)
    args = parser.parse_args(argv)
    if not VERSION.fullmatch(args.version) or not re.fullmatch(r"[\w.-]+/[\w.-]+", args.repo):
        parser.error("use a version such as 0.1.0 or 0.1.0-rc.1 and an owner/name repository")
    try:
        if args.command == "draft":
            result = draft(args.version, args.archive, args.repo)
        elif args.command == "fetch":
            result = fetch(args.version, args.repo, args.output)
        else:
            result = publish(args.version, args.repo, args.directory)
    except (OSError, ValueError, KeyError) as error:
        print(f"{args.command} failed: {error}", file=sys.stderr)
        return 1
    print(json.dumps(result))
    return 0


if __name__ == "__main__":
    sys.exit(main())
