"""Run the pinned Platinum toolchain in the dswt-rom-builder image against a prepared workspace."""

import json
import os
from pathlib import Path
import shlex
import subprocess

from . import elf

IMAGE = "dswt-rom-builder"
MWCC = "/workspace/tools/metroskrew/bin/mwccarm"
PCH_RULE = "build main.nef.p/global_pch.h.mch:"
# Inside the container: the workspace, extra headers searched first, sources and output.
HEADERS, SOURCES, OUTPUT = "/port/include", "/port/src", "port-out"


class ToolchainError(RuntimeError):
    pass


def docker(mounts, script, workdir=None):
    """Run a shell script in the builder image as the calling user; mounts maps host -> container."""
    args = ["docker", "run", "--rm", "--platform", "linux/amd64", "--network", "none",
            "--user", f"{os.getuid()}:{os.getgid()}"]
    for host, container in mounts.items():
        args += ["--mount", f"type=bind,source={host},target={container}" + (",readonly" if container.startswith("/port/") else "")]
    if workdir:
        args += ["--workdir", workdir]
    result = subprocess.run(args + [IMAGE, "bash", "-c", script], capture_output=True, text=True)
    if result.returncode:
        raise ToolchainError(f"{IMAGE} failed:\n{result.stdout[-4000:]}{result.stderr[-4000:]}")
    return result.stdout


def _flags(arguments, language, variant):
    out = []
    for argument in arguments:
        if argument in ("-gccdep", "-MD"):
            continue
        if argument.startswith("-DGAME_LANGUAGE="):
            argument = f"-DGAME_LANGUAGE={language}"
        elif argument.startswith("-DWT_GAME_VARIANT=") and variant is not None:
            argument = f"-DWT_GAME_VARIANT={variant:#010x}u"
        out.append(argument)
    return out


def compile_script(workspace, language, files, wonder_trade, variant=None):
    """Shell commands that build a precompiled header and the given objects into build/port-out.

    files are workspace-relative upstream sources read from SOURCES instead; with
    wonder_trade, every src/wonder_trade source is built from the workspace too.
    Headers in HEADERS shadow the workspace's own."""
    ninja = (Path(workspace) / "build/build.ninja").read_text().split("\n")
    rule = next((i for i, line in enumerate(ninja) if line.startswith(PCH_RULE)), None)
    if rule is None or not ninja[rule + 1].strip().startswith("ARGS ="):
        raise ToolchainError("build.ninja has no precompiled header rule; build the workspace first")
    pch = _flags(shlex.split(ninja[rule + 1].split("=", 1)[1]), language, variant)
    commands = [[MWCC, "-wrap:sdk", "2.0/sp2p2", f"-I{HEADERS}", *pch, "-precompile",
                 f"{OUTPUT}/global_pch.h.mch", "../include/pch/global_pch.h"]]
    seen = set()
    for entry in json.loads((Path(workspace) / "build/compile_commands.json").read_text()):
        name = entry["file"].removeprefix("../")
        if not (name in files or (wonder_trade and name.startswith("src/wonder_trade/") and name.endswith(".c"))):
            continue
        arguments = _flags(shlex.split(entry["command"]), language, variant)
        if "-DPOKEPLATINUM_REVISION=1" not in arguments:
            raise ToolchainError(f"{name} is not built as Platinum Rev 1 code")
        arguments[arguments.index("-o") + 1] = f"{OUTPUT}/{name.replace('/', '_')}.o"
        arguments[arguments.index("global_pch.h.mch")] = f"{OUTPUT}/global_pch.h.mch"
        arguments.insert(arguments.index("-Imain.nef.p"), f"-I{HEADERS}")
        if name in files:
            arguments[-1] = f"{SOURCES}/{name}"
        commands.append(arguments)
        seen.add(name)
    missing = sorted(set(files) - seen)
    if missing:
        raise ToolchainError(f"the workspace does not build {', '.join(missing)}")
    lines = ["set -e", "cd /workspace/build", f"mkdir -p {OUTPUT}"]
    lines += [shlex.join(command) for command in commands]
    lines.append(f"rm -f {OUTPUT}/global_pch.h.mch")
    return "\n".join(lines) + "\n"


def compile_objects(workspace, sources, headers, out, language, files, wonder_trade=False, variant=None, tls=None):
    """Compile into out: files from sources (a tree like the workspace), headers searched first."""
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    mounts = {str(Path(workspace).resolve()): "/workspace", str(out.resolve()): f"/workspace/build/{OUTPUT}",
              str(Path(sources).resolve()): SOURCES, str(Path(headers).resolve()): HEADERS}
    if tls:
        mounts[str(Path(tls).resolve())] = "/tls"
    docker(mounts, compile_script(workspace, language, files, wonder_trade, variant))
    return sorted(out.glob("*.o"))


def assemble(source, output):
    """Assemble GNU as source into an object that links with Metrowerks objects."""
    directory = Path(source).resolve().parent
    docker({str(directory): str(directory)},
           shlex.join(["arm-none-eabi-as", "-march=armv5te", "-o", str(Path(output).resolve()), str(Path(source).resolve())]))
    Path(output).write_bytes(elf.set_eabi_flags(Path(output).read_bytes()))
    return Path(output)


def link(objects, script, output, roots=()):
    """Link with GNU ld; writes output, output.bin (loaded sections), output.map and output.nm."""
    output = Path(output).resolve()
    directories = sorted({str(Path(p).resolve().parent) for p in objects} | {str(Path(script).resolve().parent), str(output.parent)})
    ld = ["arm-none-eabi-ld", "-T", str(Path(script).resolve()), "-o", str(output), "-Map", f"{output}.map",
          "--no-warn-rwx-segments", "--use-blx", "--gc-sections"]
    ld += [f"--require-defined={root}" for root in roots] + [str(Path(p).resolve()) for p in objects]
    copy = ["arm-none-eabi-objcopy", "-O", "binary", "-j", ".text", "-j", ".rodata", "-j", ".data", str(output), f"{output}.bin"]
    docker({d: d for d in directories},
           f"set -e\n{shlex.join(ld)}\n{shlex.join(copy)}\narm-none-eabi-nm {shlex.quote(str(output))} > {shlex.quote(str(output))}.nm\n")
    return output
