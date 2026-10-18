#!/usr/bin/env python3
"""Build an ignored DS ROM that exercises the actual blocked retail entrypoints."""

from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def write_probe_file(path, content, exclusive=False):
    descriptor, temporary = tempfile.mkstemp(prefix=".platinumwt-probe-", dir=path.parent)
    try:
        with os.fdopen(descriptor, "wb") as output:
            output.write(content)
        if exclusive:
            os.link(temporary, path)
        else:
            os.replace(temporary, path)
    finally:
        Path(temporary).unlink(missing_ok=True)


@contextmanager
def probe_edits(edits):
    for path, original, _ in edits:
        if path.is_symlink() or (path.exists() if original is None else path.read_bytes() != original):
            raise RuntimeError(f"Preserve unexpected local input: {path}")
    applied = []
    try:
        for path, original, modified in edits:
            applied.append((path, original, modified))
            write_probe_file(path, modified, exclusive=original is None)
        yield
    finally:
        conflicts = []
        for path, original, modified in reversed(applied):
            if original is None and not path.exists() and not path.is_symlink():
                continue
            if original is not None and not path.is_symlink() and path.exists() and path.read_bytes() == original:
                continue
            if not path.exists() or path.is_symlink() or path.read_bytes() != modified:
                conflicts.append(str(path))
            elif original is None:
                path.unlink()
            else:
                write_probe_file(path, original)
        if conflicts:
            raise RuntimeError("Preserved concurrent edits; inspect: " + ", ".join(conflicts))


def build():
    environment = dict(os.environ, ROM_REVISION="1")
    command = ["bash", str(ROOT / "scripts/build-rom.sh")]
    subprocess.run(command, cwd=ROOT, env=environment, check=True)
    workspace = ROOT / "game/pokeplatinum"
    production_hash = hashlib.sha256((workspace / "build/pokeplatinum.us.nds").read_bytes()).digest()
    tls = subprocess.check_output(["python3", str(ROOT / "scripts/prepare-tls.py")], cwd=ROOT, text=True).strip()
    app = workspace / "src/wonder_trade/application.c"
    messages = workspace / "res/text/wonder_trade.json"
    original = (ROOT / "client/src/wonder_trade/application.c").read_bytes()
    text = original.decode()
    replacements = {
        '#include "wonder_trade/recovery.h"': '#include "wonder_trade/recovery.h"\n#include "../../.git/platinumwt-entrypoint-probe.inc"',
        '    worker->result = WtRecovery_Check(io);': '    worker->result = WtRecovery_Check(io);\n    if (worker->result == 0) worker->result = WtRecovery_ProbeEntrypoints() ? 1 : -100;',
        '    u32 size, unused;': '    u32 size, unused;\n    int probeOK;',
        '    OS_JoinThread(&worker->thread);': '    OS_JoinThread(&worker->thread);\n    probeOK = worker->result == 1;',
        '    WonderTrade_Show(app, WT_APP_RECOVERY, WonderTrade_Text_RecoveryRequired);': '    WonderTrade_Show(app, WT_APP_RECOVERY, probeOK ? WonderTrade_Text_Acknowledged : WonderTrade_Text_RecoveryRequired);',
    }
    for old, new in replacements.items():
        if text.count(old) != 1:
            raise RuntimeError("Game integration changed; inspect the probe before rebuilding")
        text = text.replace(old, new)
    original_messages = (ROOT / "client/res/text/wonder_trade.json").read_bytes()
    data = json.loads(original_messages)
    for message in data["messages"]:
        if message["id"] == "WonderTrade_Text_Acknowledged":
            message["en_US"] = "Retail save guards passed.\nSave data and locks unchanged."
    edits = [
        (workspace / ".git/platinumwt-entrypoint-probe.inc", None, (ROOT / "client/tests/retail_guard_probe.c").read_bytes()),
        (app, original, text.encode()),
        (messages, original_messages, (json.dumps(data, ensure_ascii=False, indent=2) + "\n").encode()),
    ]
    output = workspace / "build/pokeplatinum-recovery-probe.nds"
    try:
        with probe_edits(edits):
            subprocess.run(["docker", "run", "--rm", "--user", f"{os.getuid()}:{os.getgid()}",
                            "-v", f"{workspace}:/workspace", "-v", f"{tls}:/tls:ro", "-w", "/workspace",
                            "platinumwt-rom-builder", "make", "release"], cwd=ROOT, env=environment, check=True)
            shutil.copyfile(workspace / "build/pokeplatinum.us.nds", output)
    finally:
        # Rebuild from the restored production inputs; never leave a probe as the normal output.
        subprocess.run(command, cwd=ROOT, env=environment, check=True)
    if hashlib.sha256((workspace / "build/pokeplatinum.us.nds").read_bytes()).digest() != production_hash:
        raise RuntimeError("Restored production ROM differs; inspect the local build before use")
    print(f"Separate entrypoint test ROM: {output}")


if __name__ == "__main__":
    build()
