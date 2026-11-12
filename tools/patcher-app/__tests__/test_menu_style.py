import os
from pathlib import Path
import sys
import unittest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE.parents[1] / "bps-patch"))
import bundle
import dswt_patcher
import menu_style as style


class FontTests(unittest.TestCase):
    def test_every_glyph_is_a_clean_rectangle(self):
        for char, rows in style.FONT.items():
            with self.subTest(char=char):
                self.assertEqual(len(rows), style.LINE)
                self.assertEqual(len({len(row) for row in rows}), 1)
                self.assertTrue(all(set(row) <= {".", "#"} for row in rows))
                self.assertFalse(any("#" in row for row in rows[7:]) and char not in "gjpqy,;_",
                                 "only descender glyphs reach below the baseline")

    def test_covers_what_the_app_and_its_messages_say(self):
        text = " ".join([
            *dswt_patcher.Window.ITEMS, dswt_patcher.SAVE_ADVICE, "DSWT PATCHER v0.1.0-rc.1 Checking your ROM...",
            "Pokémon HeartGold (France, French, Rev 1) is not supported yet. Supported: Pokémon Platinum USA Rev 0.",
            f"This file is not a {bundle.GAME_NAMES} ROM.",
            "Trimmed dumps work; encrypted dumps do not. C:\\Users\\Ash\\Platinum [!] (Wonder Trade).nds",
            "/home/ash/roms/platinum_us-rev1.nds already exists; choose a new file name",
            *(describe for code in bundle.CARTRIDGES for describe in bundle.CARTRIDGES[code]),
            "abcdefghijklmnopqrstuvwxyz ABCDEFGHIJKLMNOPQRSTUVWXYZ 0123456789 ’'\"%&+=~*#@<>",
        ])
        self.assertEqual(sorted({char for char in text if char not in style.FONT}), [])
        self.assertEqual(style.glyph("ポ"), style.MISSING)

    def test_measure_matches_the_runs_drawn(self):
        for text in ("A", "Wonder Trade", "i.l!"):
            runs = style.text_runs(text, 10, 20)
            self.assertEqual(max(x + width for x, _, width in runs) - 10, style.measure(text))
        self.assertEqual(style.measure(""), 0)


class LayoutTests(unittest.TestCase):
    def test_wrap_keeps_every_line_inside_the_box(self):
        width = dswt_patcher.Window.TEXT_WIDTH
        text = ("Saved /home/someone/a/very/long/path/without/any/spaces/at/all/Platinum (Wonder Trade).nds.\n\n"
                + dswt_patcher.SAVE_ADVICE)
        lines = style.wrap(text, width)
        self.assertTrue(all(style.measure(line) <= width for line in lines))
        self.assertEqual("".join(lines).replace(" ", ""), text.replace(" ", "").replace("\n", ""))

    def test_pages_hold_two_lines_and_split_paragraphs(self):
        pages = style.pages("One.\n\nTwo three four five six seven eight nine ten", 40)
        self.assertEqual(pages[0], ["One."])
        self.assertTrue(all(1 <= len(page) <= 2 for page in pages))
        self.assertEqual(style.pages("", 40), [[""]])

    def test_panels_fit_the_screen_and_do_not_overlap(self):
        window = dswt_patcher.Window
        width, height = style.SCREEN
        boxes = [window.MENU, window.INFO, window.MESSAGE]
        for x, y, w, h in boxes:
            self.assertTrue(0 <= x and x + w <= width and 16 <= y and y + h <= height)
            for part in style.frame(x, y, w, h, "border"):
                self.assertTrue(x <= part[0] and part[0] + part[2] <= x + w and y <= part[1] and part[1] + part[3] <= y + h)
        for index, (ax, ay, aw, ah) in enumerate(boxes):
            for bx, by, bw, bh in boxes[index + 1:]:
                self.assertTrue(ax + aw <= bx or bx + bw <= ax or ay + ah <= by or by + bh <= ay)
        menu_width = window.MENU[2]
        self.assertTrue(all(18 + style.measure(item) <= menu_width - 6 for item in window.ITEMS))
        self.assertEqual([width for _, _, width in style.cursor(0, 0)], [1, 2, 3, 4, 3, 2, 1])


def _display():
    try:
        import tkinter
        tkinter.Tk().destroy()
        return True
    except Exception:  # noqa: BLE001 - no Tk or no display
        return False


@unittest.skipUnless(os.environ.get("DSWT_TEST_WINDOW") and _display(), "set DSWT_TEST_WINDOW with Tk and a display")
class WindowTests(unittest.TestCase):
    def test_keyboard_drives_the_menu(self):
        sys.path.insert(0, str(HERE.parents[1] / "bps-patch/__tests__"))
        from test_app import AppFixture

        class Case(AppFixture):
            def runTest(inner):
                window = dswt_patcher.Window(inner.bundle)
                window.root.withdraw()
                self.assertEqual(len(window.pages), 1, "the opening message fits one box")
                window.move(1)
                self.assertEqual(window.ITEMS[window.selected], "SAVE PATCHED ROM")
                window.press_a()
                self.assertIn("Choose a ROM first", window.message)
                window.press_b()
                self.assertEqual(window.ITEMS[window.selected], "EXIT")
                window.root.destroy()
        result = unittest.TextTestRunner(stream=open(os.devnull, "w")).run(Case())
        self.assertTrue(result.wasSuccessful(), result.errors + result.failures)


if __name__ == "__main__":
    unittest.main()
