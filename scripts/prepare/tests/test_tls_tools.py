import binascii
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[3]


def module(group, name):
    spec = importlib.util.spec_from_file_location(name.replace("-", "_"), ROOT / "scripts" / group / f"{name}.py")
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


connection = module("build", "configure-connection")
provision = module("prepare", "provision-save")
prepare = module("prepare", "prepare-tls")


def synthetic_save(magic=0x20060623):
    data = bytearray(b"\xff" * 524288)
    for half in (0, 262144):
        for block, (offset, size) in enumerate(((0, 53036), (53036, 74212))):
            start = half + offset
            data[start:start + size - 20] = bytes(size - 20)
            crc = binascii.crc_hqx(data[start:start + size - 20], 0xffff)
            struct.pack_into("<IIIIBxH", data, start + size - 20, 1, 1, size, magic, block, crc)
    return bytes(data)


# Generation V saves (client/core/save-layouts/save-layout-bw.json, client/core/save-layouts/save-layout-b2w2.json): the
# main copies, the end of their blocks (where the CRC table starts), the block count,
# the trainer data block's size, the musical distribution area, the journal banks and the targets.
GEN_V = {
    "bw": {"copies": (0, 0x24000), "blocks_end": 0x23F00, "blocks": 69, "trainer_size": 0x68,
           "musical": (0x56000, 0x1FD14), "banks": (0x74000, 0x75000), "targets": ("black-us", "white-us")},
    "b2w2": {"copies": (0, 0x26000), "blocks_end": 0x25F00, "blocks": 74, "trainer_size": 0xB0,
             "musical": (0x55800, 0x17D14), "banks": (0x6B000, 0x6C000), "targets": ("black2-us", "white2-us")},
}
TRAINER = 0x19400


def gen_v_footer(family):
    facts = GEN_V[family]
    return facts["blocks_end"] + (facts["blocks"] + facts["blocks"] % 2) * 2


def gen_v_save(family, markers=(0, 0), musical=b"\xff"):
    """A synthetic Black/White or Black 2/White 2 save: both main copies with a valid CRC
    table and footer, the trainer data block (27) with marker 6 per copy, and a musical area."""
    facts = GEN_V[family]
    end, size, footer = facts["blocks_end"], facts["trainer_size"], gen_v_footer(family)
    data = bytearray(b"\xff" * 524288)
    for copy, marker in zip(facts["copies"], markers):
        data[copy:copy + end] = bytes(end)
        trainer = copy + TRAINER
        struct.pack_into("<H", data, trainer + 0x3C, marker)
        crc = binascii.crc_hqx(data[trainer:trainer + size], 0xffff)
        struct.pack_into("<HH", data, trainer + size, 1, crc)
        struct.pack_into("<H", data, copy + end + 2 * 27, crc)
        table = data[copy + end:copy + footer]
        struct.pack_into("<IIIHH", data, copy + footer, 1, footer + 16, 0x31053527, 0, binascii.crc_hqx(table, 0xffff))
    start, used = facts["musical"]
    data[start:start + used] = (musical * used)[:used]
    return bytes(data)


