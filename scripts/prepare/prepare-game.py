#!/usr/bin/env python3
"""Prepare local upstream inputs without overwriting developer changes."""

import argparse
import json
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys
import tempfile


# Each upstream decompilation has its own pin, integration patch, adapter and
# resource overlay. The shared core overlays every workspace; another game's
# adapter and resources never do. Build patches apply to pristine workspaces too
# and leave upstream output unchanged. Staged revisions build from inputs staged
# from a verified local dump, so upstream lists no checksum for them.
GAMES = {
    "platinum": {
        "pin": "client/games/upstream.json",
        "patch": "client/games/pokeplatinum.patch",
        "build_patches": ("client/games/europe.patch",),
        "staged_revisions": ("10",),
        "workspace": "pokeplatinum",
        "adapter": "platinum",
        "resources": ("res",),
        "rom_sha1": "platinum.us/rom_rev{}.sha1",
    },
    "hgss": {
        "pin": "client/games/upstream-pokeheartgold.json",
        "patch": "client/games/pokeheartgold.patch",
        "build_patches": (),
        "staged_revisions": (),
        "workspace": "pokeheartgold",
        "adapter": "hgss",
        "resources": ("files",),
        "rom_sha1": "{}.us/rom.sha1",
    },
    # pret/pokediamond builds Diamond and Pearl with Make; ARM9 sources live
    # under arm9/, so client/core/src lands in arm9/src, and its own text overlay
    # client/games/files-dp lands in files/.
    "dp": {
        "pin": "client/games/upstream-pokediamond.json",
        "patch": "client/games/pokediamond.patch",
        "build_patches": (),
        "staged_revisions": (),
        "workspace": "pokediamond",
        "adapter": "dp",
        "resources": ("files-dp",),
        "rom_sha1": "poke{}.us.sha1",
        "destinations": {"src": "arm9/src", "files-dp": "files"},
    },
    # fuddlesworth/pokebw2 builds Black 2 and White 2 from one tree, with
    # configure.py and Ninja, from the player's own dumps in orig/. The core
    # and the bw2 adapter land in src/ and include/ as in Platinum.
    "bw2": {
        "pin": "client/games/upstream-pokebw2.json",
        "patch": "client/games/pokebw2.patch",
        "build_patches": (),
        "staged_revisions": (),
        "workspace": "pokebw2",
        "adapter": "bw2",
        "resources": ("res-bw2",),
        "rom_sha1": "{}.sha1",
        "destinations": {"res-bw2": "res"},
    },
    # Project-Holon/aletheia builds Black and White from one tree with its own
    # Python tools and CodeWarrior under Wine. Its targets table
    # (tools/targets.py) lists each cartridge's SHA-1. Wonder Trade is added to
    # the player's dump with aletheia's link as the map (scripts/port/port-bw.py),
    # so its only workspace is the pristine one.
    "bw": {
        "pin": "client/games/upstream-aletheia.json",
        "patch": None,
        # Runs the linker through Wine off Windows, as aletheia runs the compiler,
        # includes generated data through a drive path there, and builds without the
        # DSi key, keeping the dump's DSi block.
        "build_patches": ("client/games/aletheia-build.patch",),
        "staged_revisions": (),
        "workspace": "aletheia",
        "adapter": "bw",
        "resources": (),
        "rom_sha1": "tools/targets.py",
    },
}
ADAPTERS = {game["adapter"] for game in GAMES.values()}


def git(workspace, *args):
    return subprocess.check_output(["git", "-C", str(workspace), *args])


def foreign_adapter(name, game):
    parts = PurePosixPath(name).parts
    return (len(parts) > 3 and parts[0] in ("include", "src") and parts[1] == "wonder_trade"
            and parts[2] in ADAPTERS and parts[2] != game["adapter"])


def patched_paths(patch):
    paths = []
    for line in patch.read_text().splitlines():
        if not line.startswith("diff --git "):
            continue
        match = re.fullmatch(r"diff --git a/([A-Za-z0-9_./-]+) b/\1", line)
        if not match:
            raise RuntimeError("Integration patch must modify regular relative paths")
        path = PurePosixPath(match[1])
        if path.is_absolute() or ".." in path.parts or str(path) != match[1]:
            raise RuntimeError("Unsafe integration patch path")
        paths.append(path.as_posix())
    if not paths or len(set(paths)) != len(paths):
        raise RuntimeError("Integration patch must name unique upstream files")
    return paths


def overlay_files(root, workspace, game=GAMES["platinum"], pristine=False):
    patches = [root / name for name in game["build_patches"]] + ([] if pristine else [root / game["patch"]])
    if not patches:
        return {}, {}
    paths = list(dict.fromkeys(path for patch in patches for path in patched_paths(patch)))
    base = {name: git(workspace, "show", f"HEAD:{name}") for name in paths}
    with tempfile.TemporaryDirectory(prefix="dswt-overlay-") as temp:
        stage = Path(temp)
        for name, content in base.items():
            dest = stage / name
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(content)
        for patch in patches:
            subprocess.run(
                ["git", "apply", "--unidiff-zero", str(patch)], cwd=stage, check=True
            )
        desired = {name: (stage / name).read_bytes() for name in paths}

    overlays = [(group, directory) for group in ("core", "games") for directory in ("include", "src")]
    overlays.extend(("games", directory) for directory in game["resources"])
    for group, directory in () if pristine else overlays:
        source = root / "client" / group / directory
        if source.parent.is_symlink() or source.is_symlink():
            raise RuntimeError(f"Client overlays must not contain symlinks: {source}")
        for path in sorted(source.rglob("*")):
            if path.is_symlink():
                raise RuntimeError(f"Client overlays must not contain symlinks: {path}")
            if path.is_file():
                name = path.relative_to(source.parent).as_posix()
                if foreign_adapter(name, game):
                    continue
                if directory in game.get("destinations", {}):
                    name = game["destinations"][directory] + name[len(directory):]
                if name in desired:
                    raise RuntimeError(f"Overlay and integration patch overlap: {name}")
                desired[name] = path.read_bytes()
    return base, desired


