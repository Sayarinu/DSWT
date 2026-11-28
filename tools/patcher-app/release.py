#!/usr/bin/env python3
"""Stage and publish a GitHub release of the player apps.

`draft` runs locally, where the ROMs are: it uploads dswt-VERSION.zip to a new draft
release tagged at the manifest's source commit. The Release patcher workflow then
runs `fetch` to verify that archive, builds the apps around it, and runs `publish`,
which uploads them with SHA256SUMS, removes the archive and publishes the draft.
`notes` rewrites a published release's notes in the current format.
Players download one file: each app carries every patch.
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


MERGED = re.compile(r"(?:[A-Z][A-Z0-9]*-[0-9]+ - )?(?P<title>.+ \(#[0-9]+\))")


def _version_key(tag):
    match = re.fullmatch(r"v([0-9]+)\.([0-9]+)\.([0-9]+)(-.+)?", tag)
    return None if match is None else (*map(int, match.groups()[:3]), match.group(4) is None, match.group(4) or "")


def previous_release(version, repository):
    """The newest published release older than this version, or None for the first."""
    current = _version_key(f"v{version}")
    releases = json.loads(gh("release", "list", "--repo", repository, "--exclude-drafts", "--limit", "100",
                             "--json", "tagName"))
    older = [tag for tag in (item["tagName"] for item in releases)
             if _version_key(tag) is not None and _version_key(tag) < current]
    return max(older, key=_version_key, default=None)


def changes(version, commit, repository):
    """Titles of the pull requests merged since the previous release, without ticket IDs."""
    previous = previous_release(version, repository)
    if previous is None:
        return None, []
    subjects = gh("api", f"repos/{repository}/compare/{previous}...{commit}", "--paginate", "--jq",
                  ".commits[] | select(.parents | length > 1) | .commit.message | split(\"\\n\")[0]")
    titles = [match.group("title") for match in map(MERGED.fullmatch, subjects.splitlines()) if match]
    return previous, titles


def notes(repository, previous, titles):
    bullets = []
    for title in titles:
        summary, number = title.rsplit(" (#", 1)
        number = number.removesuffix(")")
        bullets.append(f"- {summary}. [#{number}](https://github.com/{repository}/pull/{number})")
    if not bullets:
        bullets.append("- First release." if previous is None else "- No merged pull requests since the last release.")
    return "<details>\n<summary>Core changes</summary>\n\n" + "\n".join(bullets) + "\n\n</details>\n"


def draft(version, archive, repository):
    manifest, _ = build.read_archive(archive, version)
    if Path(archive).name != archive_name(version):
        raise build.BuildError(f"Name the archive {archive_name(version)}")
    commit = manifest["source"]["commit"]
    gh("api", f"repos/{repository}/commits/{commit}", "--silent")
    gh("release", "create", f"v{version}", str(archive), "--repo", repository, "--draft", "--target", commit,
       "--title", f"DSWT {version}", "--notes",
       notes(repository, *changes(version, commit, repository)))
    return {"release": f"v{version}", "commit": commit, "draft": True}


def rewrite(version, repository):
    """Replace a published release's notes, keeping the source commit it was released with."""
    tag = f"v{version}"
    commit = gh("api", f"repos/{repository}/commits/{tag}", "--jq", ".sha").strip()
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise build.BuildError(f"Cannot resolve the release tag {tag}")
    text = notes(repository, *changes(version, commit, repository))
    gh("release", "edit", f"v{version}", "--repo", repository, "--notes", text)
    return {"release": f"v{version}", "notes": text}


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


def publish(version, repository, directory, companion=False):
    directory = Path(directory)
    names = {f"dswt-patcher-{version}-{name}.zip" for name in PLATFORMS}
    if companion:
        names.update({"dswt.nds", "THIRD-PARTY-NOTICES.txt"})
    found = {path.name for path in directory.iterdir() if path.is_file()}
    if found != names | {archive_name(version)}:
        raise build.BuildError(f"Release files differ from the expected set: {sorted(found ^ (names | {archive_name(version)}))}")
    build.read_archive(directory / archive_name(version), version)
    if _draft_assets(version, repository) != {archive_name(version)}:
        raise build.BuildError(f"The draft must still hold only {archive_name(version)}")
    # The apps carry the archive's patches, so players download one file each.
    sums = "".join(f"{hashlib.sha256((directory / name).read_bytes()).hexdigest()}  {name}\n" for name in sorted(names))
    (directory / "SHA256SUMS").write_text(sums)
    gh("release", "upload", f"v{version}", *(str(directory / name) for name in sorted(names)),
       str(directory / "SHA256SUMS"), "--repo", repository)
    gh("release", "delete-asset", f"v{version}", archive_name(version), "--repo", repository, "--yes")
    if _draft_assets(version, repository) != names | {"SHA256SUMS"}:
        raise build.BuildError("The draft does not hold exactly the selected downloads and SHA256SUMS")
    gh("release", "edit", f"v{version}", "--repo", repository, "--draft=false", "--latest")
    return {"release": f"v{version}", "published": True, "files": sorted(names | {"SHA256SUMS"})}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", default="Sayarinu/DSWT")
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("draft", "fetch", "publish", "notes"):
        command = commands.add_parser(name)
        command.add_argument("--version", required=True)
    commands.choices["draft"].add_argument("--archive", required=True, type=Path)
    commands.choices["fetch"].add_argument("--output", required=True, type=Path)
    commands.choices["publish"].add_argument("--directory", required=True, type=Path)
    commands.choices["publish"].add_argument("--companion", choices=("true", "false"), default="false")
    args = parser.parse_args(argv)
    if not VERSION.fullmatch(args.version) or not re.fullmatch(r"[\w.-]+/[\w.-]+", args.repo):
        parser.error("use a version such as 0.1.0 or 0.1.0-rc.1 and an owner/name repository")
    try:
        if args.command == "draft":
            result = draft(args.version, args.archive, args.repo)
        elif args.command == "notes":
            result = rewrite(args.version, args.repo)
        elif args.command == "fetch":
            result = fetch(args.version, args.repo, args.output)
        else:
            result = publish(args.version, args.repo, args.directory, companion=args.companion == "true")
    except (OSError, ValueError, KeyError) as error:
        print(f"{args.command} failed: {error}", file=sys.stderr)
        return 1
    print(json.dumps(result))
    return 0


if __name__ == "__main__":
    sys.exit(main())
