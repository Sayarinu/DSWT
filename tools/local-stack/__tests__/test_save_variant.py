import importlib.util
from pathlib import Path
import struct
import unittest


DIRECTORY = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("save_variant", DIRECTORY / "save_variant.py")
variant = importlib.util.module_from_spec(spec)
spec.loader.exec_module(variant)
fixture = variant.stack.load_module("variant_fixture", variant.stack.ROOT / "scripts/tests/test_verify_demo_trade.py")


def party(raw, bank, slot):
    start = bank + variant.PARTY + 8 + slot * variant.SLOT
    data = raw[start:start + variant.SLOT]
    canonical = variant.VERIFY.canonical(bytes(data[:variant.BOX]))
    return canonical, variant.stream(data[variant.BOX:], struct.unpack_from("<I", canonical)[0])


class SaveVariantTests(unittest.TestCase):
    def setUp(self):
        self.raw = bytes(fixture.retail([fixture.pk(10, 0x12345678), fixture.pk(20, 0x9ABCDEF0)], fixture.pk(30, 3)))

    def test_unchanged_slots_round_trip_and_both_copies_receive_valid_edits(self):
        edits = {1: {"egg": True, "hp": 0, "item": 137, "capsule": 2, "move": [0, 147]}}
        result = variant.variant(self.raw, edits)
        for bank in (0, 0x40000):
            variant.VERIFY.inventory(result, bank)  # Footer CRCs and checksums still verify.
            self.assertEqual(party(result, bank, 0), party(self.raw, bank, 0))
            canonical, extension = party(result, bank, 1)
            self.assertTrue(struct.unpack_from("<I", canonical, 0x38)[0] >> 30 & 1)
            self.assertEqual(struct.unpack_from("<H", canonical, 0x0A)[0], 137)
            self.assertEqual(struct.unpack_from("<H", canonical, 0x28)[0], 147)
            self.assertEqual((struct.unpack_from("<H", extension, 0x8E - variant.BOX)[0], extension[0x8D - variant.BOX]), (0, 2))
            self.assertEqual(struct.unpack_from("<H", canonical, 8)[0], 20)  # Species and identity untouched.
        self.assertEqual(variant.variant(self.raw, {1: {"egg": False}}), self.raw)  # A no-op edit is byte-exact.

    def test_invalid_edits_and_slots_are_refused(self):
        for text in ('{}', '{"0": {}}', '{"0": {"shiny": true}}', '{"0": {"hp": -1}}', '{"0": {"item": 999}}',
                     '{"0": {"egg": 1}}', '{"0": {"move": [4, 1]}}', '{"0": {"move": [0, 0]}}'):
            with self.subTest(text=text), self.assertRaises(ValueError): variant.parse(text)
        with self.assertRaisesRegex(ValueError, "occupied"): variant.variant(self.raw, {2: {"hp": 0}})
