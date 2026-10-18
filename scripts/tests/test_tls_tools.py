import binascii
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


def module(name):
    spec = importlib.util.spec_from_file_location(name.replace("-", "_"), ROOT / "scripts" / f"{name}.py")
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


connection = module("configure-connection")
provision = module("provision-save")
prepare = module("prepare-tls")


def synthetic_save():
    data = bytearray(b"\xff" * 524288)
    for half in (0, 262144):
        for block, (offset, size) in enumerate(((0, 53036), (53036, 74212))):
            start = half + offset
            data[start:start + size - 20] = bytes(size - 20)
            crc = binascii.crc_hqx(data[start:start + size - 20], 0xffff)
            struct.pack_into("<IIIIBxH", data, start + size - 20, 1, 1, size, 0x20060623, block, crc)
    return bytes(data)


class TlsToolTests(unittest.TestCase):
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
            provision.provision(original, "europe")
        with self.assertRaises(ValueError):
            provision.provision(original[:262144], "us-rev0")

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
            (root / "client").mkdir()
            (root / "client/tls-dependency.json").write_text(json.dumps({"version": "test", "sha256": "expected"}))
            source = root / "game/mbedtls-test"
            source.mkdir(parents=True)
            (source / "source.c").write_text("original")
            (source / "alias.c").symlink_to("source.c")
            recorded = {"archive_sha256": "expected", "files": prepare.inventory(source)}
            (source / ".platinumwt-files.json").write_text(json.dumps(recorded))
            self.assertEqual(prepare.prepare(root), source)
            (source / "source.c").write_text("changed")
            with self.assertRaisesRegex(ValueError, "differs"):
                prepare.prepare(root)
            (source / "escape").symlink_to(root / "client")
            with self.assertRaisesRegex(ValueError, "escapes"):
                prepare.inventory(source)


if __name__ == "__main__":
    unittest.main()