def configure(root, workspace, pin, pristine, game=GAMES["platinum"]):
    if workspace.is_symlink() or (workspace / ".git").is_symlink():
        raise RuntimeError("Game workspace must be a standalone local checkout")
    if not (workspace / ".git").is_dir():
        raise RuntimeError("Existing game workspace is not a standalone Git checkout")
    if Path(git(workspace, "rev-parse", "--show-toplevel").decode().strip()) != workspace:
        raise RuntimeError("Game workspace must be its own checkout")
    if git(workspace, "rev-parse", "HEAD").decode().strip() != pin["revision"]:
        raise RuntimeError("Game workspace has an incompatible upstream revision")
    if git(workspace, "rev-parse", "HEAD^{tree}").decode().strip() != pin["tree"]:
        raise RuntimeError("Game workspace does not match the pinned upstream tree")
    for revision, digest in pin["rom_sha1"].items():
        if revision in game["staged_revisions"]:
            continue
        if "{}" not in game["rom_sha1"]:
            # A table of every target's SHA-1 in one file
            if f'"{digest}"'.encode() not in git(workspace, "show", "HEAD:" + game["rom_sha1"]):
                raise RuntimeError("Pristine ROM checksums differ from the recorded inputs")
            continue
        upstream = git(workspace, "show", "HEAD:" + game["rom_sha1"].format(revision))
        if upstream.decode().split()[0] != digest:
            raise RuntimeError("Pristine ROM checksums differ from the recorded inputs")

    base, desired = overlay_files(root, workspace, game, pristine)
    marker = workspace / ".git/dswt-overlays.json"
    if marker.is_symlink():
        raise RuntimeError("Overlay ownership record must not be a symlink")
    previous = json.loads(marker.read_text()) if marker.exists() else []
    if not isinstance(previous, list) or not all(isinstance(name, str) for name in previous):
        raise RuntimeError("Invalid overlay ownership record")
    changed = set(filter(None, git(workspace, "diff", "--name-only", "--no-renames", "-z", "HEAD").decode().split("\0")))
    if changed - base.keys():
        raise RuntimeError("Unmanaged upstream changes found; keep this workspace aside before rebuilding")
    untracked = set(filter(None, git(workspace, "ls-files", "--others", "--exclude-standard", "-z").decode().split("\0")))
    if untracked - desired.keys():
        raise RuntimeError("Unmanaged local files found; keep this workspace aside before rebuilding")
    for name, content in desired.items():
        dest = workspace / name
        if any(path.is_symlink() for path in (dest, *dest.parents)):
            raise RuntimeError(f"Refusing a symlink in the game workspace: {name}")
        if (name in base or name in previous) and not dest.exists():
            raise RuntimeError(f"Local deletion of {name}; keep this workspace aside before rebuilding")
        # A checkout applies .gitattributes (such as eol=crlf), so compare with that form too.
        checkout = git(workspace, "cat-file", "--filters", f"HEAD:{name}") if name in base else None
        if dest.exists() and dest.read_bytes() not in (base.get(name), checkout, content):
            raise RuntimeError(f"Local changes in {name}; keep this workspace aside before rebuilding")
    for name, content in desired.items():
        dest = workspace / name
        if not dest.exists() or dest.read_bytes() != content:
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(content)
    marker.write_text(json.dumps(sorted(desired)) + "\n")


def prepare(root, pristine=False, name="platinum"):
    root = root.resolve()
    game = GAMES[name]
    if game["patch"] is None:
        # Wonder Trade is not built in this workspace: the port reads its pristine build.
        pristine = True
    pin = json.loads((root / game["pin"]).read_text())
    if (root / "game").is_symlink():
        raise RuntimeError("Game workspace directory must not be a symlink")
    workspace = root / "game" / (game["workspace"] + ("-pristine" if pristine else ""))
    if workspace.exists() or workspace.is_symlink():
        configure(root, workspace, pin, pristine, game)
        return workspace

    workspace.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=f".{workspace.name}-", dir=workspace.parent) as temp:
        stage = Path(temp)
        git(stage, "init", "--quiet")
        git(stage, "fetch", "--depth=1", pin["repository"], pin["revision"])
        git(stage, "checkout", "--quiet", "--detach", "FETCH_HEAD")
        configure(root, stage, pin, pristine, game)
        stage.rename(workspace)
    return workspace


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pristine", action="store_true", help="prepare a separate unmodified workspace")
    parser.add_argument("--game", choices=sorted(GAMES), default="platinum",
                        help="upstream to prepare: platinum, hgss for HeartGold and SoulSilver, "
                             "dp for Diamond and Pearl, bw2 for Black 2 and White 2, or bw for Black and White (always pristine)")
    args = parser.parse_args()
    try:
        print(prepare(Path(__file__).resolve().parents[2], args.pristine, args.game))
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"Cannot prepare game workspace: {error}", file=sys.stderr)
        sys.exit(1)
