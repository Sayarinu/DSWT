"""Preserve synthetic active journals through checked host entropy refresh."""

import argparse
import binascii
import hashlib
import os
from pathlib import Path
import selectors
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
BANKS = (0x30000, 0x70000)
# Generation V (protocol/save-layout-bw.json, protocol/save-layout-b2w2.json): the
# main copies, the end of their blocks (where the CRC table starts), the block
# count, the trainer data block's size, the musical distribution area and the
# journal banks inside it, and the targets.
GEN_V = {
    "bw": {"copies": (0, 0x24000), "blocks_end": 0x23F00, "blocks": 69, "trainer_size": 0x68,
           "musical": (0x56000, 0x1FD14), "banks": (0x74000, 0x75000), "targets": ("black-us", "white-us")},
    "b2w2": {"copies": (0, 0x26000), "blocks_end": 0x25F00, "blocks": 74, "trainer_size": 0xB0,
             "musical": (0x55800, 0x17D14), "banks": (0x6B000, 0x6C000), "targets": ("black2-us", "white2-us")},
}


def seal(record):
    record[4060:4092] = hashlib.sha256(record[:4060]).digest()
    record[4092:] = b"CMIT"


# Retail block offsets/sizes and footer per game family (protocol/save-layout*.json).
LAYOUTS = {
    "us-rev1": (((0, 53036), (53036, 74212)), 20, lambda size, block, crc: (1, 1, size, 0x20060623, block, crc), "<IIIIBxH"),
    "diamond-us-rev5": (((0, 49408), (49408, 74208)), 20, lambda size, block, crc: (1, 1, size, 0x20060623, block, crc),
                        "<IIIIBxH"),
    "heartgold-us-rev0": (((0, 63016), (63232, 74512)), 16, lambda size, block, crc: (1, size, 0x20060623, block, crc),
                          "<IIIHH"),
}


def retail_save(record, target="us-rev1"):
    blocks, footer, fields, layout = LAYOUTS[target]
    data = bytearray(b"\xff" * 524288)
    for half in (0, 262144):
        for block, (offset, size) in enumerate(blocks):
            start = half + offset
            data[start:start + size - footer] = bytes(size - footer)
            crc = binascii.crc_hqx(data[start:start + size - footer], 0xffff)
            struct.pack_into(layout, data, start + size - footer, *fields(size, block, crc))
    for bank in BANKS:
        data[bank:bank + 4096] = record
    return bytes(data)


