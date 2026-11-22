from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
# A wide string literal, such as the Generation V adapters' L"..." messages.
WIDE = re.compile(r'(?<![\w"])L"((?:[^"\\\n]|\\.)*)"')
HEX = re.compile(r"\\x([0-9A-Fa-f]*)")


def wide_literals():
    for source in sorted([*(ROOT / "client/src").rglob("*.[ch]"), *(ROOT / "client/include").rglob("*.h")]):
        for number, line in enumerate(source.read_text(encoding="utf-8").splitlines(), 1):
            for literal in WIDE.findall(line):
                yield f"{source.relative_to(ROOT)}:{number}", literal


class WideTextTests(unittest.TestCase):
    def test_wide_literals_exist(self):
        self.assertTrue(any(True for _ in wide_literals()))

    def test_wide_literals_hold_only_ascii(self):
        """The compiler reads source as bytes: a UTF-8 ’ would become three characters on screen."""
        for where, literal in wide_literals():
            with self.subTest(where=where):
                self.assertTrue(literal.isascii(), f"write {literal!r} with \\x escapes")

    def test_wide_hex_escapes_are_one_character(self):
        """A hex escape takes every hex digit after it, so each is written with exactly four."""
        for where, literal in wide_literals():
            for digits in HEX.findall(literal):
                with self.subTest(where=where, escape=digits):
                    self.assertEqual(len(digits), 4)


if __name__ == "__main__":
    unittest.main()
