from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
SDK = re.compile(r"(std\w+|string|stdio)\.h|nitro\.h|nnsys/fnd\.h|nitroWiFi/\w+\.h|dwc\.h|(mbedtls|psa)/[\w/]+\.h")


class SharedCoreTests(unittest.TestCase):
    def test_core_uses_no_game_headers(self):
        """Only the game adapter subdirectories may include decompilation headers."""
        sources = [*sorted((ROOT / "client/src/wonder_trade").glob("*.c")),
                   *sorted((ROOT / "client/include/wonder_trade").glob("*.h"))]
        self.assertTrue(sources)
        for source in sources:
            for quoted, angled in re.findall(r'^\s*#\s*include\s*(?:"([^"]+)"|<([^>]+)>)', source.read_text(), re.M):
                with self.subTest(source=source.name, include=quoted or angled):
                    if quoted:
                        self.assertTrue(quoted == "wonder_trade_config.h" or (
                            quoted.startswith("wonder_trade/") and quoted.count("/") == 1))
                    else:
                        self.assertRegex(angled, SDK)


ADAPTERS = {"platinum": False, "hgss": False, "dp": False, "bw": True, "bw2": True}
OPS = ("WtFlash_Hash", "PartyCount", "Slot", "Occupied", "SaveHash", "SaveVerify", "SaveCheckpoint",
       "Copy", "(?:Discard|Drop)", "(?:Commit|Install)", "SaveCounters", "Build", "Empty")


def function(source, signature):
    """The body of the function whose definition starts with signature."""
    start = source.index(signature)
    end = source.index("\n}\n", start)
    return source[start:end]


class AdapterDeliveryTests(unittest.TestCase):
    """Every game adapter decides deliveries, recoveries and uniqueness with the
    shared code in wonder_trade/delivery.h, which client/tests/delivery_test.c
    drives through the engine. The adapters' Nitro code cannot run on a host,
    so this checks that each one still hands those decisions over: deleting a
    call here turns a test red."""

    def test_adapters_route_decisions_to_shared_code(self):
        for game, battle_box in ADAPTERS.items():
            with self.subTest(game=game):
                source = (ROOT / f"client/src/wonder_trade/{game}/game_save.c").read_text()
                self.assertIn('#include "wonder_trade/delivery.h"', source)
                inventory = function(source, "int WtGame_Inventory(")
                self.assertIn("!WtDelivery_Bound(entry)) return -1;", inventory)
                self.assertRegex(inventory, r"if \(step != WT_INVENTORY_PREPARE_REMOVE && step != WT_INVENTORY_REMOVE"
                                            r" && step != WT_INVENTORY_RESTORE\)\s+return WtDelivery_Step\(&ops, step, entry\);")
                self.assertRegex(inventory, r"if \(WtDelivery_Unique\(&ops, candidate, entry->outgoing\) != 0")
                self.assertIn("WtDelivery_Unique(&ops, candidate, canonical) == 0", function(source, "BOOL WtGameSave_Unique("))
                self.assertIn("WtDelivery_CanLeave(&ops, entry)", function(source, "int WtGame_CanLeave("))
                self.assertRegex(source, r"const WtDeliveryOps ops0 = \{\s*NULL, NULL, " + r",\s*".join(OPS) + r"\s*\};")
                self.assertIn("WtLocatorsMatchGame", source)
                destination = function(source, "static unsigned char *Destination(")
                self.assertIn("where.kind == 1", destination)
                self.assertIn("where.kind == 2", destination)
                self.assertEqual("where.kind == 3" in destination, battle_box)
                # Nothing decides a delivery or an absence on its own any more.
                for name in ("Absent", "ScanOutgoing", "PrepareDelivery", "VerifyReceived", "Applied", "Removed",
                             "Behind", "Receive"):
                    self.assertNotRegex(source, rf"\bstatic int {name}\(")


if __name__ == "__main__":
    unittest.main()
