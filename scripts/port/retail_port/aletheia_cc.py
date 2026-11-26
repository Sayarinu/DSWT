"""Compile aletheia source files the way aletheia's own build does; run with aletheia's tools.

usage: aletheia_cc.py WORKSPACE TARGET OUTPUT CHANGED SOURCE...

Each SOURCE names a file of WORKSPACE/src/game, such as ov021/field_menu.c.
Where CHANGED/src/game holds a file of the same name, that copy is compiled in
its place, among the workspace's other sources and headers. Each is compiled
with the compiler, flags, instruction set and language its header names, and,
for White, with its address literals mapped as aletheia maps them
(tools/white_map.py). The object goes to OUTPUT/<region>_<name>.o; a JSON
line per source reports it.
"""

import json
import os
from pathlib import Path
import shutil
import sys
import tempfile


def mirror(game, changed, root):
    """root/src/game: the workspace's src/game with the changed files in place of its own."""
    tree = root / "src/game"
    tree.mkdir(parents=True)
    replaced = {p.relative_to(changed).as_posix() for p in changed.rglob("*") if p.is_file()} if changed.is_dir() else set()
    regions = {name.split("/")[0] for name in replaced}
    for entry in game.iterdir():
        if entry.name not in regions:
            (tree / entry.name).symlink_to(entry)
            continue
        (tree / entry.name).mkdir()
        for item in entry.iterdir():
            name = f"{entry.name}/{item.name}"
            if name not in replaced:
                (tree / name).symlink_to(item)
    for name in replaced:
        shutil.copyfile(changed / name, tree / name)
    return tree


def main():
    workspace, target, output, changed, *sources = sys.argv[1:]
    workspace, output, changed = Path(workspace).resolve(), Path(output).resolve(), Path(changed).resolve()
    os.environ["ALETHEIA_TARGET"] = target
    sys.path.insert(0, str(workspace / "tools"))
    os.chdir(workspace)
    import gamebuild  # noqa: E402  (aletheia's: the header settings)
    import match  # noqa: E402  (aletheia's: compile one translation unit)

    output.mkdir(parents=True, exist_ok=True)
    failed = False
    with tempfile.TemporaryDirectory() as scratch:
        # The compile takes its include roots from the tree the source lies in.
        match.ROOT = scratch
        tree = mirror(workspace / "src/game", changed / "src/game", Path(scratch))
        for name in sources:
            source = tree / name
            region, stem = Path(name).parent.as_posix(), Path(name).stem
            raw = source.read_text(encoding="latin-1")
            header, settings = gamebuild._settings(raw.removeprefix("\xef\xbb\xbf"))
            with tempfile.TemporaryDirectory() as work:
                obj, _, text, _ = match.compile_tu(
                    str(source), settings["compiler"], settings["flags"].split(), settings["isa"] == "thumb",
                    settings["lang"] or None, work)
                out = output / f"{region}_{stem}.o"
                if obj:
                    shutil.copyfile(obj, out)
            report = {"source": name, "object": str(out) if obj else None, "address": header.get("address"),
                      "region": header.get("region"), "isa": settings["isa"], "compiler": settings["compiler"],
                      "flags": settings["flags"]}
            if not obj:
                report["error"] = text[-4000:]
                failed = True
            print(json.dumps(report), flush=True)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
