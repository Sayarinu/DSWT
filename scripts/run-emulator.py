#!/usr/bin/env python3
"""Refresh a checked journal's entropy durably before starting its emulator."""

import argparse
import fcntl
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import stat
import struct
import subprocess


ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location("provision_save", ROOT / "scripts/provision-save.py")
provision = importlib.util.module_from_spec(spec)
spec.loader.exec_module(provision)


def refreshed_record(data, target):
    layout = provision.layout_for(target)
    if len(data) != layout["flash_size"]:
        raise ValueError("Only full saves for verified targets are supported")
    blanked = bytearray(data)
    valid = []
    for bank in layout["journal_banks"]:
        record = data[bank:bank + 4096]
        tail = data[bank + 4096:bank + layout["bank_reserved_size"]]
        if tail not in (bytes(len(tail)), b"\xff" * len(tail)):
            raise ValueError("Unknown reserved-sector contents; preserve recovery evidence")
        blanked[bank:bank + 4096] = tail[:1] * 4096
        if record in (bytes(4096), b"\xff" * 4096):
            continue
        canonical = provision.seed_record(b"\x01" * 32, 0)
        for start, end in ((0, 16), (24, 1444), (1476, 4060)):
            if record[start:end] != canonical[start:end]:
                raise ValueError("Only known EMPTY version-1 layouts can be refreshed by this probe")
        generation = struct.unpack_from("<Q", record, 16)[0]
        seed = record[1444:1476]
        intact = record[4092:] == b"CMIT" and record[4060:4092] == hashlib.sha256(record[:4060]).digest()
        if intact:
            if not any(seed) or record != provision.seed_record(seed, generation):
                raise ValueError("Unsupported committed journal; preserve recovery evidence")
            valid.append((generation, record, bank))
    if not valid:
        raise ValueError("No valid provisioned seed; create a separate save copy first")
    valid.sort(key=lambda item: item[0])
    generation, selected, selected_bank = valid[-1]
    if len(valid) == 2 and valid[0][0] == generation and valid[0][1] != selected:
        raise ValueError("Conflicting journal mirrors; preserve recovery evidence")
    if generation == 2**64 - 1:
        raise ValueError("Journal generation exhausted")
    # Reuse retail CRC/layout validation without modifying any original bytes.
    fresh = provision.provision(bytes(blanked), target)
    seed = fresh[layout["journal_banks"][0] + 1444:layout["journal_banks"][0] + 1476]
    if seed == selected[1444:1476]:
        raise ValueError("Host seed did not change; refusing launch")
    return layout, provision.seed_record(seed, generation + 1), selected_bank


def write_all(descriptor, data, offset):
    while data:
        written = os.pwrite(descriptor, data, offset)
        if written <= 0:
            raise OSError("Incomplete save write")
        offset += written
        data = data[written:]


def owned_file(descriptor):
    info = os.fstat(descriptor)
    if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or info.st_uid != os.geteuid():
        raise ValueError("Use an owned regular save copy without hardlinks")


def run(save, target, command, store_tool=None):
    if not command:
        raise ValueError("Supply the emulator command after --")
    save = save.absolute()
    lock = os.open(str(save) + ".dswt.lock", os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600)
    try:
        owned_file(lock)
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        environment = dict(os.environ, DSWT_SAVE_LOCK_FD=str(lock))
        if store_tool:
            if not Path(str(save) + '.dswt').exists():
                # No recovery store may be created from an original/unseeded save.
                subprocess.run([str(store_tool), 'inspect', str(save), target], env=environment,
                               pass_fds=(lock,), check=True, capture_output=True, timeout=15)
            subprocess.run([str(store_tool), 'prepare', str(save)], env=environment,
                           pass_fds=(lock,), check=True, capture_output=True, timeout=15)
            subprocess.run([str(store_tool), 'seed', str(save), target], env=environment,
                           pass_fds=(lock,), check=True, capture_output=True, timeout=15)
            environment['DSWT_DURABLE_SAVE'] = str(save)
            result = subprocess.run(command, pass_fds=(lock,), env=environment, check=False).returncode
            subprocess.run([str(store_tool), 'prepare', str(save)], env=environment,
                           pass_fds=(lock,), check=True, capture_output=True, timeout=15)
            return result if result >= 0 else 128 - result
        descriptor = os.open(save, os.O_RDWR | os.O_NOFOLLOW)
        try:
            owned_file(descriptor)
            if os.fstat(descriptor).st_size != 524288:
                raise ValueError("Use a full 512 KiB provisioned save copy")
            data = os.pread(descriptor, 524288, 0)
            layout, record, selected_bank = refreshed_record(data, target)
            os.fchmod(descriptor, 0o600)
            # Retain the selected old bank until its replacement is durable.
            banks = [bank for bank in layout["journal_banks"] if bank != selected_bank] + [selected_bank]
            for bank in banks:
                write_all(descriptor, bytes(4), bank + 4092)
                os.fsync(descriptor)
                write_all(descriptor, record[:4092], bank)
                os.fsync(descriptor)
                write_all(descriptor, record[4092:], bank + 4092)
                os.fsync(descriptor)
                if os.pread(descriptor, 4096, bank) != record:
                    raise OSError("Refreshed seed did not verify")
            # The child retains the lock if its launcher is interrupted.
            result = subprocess.run(command, pass_fds=(lock,), env=environment, check=False).returncode
            os.fsync(descriptor)
            return result if result >= 0 else 128 - result
        finally:
            os.close(descriptor)
    finally:
        os.close(lock)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--save", type=Path, required=True)
    parser.add_argument("--target", choices=provision.TARGETS, required=True)
    parser.add_argument("--store-tool", type=Path, help="Use the built dswt-save-store and durable emulator mode")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    try:
        raise SystemExit(run(args.save, args.target, command, args.store_tool))
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        parser.exit(1, f"Cannot start emulator: {error}\n")