class TlsToolTests(unittest.TestCase):
    def test_gen_v_saves_take_the_journal_in_the_released_musical_area(self):
        for family, facts in GEN_V.items():
            first, second = facts["banks"]
            for target in facts["targets"]:
                for musical in (b"\xff", b"\0", b"stale musical distribution"):
                    with self.subTest(target=target, musical=musical[:4]):
                        original = gen_v_save(family, musical=musical)
                        result = provision.provision(original, target)
                        record = result[first:first + 4096]
                        self.assertEqual(record, result[second:second + 4096])
                        self.assertEqual(record[:4], b"DSWJ")
                        self.assertEqual(record[4092:], b"CMIT")
                        self.assertEqual(result[:first], original[:first])
                        self.assertEqual(result[second + 4096:], original[second + 4096:])
                        with self.assertRaisesRegex(ValueError, "not blank"):
                            provision.provision(result, target)
            with self.subTest(family=family), self.assertRaisesRegex(ValueError, "no valid copy"):
                provision.provision(synthetic_save(), facts["targets"][0])
            with self.subTest(family=family), self.assertRaisesRegex(ValueError, "no valid copy"):
                provision.provision(gen_v_save(family), "us-rev1")
        # Each Generation V family's save is refused as the other's.
        for family, other in (("bw", "black2-us"), ("b2w2", "white-us")):
            with self.subTest(family=family), self.assertRaisesRegex(ValueError, "no valid copy"):
                provision.provision(gen_v_save(family), other)

    def test_gen_v_save_still_reading_its_musical_is_preserved(self):
        # Either copy's current trainer data with marker 6 set means the game
        # still loads the musical distribution from the area.
        for family, facts in GEN_V.items():
            for markers in ((0xC21E, 0), (0, 0xC21E)):
                for musical in (b"\xff", b"downloaded musical"):
                    with self.subTest(family=family, markers=markers, musical=musical[:4]), \
                            self.assertRaisesRegex(ValueError, "musical"):
                        provision.provision(gen_v_save(family, markers, musical), facts["targets"][0])

    def test_gen_v_retail_footer_and_trainer_block_are_checked(self):
        for family, facts in GEN_V.items():
            footer, backup = gen_v_footer(family), facts["copies"][1]
            for offset in (footer + 4, footer + 8, facts["blocks_end"], TRAINER):
                data = bytearray(gen_v_save(family))
                data[offset] ^= 1
                data[backup + offset] ^= 1
                with self.subTest(family=family, offset=hex(offset)), self.assertRaisesRegex(ValueError, "no valid copy"):
                    provision.provision(bytes(data), facts["targets"][0])
            # One valid copy of the footer and of the trainer data is enough.
            for offset in (facts["blocks_end"], TRAINER):
                data = bytearray(gen_v_save(family)); data[offset] ^= 1
                with self.subTest(family=family, valid_backup=hex(offset)):
                    self.assertTrue(provision.provision(bytes(data), facts["targets"][1]))

    def test_profile_rejects_injection_invalid_ports_and_missing_pins(self):
        good = {"hostname": "wt.example.invalid", "ipv4": "192.0.2.1", "port": 8443,
                "current_spki_sha256": "12" * 32, "next_spki_sha256": "34" * 32}
        output = connection.header(good)
        self.assertIn('"wt.example.invalid:8443"', output)
        self.assertIn("0xc0000201UL", output)
        for key, value in (("hostname", 'x"\n#define BYPASS 1'), ("hostname", "http://x"),
                           ("ipv4", "0.0.0.0"), ("ipv4", "224.0.0.1"), ("port", 0),
                           ("port", True), ("port", 65536), ("current_spki_sha256", "00" * 32),
                           ("next_spki_sha256", "12" * 32), ("next_spki_sha256", "xx" * 32)):
            with self.subTest(key=key, value=value), self.assertRaises((ValueError, TypeError)):
                connection.header({**good, key: value})

    def test_provision_only_changes_new_journal_records(self):
        original = synthetic_save()
        result = provision.provision(original, "us-rev1")
        another = provision.provision(original, "us-rev1")
        first, second = 0x30000, 0x70000
        self.assertEqual(result[first:first + 4096], result[second:second + 4096])
        self.assertEqual(result[:first], original[:first])
        self.assertEqual(result[first + 4096:second], original[first + 4096:second])
        self.assertEqual(result[second + 4096:], original[second + 4096:])
        record = result[first:first + 4096]
        self.assertEqual(record[4060:4092], hashlib.sha256(record[:4060]).digest())
        self.assertEqual(record[4092:], b"CMIT")
        self.assertNotEqual(record[1444:1476], another[first + 1444:first + 1476])
        with self.assertRaisesRegex(ValueError, "not blank"):
            provision.provision(result, "us-rev1")
        with self.assertRaises(ValueError):
            provision.provision(original, "diamond-us-rev5")
        with self.assertRaises(ValueError):
            provision.provision(original[:262144], "us-rev0")

    def test_save_footer_magic_follows_the_cartridge_language(self):
        # Korean cartridges mark save blocks with 0x20070903; every other language uses 0x20060623.
        layout = provision.layout_for("us-rev1")
        self.assertEqual(provision.footer_magic(layout, "us-rev1"), 0x20060623)
        self.assertEqual(provision.footer_magic(layout, "platinum-korea"), 0x20070903)
        self.assertIn("Korean", layout["languages"])
        self.assertTrue(provision.provision(synthetic_save(), "us-rev1"))
        with self.assertRaisesRegex(ValueError, "no valid copy"):
            provision.provision(synthetic_save(0x20070903), "us-rev1")

    def test_hgss_saves_are_checked_against_their_own_layout(self):
        hgss = bytearray(b"\xff" * 524288)
        for half in (0, 262144):
            for block, (offset, size) in enumerate(((0, 63016), (63232, 74512))):
                start = half + offset
                hgss[start:start + size - 16] = bytes(size - 16)
                crc = binascii.crc_hqx(hgss[start:start + size - 16], 0xffff)
                struct.pack_into("<IIIHH", hgss, start + size - 16, 1, size, 0x20060623, block, crc)
        for target in ("heartgold-us-rev0", "soulsilver-us-rev0"):
            with self.subTest(target=target):
                result = provision.provision(bytes(hgss), target)
                self.assertEqual(result[0x30000:0x31000], result[0x70000:0x71000])
                self.assertEqual(result[:0x30000], bytes(hgss[:0x30000]))
                # A Platinum footer layout is not an HGSS one, and vice versa.
                with self.assertRaisesRegex(ValueError, "no valid copy"):
                    provision.provision(synthetic_save(), target)
        with self.assertRaisesRegex(ValueError, "no valid copy"):
            provision.provision(bytes(hgss), "us-rev1")

    def test_dp_saves_are_checked_against_their_own_layout(self):
        dp = bytearray(b"\xff" * 524288)
        for half in (0, 262144):
            for block, (offset, size) in enumerate(((0, 49408), (49408, 74208))):
                start = half + offset
                dp[start:start + size - 20] = bytes(size - 20)
                crc = binascii.crc_hqx(dp[start:start + size - 20], 0xffff)
                struct.pack_into("<IIIIBxH", dp, start + size - 20, 1, 1, size, 0x20060623, block, crc)
        for target in ("diamond-us-rev5", "pearl-us-rev5"):
            with self.subTest(target=target):
                result = provision.provision(bytes(dp), target)
                self.assertEqual(result[0x30000:0x31000], result[0x70000:0x71000])
                self.assertEqual(result[:0x30000], bytes(dp[:0x30000]))
                # Platinum shares the footer format but not the block offsets.
                with self.assertRaisesRegex(ValueError, "no valid copy"):
                    provision.provision(synthetic_save(), target)
        with self.assertRaisesRegex(ValueError, "no valid copy"):
            provision.provision(bytes(dp), "us-rev1")

    def test_corrupt_retail_or_nonblank_reserved_sectors_are_preserved(self):
        for offset in (0, 53036, 0x31000):
            data = bytearray(synthetic_save())
            data[offset] ^= 1
            if offset < 53037:
                data[262144 + offset] ^= 1
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                provision.provision(bytes(data), "us-rev0")

    def test_provision_output_cannot_replace_a_file_symlink_or_hardlink(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / "original.sav"
            source.write_bytes(b"preserve")
            linked = root / "linked.sav"
            linked.symlink_to(source)
            hardlink = root / "hard.sav"
            hardlink.hardlink_to(source)
            for path in (source, linked, hardlink):
                with self.assertRaises(FileExistsError):
                    provision.write_new(path, b"changed")
            self.assertEqual(source.read_bytes(), b"preserve")
            output = root / "new.sav"
            provision.write_new(output, b"new")
            self.assertEqual(output.stat().st_mode & 0o777, 0o600)

    def test_tls_cache_detects_edits_and_escaping_symlinks(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "client/core").mkdir(parents=True)
            (root / "client/core/tls-dependency.json").write_text(json.dumps({"version": "test", "sha256": "expected"}))
            source = root / "game/mbedtls-test"
            source.mkdir(parents=True)
            (source / "source.c").write_text("original")
            (source / "alias.c").symlink_to("source.c")
            recorded = {"archive_sha256": "expected", "files": prepare.inventory(source)}
            (source / ".dswt-files.json").write_text(json.dumps(recorded))
            self.assertEqual(prepare.prepare(root), source)
            (source / "source.c").write_text("changed")
            with self.assertRaisesRegex(ValueError, "differs"):
                prepare.prepare(root)
            (source / "escape").symlink_to(root / "client")
            with self.assertRaisesRegex(ValueError, "escapes"):
                prepare.inventory(source)


if __name__ == "__main__":
    unittest.main()
