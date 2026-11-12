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


if __name__ == "__main__":
    unittest.main()
