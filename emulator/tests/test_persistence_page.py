"""Each supported cartridge gets its family's virtual persistence page, and no other game gets one."""

import argparse
import json
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
GEN4 = 0x2FF00  # client/include/wonder_trade/persistence.h


class PersistencePageTests(unittest.TestCase):
    def addresses(self, codes):
        output = subprocess.run([str(self.probe), *codes], capture_output=True, text=True, timeout=5, check=True).stdout
        return {code: int(address) for code, address in (line.split() for line in output.splitlines())}

    def test_every_catalogued_cartridge_uses_its_family_page(self):
        catalog = json.loads((ROOT / "patches/compatibility.json").read_text())
        b2w2 = json.loads((ROOT / "protocol/save-layout-b2w2.json").read_text())["persistence_address"]
        expected = {}
        for variant in catalog["variants"]:
            expected[variant["game_code"]] = b2w2 if variant["game_code"][:2] == "IR" else GEN4
        self.assertIn(b2w2, expected.values())
        self.assertEqual(self.addresses(expected), expected)

    def test_other_games_and_unverified_gen_v_cartridges_get_no_page(self):
        codes = ("IRBO", "IRAO", "IREJ", "IRDK", "IREF", "####", "AMHE", "AAAA")
        self.assertEqual(set(self.addresses(codes).values()), {0})


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe", type=Path)
    args = parser.parse_args()
    PersistencePageTests.probe = args.probe.resolve()
    unittest.main(argv=[__file__], verbosity=2)