def gen_v_save(family, record, marker=0):
    """A Black/White or Black 2/White 2 save: both main copies with a valid CRC table
    and footer, trainer data block 27 holding musical marker 6, a stale musical
    distribution, and the record in both banks of its area."""
    facts = GEN_V[family]
    end, trainer, size = facts["blocks_end"], 0x19400, facts["trainer_size"]
    crcs = (facts["blocks"] + facts["blocks"] % 2) * 2
    data = bytearray(b"\xff" * 524288)
    for copy in facts["copies"]:
        data[copy:copy + end] = bytes(end)
        struct.pack_into("<H", data, copy + trainer + 0x3C, marker)
        crc = binascii.crc_hqx(data[copy + trainer:copy + trainer + size], 0xffff)
        struct.pack_into("<HH", data, copy + trainer + size, 1, crc)
        struct.pack_into("<H", data, copy + end + 2 * 27, crc)
        table = data[copy + end:copy + end + crcs]
        struct.pack_into("<IIIHH", data, copy + end + crcs, 1, end + crcs + 16, 0x31053527, 0,
                         binascii.crc_hqx(table, 0xffff))
    start, used = facts["musical"]
    data[start:start + used] = (b"stale musical distribution" * (used // 26 + 1))[:used]
    for bank in facts["banks"]:
        data[bank:bank + 4096] = record
    return bytes(data)


class JournalSeedTests(unittest.TestCase):
    banks = BANKS

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="dswt-journal-host-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        # ld.so splits LD_PRELOAD on spaces, including spaces in checkout paths.
        preload = tempfile.TemporaryDirectory(prefix="dswt-preload-", dir="/tmp")
        self.addCleanup(preload.cleanup)
        alias = Path(preload.name) / "faults.so"
        alias.symlink_to(self.faults)
        self.faults = alias
        self.save = self.root / "copy.sav"

    def record(self, phase, fixture=None):
        record = subprocess.check_output([str(fixture or self.fixture), str(phase)], timeout=5)
        self.assertEqual(len(record), 4096)
        return record

    def install(self, phase):
        original = retail_save(self.record(phase))
        self.save.write_bytes(original)
        self.save.chmod(0o600)
        return original

    def tool(self, mode, *, ok=True, target="us-rev1"):
        args = [str(self.store), mode, str(self.save)]
        if mode in ("inspect", "seed"):
            args.append(target)
        result = subprocess.run(args, capture_output=True, timeout=10)
        self.assertEqual(result.returncode == 0, ok, result.stdout + result.stderr)
        return result

    def preserved(self, original):
        actual = self.save.read_bytes()
        first, second = (actual[bank:bank + 4096] for bank in self.banks)
        self.assertEqual(first, second)
        self.assertEqual(first[4092:], b"CMIT")
        self.assertEqual(first[4060:4092], hashlib.sha256(first[:4060]).digest())
        self.assertGreater(struct.unpack_from("<Q", first, 16)[0],
                           struct.unpack_from("<Q", original, self.banks[0] + 16)[0])
        self.assertNotEqual(first[1444:1476], original[self.banks[0] + 1444:self.banks[0] + 1476])
        restored = bytearray(actual)
        for bank in self.banks:
            for offset, size in ((16, 8), (1444, 32), (4060, 32)):
                restored[bank + offset:bank + offset + size] = original[bank + offset:bank + offset + size]
        self.assertEqual(bytes(restored), original)
        self.tool("check")

    def test_all_supported_phases_preserve_every_non_entropy_byte(self):
        for phase in range(11):
            with self.subTest(phase=phase):
                self.save = self.root / f"phase-{phase}.sav"
                original = self.install(phase)
                self.tool("inspect")
                self.assertFalse(Path(str(self.save) + ".dswt").exists())
                self.assertEqual(self.save.read_bytes(), original)
                self.tool("prepare")
                self.tool("seed")
                self.preserved(original)

    def test_unknown_conflicting_exhausted_and_foreign_input_is_read_only(self):
        original = retail_save(self.record(8))
        variants = [retail_save(b"\xff" * 4096), original[:100]]
        for offset in (4, 6, 12, 48, 1952, 2216):
            data = bytearray(original)
            if offset == 48:
                data[BANKS[0] + 48:BANKS[0] + 80] = bytes(32)
            else:
                data[BANKS[0] + offset] = 255
            record = data[BANKS[0]:BANKS[0] + 4096]
            seal(record); data[BANKS[0]:BANKS[0] + 4096] = record
            variants.append(bytes(data))
        data = bytearray(original); data[0x32000] = 1; variants.append(bytes(data))
        data = bytearray(original); data[BANKS[1] + 1444] ^= 1
        record = data[BANKS[1]:BANKS[1] + 4096]; seal(record); data[BANKS[1]:BANKS[1] + 4096] = record
        variants.append(bytes(data))
        data = bytearray(original)
        for bank in BANKS:
            record = data[bank:bank + 4096]; struct.pack_into("<Q", record, 16, 2**64 - 1); seal(record)
            data[bank:bank + 4096] = record
        variants.append(bytes(data))
        data = bytearray(original); data[0] ^= 1; data[262144] ^= 1; variants.append(bytes(data))
        for index, data in enumerate(variants):
            with self.subTest(index=index):
                self.save = self.root / f"unknown-{index}.sav"; self.save.write_bytes(data); self.save.chmod(0o600)
                self.tool("inspect", ok=False)
                self.assertEqual(self.save.read_bytes(), data)
                self.assertFalse(Path(str(self.save) + ".dswt").exists())
        self.install(1); self.tool("inspect", target="diamond-us-rev5", ok=False)

    def test_korean_footer_magic_is_refused_for_other_targets(self):
        record = self.record(5)
        self.save = self.root / "korean.sav"
        data = bytearray(retail_save(record))
        for half in (0, 262144):
            for offset, size in ((0, 53036), (53036, 74212)):
                struct.pack_into("<I", data, half + offset + size - 20 + 12, 0x20070903)
        self.save.write_bytes(bytes(data)); self.save.chmod(0o600)
        self.tool("inspect", target="us-rev1", ok=False)
        self.assertEqual(self.save.read_bytes(), bytes(data))

    def test_each_game_family_layout_is_checked(self):
        record = self.record(5)
        for target in LAYOUTS:
            with self.subTest(target):
                self.save = self.root / f"{target}.sav"
                for other in LAYOUTS:
                    self.save.unlink(missing_ok=True)
                    self.save.write_bytes(retail_save(record, target))
                    self.save.chmod(0o600)
                    self.tool("inspect", target=other, ok=other == target)
                self.tool("prepare")
                self.tool("seed", target=target)
                self.assertNotEqual(self.save.read_bytes()[BANKS[0] + 1444:BANKS[0] + 1476], record[1444:1476])

    def test_a_recognized_torn_mirror_is_repaired_without_losing_active_data(self):
        original = self.install(5)
        damaged = bytearray(original); damaged[BANKS[1] + 4092:BANKS[1] + 4096] = bytes(4)
        self.save.write_bytes(damaged)
        self.tool("inspect"); self.tool("prepare"); self.tool("seed")
        self.preserved(original)

    def test_seed_failure_preserves_existing_unknown_evidence_and_snapshots(self):
        original = bytearray(self.install(8)); original[BANKS[1] + 4] = 2
        self.save.write_bytes(original)
        self.tool("prepare")
        store = Path(str(self.save) + ".dswt")
        before = {name: (store / name).read_bytes() for name in ("A", "B", "HEAD")}
        self.tool("seed", ok=False)
        self.assertEqual(self.save.read_bytes(), bytes(original))
        self.assertEqual({name: (store / name).read_bytes() for name in before}, before)

    def test_every_seed_publication_survives_kill_or_write_failure(self):
        self.seed_survives_every_fault(self.install(5), "us-rev1")

    def test_every_gen_v_seed_publication_survives_kill_or_write_failure(self):
        for family, target in (("bw", "black-us"), ("b2w2", "black2-us")):
            with self.subTest(family=family):
                self.banks = GEN_V[family]["banks"]
                self.save = self.root / f"{family}.sav"
                original = gen_v_save(family, self.record(5, self.fixtures[family]))
                self.save.write_bytes(original); self.save.chmod(0o600)
                self.tool("inspect", target=target)
                self.seed_survives_every_fault(original, target)

    def seed_survives_every_fault(self, original, target):
        self.tool("prepare")
        store = Path(str(self.save) + ".dswt")
        baseline = Path(str(self.save) + ".baseline")
        shutil.copytree(store, baseline)
        # Each journal mirror is one logged record (append and fsync); the
        # checkpoint then closes the log, makes four atomic host publications
        # with five fences each and starts a new log. Every fault point is tried
        # until a run passes them all.
        for mode in ("before", "after", "error"):
            for stage in range(1, 61):
                shutil.rmtree(store); shutil.copytree(baseline, store); self.save.write_bytes(original)
                env = dict(os.environ, LD_PRELOAD=str(self.faults), DSWT_FAULT_ARMED="1",
                           DSWT_FAULT_SAVE=str(self.save), DSWT_FAULT_AT=str(stage), DSWT_FAULT_MODE=mode)
                process = subprocess.Popen([str(self.store), "seed", str(self.save), target],
                                           env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                try:
                    if mode != "error":
                        with selectors.DefaultSelector() as selector:
                            selector.register(process.stderr, selectors.EVENT_READ)
                            self.assertTrue(selector.select(5), "seed fence did not report")
                            if process.stderr.readline() == b"STOPPED\n":
                                process.kill()
                    out, err = process.communicate(timeout=5)
                finally:
                    if process.poll() is None:
                        process.kill(); process.communicate()
                if b"verified" in out and process.returncode == 0:
                    self.assertGreaterEqual(stage, 30, f"{mode}: only {stage - 1} fault points")
                    self.preserved(original)
                    break
                with self.subTest(mode=mode, stage=stage):
                    self.assertEqual(process.returncode, 74 if mode == "error" else -signal.SIGKILL, err)
                    self.tool("prepare")
                    self.tool("seed", target=target)
                    self.preserved(original)
            else:
                self.fail(f"{mode}: the seed never completed")

    def test_a_kill_during_the_first_journal_write_leaves_unused_banks(self):
        self.first_write_survives_every_kill(retail_save(b"\xff" * 4096), BANKS, self.fixture)

    def test_a_kill_during_the_first_black_journal_write_leaves_unused_banks(self):
        self.first_write_survives_every_kill(gen_v_save("bw", b"\xff" * 4096), GEN_V["bw"]["banks"], self.fixtures["bw"])

    def test_a_kill_during_the_first_black2_journal_write_leaves_unused_banks(self):
        self.first_write_survives_every_kill(gen_v_save("b2w2", b"\xff" * 4096), GEN_V["b2w2"]["banks"],
                                             self.fixtures["b2w2"])

    def first_write_survives_every_kill(self, blank, banks, fixture):
        """The game's first journal write as the durable melonDS logs it, one
        flash write at a time (wonder_trade/flash.c: tag cleared, the body a page
        at a time, then the tag), in each bank in turn. The emulator is killed at
        every fault point. Whatever survives is either unused banks, which the
        journal core erases and initializes, or a record; never a blocked save."""
        record = bytearray(4096)
        struct.pack_into("<4sHHIIQ", record, 0, b"DSWJ", 1, 0, 4096, 0, 1)
        struct.pack_into("<HH", record, 1440, 1, 32)
        record[1444:1476] = bytes(range(1, 33))
        seal(record)
        writes = []
        for bank in banks:
            writes.append((bank + 4092, bytes(4)))
            writes += [(bank + page, record[page:min(page + 256, 4092)]) for page in range(0, 4092, 256)]
            writes.append((bank + 4092, bytes(record[4092:])))
        images, data = [], bytearray(blank)
        for address, chunk in writes:
            data[address:address + len(chunk)] = chunk
            images.append(bytes(data))
        paths = []
        for index, image in enumerate(images):
            path = self.root / f"first-write-{index}"
            path.write_bytes(image)
            paths.append(str(path))
        self.save.write_bytes(blank); self.save.chmod(0o600)
        store = Path(str(self.save) + ".dswt")
        self.tool("prepare")
        baseline = self.root / "first-write-baseline"
        shutil.copytree(store, baseline)
        states = [blank] + images
        for mode in ("before", "after"):
            for stage in range(1, 400):
                shutil.rmtree(store); shutil.copytree(baseline, store); self.save.write_bytes(blank)
                env = dict(os.environ, LD_PRELOAD=str(self.faults), DSWT_FAULT_SAVE=str(self.save),
                           DSWT_FAULT_AT=str(stage), DSWT_FAULT_MODE=mode)
                process = subprocess.Popen([str(self.probe), str(self.save), *paths], env=env,
                                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                try:
                    with selectors.DefaultSelector() as selector:
                        selector.register(process.stderr, selectors.EVENT_READ)
                        self.assertTrue(selector.select(5), "fault checkpoint did not report")
                        if process.stderr.readline() == b"STOPPED\n":
                            process.kill()
                    out, err = process.communicate(timeout=5)
                finally:
                    if process.poll() is None:
                        process.kill(); process.communicate()
                completed = out == b"COMMITTED\n" and process.returncode == 0
                with self.subTest(mode=mode, stage=stage):
                    if not completed:
                        self.assertEqual(process.returncode, -signal.SIGKILL, err)
                    self.tool("prepare")
                    actual = self.save.read_bytes()
                    self.assertIn(actual, states)
                    state = states.index(actual)
                    banked = b"".join(actual[bank:bank + 4096] for bank in banks)
                    result = subprocess.run([str(fixture), "classify"], input=banked, capture_output=True, timeout=5)
                    # Until the first bank's tag is written no record exists.
                    self.assertEqual(result.stdout, b"unused\n" if state < len(writes) // 2 else b"journal\n")
                if completed:
                    self.assertEqual(self.save.read_bytes(), images[-1])
                    self.assertGreater(stage, len(images), f"{mode}: only {stage - 1} fault points")
                    break
            else:
                self.fail(f"{mode}: the probe never completed")

    def test_durable_launcher_uses_native_active_refresh_and_preserves_lock(self):
        original = self.install(8)
        marker = self.root / "child-observed"
        code = ("import fcntl,os,sys; from pathlib import Path; "
                "assert os.environ['DSWT_DURABLE_SAVE']==sys.argv[1]; "
                "fcntl.flock(int(os.environ['DSWT_SAVE_LOCK_FD']),fcntl.LOCK_EX|fcntl.LOCK_NB); "
                "Path(sys.argv[2]).write_text('started')")
        result = subprocess.run([sys.executable, str(ROOT / "scripts/run-emulator.py"),
                                 "--save", str(self.save), "--target", "us-rev1", "--store-tool", str(self.store),
                                 "--", sys.executable, "-c", code, str(self.save), str(marker)],
                                capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(marker.read_text(), "started")
        self.preserved(original)

    def test_unprovisioned_launcher_never_creates_store_or_starts_child(self):
        original = retail_save(b"\xff" * 4096)
        self.save.write_bytes(original); self.save.chmod(0o600)
        marker = self.root / "must-not-start"
        result = subprocess.run([sys.executable, str(ROOT / "scripts/run-emulator.py"),
                                 "--save", str(self.save), "--target", "us-rev1", "--store-tool", str(self.store),
                                 "--", sys.executable, "-c", "from pathlib import Path;Path(__import__('sys').argv[1]).touch()", str(marker)],
                                capture_output=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(marker.exists())
        self.assertFalse(Path(str(self.save) + ".dswt").exists())
        self.assertEqual(self.save.read_bytes(), original)

    def test_gen_v_journals_refresh_in_every_phase(self):
        for family, facts in GEN_V.items():
            self.banks = facts["banks"]
            others = [target for name, other in GEN_V.items() if name != family for target in other["targets"]]
            for target in facts["targets"]:
                for phase in range(11):
                    with self.subTest(target=target, phase=phase):
                        self.save = self.root / f"{target}-{phase}.sav"
                        original = gen_v_save(family, self.record(phase, self.fixtures[family]))
                        self.save.write_bytes(original); self.save.chmod(0o600)
                        for other in ("us-rev1", "heartgold-us-rev0", *others):
                            self.tool("inspect", target=other, ok=False)
                        self.tool("inspect", target=target)
                        self.assertEqual(self.save.read_bytes(), original)
                        self.tool("prepare")
                        self.tool("seed", target=target)
                        self.preserved(original)

    def test_gen_v_inspection_refuses_other_families_and_a_musical_in_use(self):
        gen4 = retail_save(self.record(5))
        for family, facts in GEN_V.items():
            fixture, target = self.fixtures[family], facts["targets"][0]
            variants = {
                "a Generation IV save": gen4,
                # Phase 5 holds a party Pokémon: a Generation IV-sized one is not a Generation V journal.
                "a Generation IV journal": gen_v_save(family, self.record(5)),
                "a musical still in use": gen_v_save(family, self.record(5, fixture), marker=0xC21E),
                "no journal": gen_v_save(family, b"\xff" * 4096),
            }
            for other in GEN_V:
                if other != family:
                    variants[f"a {other} save"] = gen_v_save(other, self.record(5, self.fixtures[other]))
            damaged = bytearray(gen_v_save(family, self.record(5, fixture)))
            footer = facts["blocks_end"] + (facts["blocks"] + facts["blocks"] % 2) * 2
            for copy in facts["copies"]:
                damaged[copy + footer + 8] ^= 1
            variants["no valid footer"] = bytes(damaged)
            for name, data in variants.items():
                with self.subTest(family=family, variant=name):
                    self.save = self.root / f"refused-{family}-{len(name)}.sav"
                    self.save.write_bytes(data); self.save.chmod(0o600)
                    self.tool("inspect", target=target, ok=False)
                    self.assertEqual(self.save.read_bytes(), data)
                    self.assertFalse(Path(str(self.save) + ".dswt").exists())
            self.save = self.root / f"{family}.sav"
            self.save.write_bytes(gen_v_save(family, self.record(5, fixture))); self.save.chmod(0o600)
            self.tool("inspect", target="us-rev1", ok=False)

    def test_durable_launcher_refreshes_a_gen_v_journal(self):
        for family, target in (("bw", "white-us"), ("b2w2", "black2-us")):
            with self.subTest(family=family):
                self.banks = GEN_V[family]["banks"]
                self.save = self.root / f"{family}.sav"
                original = gen_v_save(family, self.record(8, self.fixtures[family]))
                self.save.write_bytes(original); self.save.chmod(0o600)
                marker = self.root / f"{family}-child-observed"
                code = ("import os,sys; from pathlib import Path; "
                        "assert os.environ['DSWT_DURABLE_SAVE']==sys.argv[1]; Path(sys.argv[2]).write_text('started')")
                result = subprocess.run([sys.executable, str(ROOT / "scripts/run-emulator.py"),
                                         "--save", str(self.save), "--target", target, "--store-tool", str(self.store),
                                         "--", sys.executable, "-c", code, str(self.save), str(marker)],
                                        capture_output=True, timeout=15)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(marker.read_text(), "started")
                self.preserved(original)

    def test_inspection_rejects_symlink_and_hardlink_aliases(self):
        original = self.install(1)
        source = self.save
        self.save = self.root / "alias.sav"; self.save.symlink_to(source)
        self.tool("inspect", ok=False)
        self.save.unlink(); os.link(source, self.save)
        self.tool("inspect", ok=False)
        self.assertEqual(source.read_bytes(), original)

    def test_failed_zero_or_repeated_host_entropy_never_publishes(self):
        original = self.install(8)
        self.tool("prepare")
        store = Path(str(self.save) + ".dswt")
        before = {name: (store / name).read_bytes() for name in ("A", "B", "HEAD")}
        for mode in ("error", "zero", "same"):
            with self.subTest(mode=mode):
                env = dict(os.environ, LD_PRELOAD=str(self.faults), DSWT_SEED_RANDOM_MODE=mode)
                result = subprocess.run([str(self.store), "seed", str(self.save), "us-rev1"],
                                        env=env, capture_output=True, timeout=5)
                self.assertEqual(result.returncode, 74, result.stdout + result.stderr)
                self.assertEqual(self.save.read_bytes(), original)
                self.assertEqual({name: (store / name).read_bytes() for name in before}, before)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("store", type=Path)
    parser.add_argument("fixture", type=Path)
    parser.add_argument("fixture_bw", type=Path)
    parser.add_argument("fixture_b2w2", type=Path)
    parser.add_argument("faults", type=Path)
    parser.add_argument("probe", type=Path)
    args = parser.parse_args()
    JournalSeedTests.store, JournalSeedTests.fixture, JournalSeedTests.faults = (
        args.store.resolve(), args.fixture.resolve(), args.faults.resolve())
    JournalSeedTests.fixtures = {"bw": args.fixture_bw.resolve(), "b2w2": args.fixture_b2w2.resolve()}
    JournalSeedTests.probe = args.probe.resolve()
    unittest.main(argv=[__file__], verbosity=2)
