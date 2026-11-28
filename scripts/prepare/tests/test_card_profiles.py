"""Every catalog addition needs a matching launcher-handoff profile."""
import json
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[3]


class CardProfileCoverage(unittest.TestCase):
    def test_every_catalog_build_has_one_profile(self):
        catalog = json.loads((ROOT / "client/games/compatibility.json").read_text())
        profiles = (ROOT / "client/companion/src/platform/nds/card_profiles.inc").read_text()
        names = re.findall(r"^    /\* ([\w-]+) \*/$", profiles, re.MULTILINE)
        self.assertCountEqual(names, [entry["id"] for entry in catalog["variants"]])
