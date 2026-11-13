#!/usr/bin/env python3
"""DSWT patcher: add Wonder Trade to a clean dump of your own Gen IV Pokémon cartridge.

Run without arguments for the window. `--input ROM [--output FILE]` patches from the
command line, and `--self-test` checks the embedded release bundle.
"""

import argparse
import json
from pathlib import Path
import queue
import sys
import threading

FROZEN = getattr(sys, "frozen", False)
HERE = Path(getattr(sys, "_MEIPASS", Path(__file__).resolve().parent))
if not FROZEN:
    sys.path.insert(0, str(HERE.parent / "bps-patch"))
import bps  # noqa: E402
import bundle  # noqa: E402
import menu_style as style  # noqa: E402

SAVE_ADVICE = ("Back up your save before your first Wonder Trade. Your emulator finds a save by the ROM's "
               "file name, so give the patched ROM the name your save uses. INSTALL.md has the details.")


def default_bundle():
    return HERE / "bundle" if FROZEN else None


def load(bundle_dir):
    if bundle_dir is None:
        raise bundle.BundleError("No release bundle: pass --bundle DIR")
    return bundle.load_manifest(bundle.read_file(Path(bundle_dir) / "manifest.json", bundle.MAX_MANIFEST, "Manifest"))


def identify(manifest, rom_path):
    """Return the manifest entry a ROM would use, or raise BundleError explaining why it cannot."""
    entry, _ = bundle.select(manifest, bundle.read_file(rom_path, bps.MAX_FILE, "ROM"))
    return entry


def self_test(bundle_dir):
    """Check the manifest and that every patch matches it and walks cleanly."""
    manifest = load(bundle_dir)
    for entry in manifest["entries"]:
        patch = bundle.read_file(Path(bundle_dir) / entry["patch"]["file"], bps.MAX_PATCH, "Patch")
        if len(patch) != entry["patch"]["size"] or bundle.sha256(patch) != entry["patch"]["sha256"]:
            raise bundle.BundleError(f"{entry['patch']['file']} does not match the manifest")
        report = bps.walk(patch)
        if report["target"]["crc32"] != entry["output"]["crc32"]:
            raise bundle.BundleError(f"{entry['patch']['file']} does not produce the recorded output")
    try:
        import tkinter  # noqa: F401 - a build without Tk would only offer the command line
        window = True
    except ImportError:
        window = False
    return {"version": manifest["version"], "variants": [entry["variant"] for entry in manifest["entries"]],
            "window": window}


class Window:
    """A DS-style menu: choose a ROM, see what it is, save the patched copy.

    Everything is drawn on a 256x192 canvas scaled by a whole number, using the
    original font and frames in menu_style. Arrow keys move the cursor; Enter, Z
    or Space is A; Escape, X or Backspace is B; the mouse works too.
    """

    ITEMS = ("CHOOSE ROM", "SAVE PATCHED ROM", "HELP", "EXIT")
    MENU = (128, 22, 124, 74)       # x, y, width, height of the menu panel
    INFO = (4, 22, 120, 114)
    MESSAGE = (4, 140, 248, 48)
    TEXT_WIDTH = MESSAGE[2] - 40  # leaves room for the page arrow

    def __init__(self, bundle_dir):
        import tkinter as tk
        from tkinter import filedialog
        _dpi_aware()
        self.filedialog = filedialog
        self.bundle_dir = bundle_dir
        self.manifest = load(bundle_dir)
        self.results = queue.Queue()
        self.rom = self.entry = self.shown = None
        self.outcome = "Waiting"
        self.selected, self.busy, self.pages, self.page, self.kind = 0, False, [[""]], 0, "text"
        self.root = tk.Tk()
        self.root.title(f"DSWT Patcher {self.manifest['version']}")
        self.root.resizable(False, False)
        screen = self.root.winfo_screenheight()
        self.scale = max(2, min(5, int(screen * 0.7) // style.SCREEN[1]))
        width, height = (side * self.scale for side in style.SCREEN)
        self.canvas = tk.Canvas(self.root, width=width, height=height, highlightthickness=0, bd=0,
                                background=style.PALETTE["stripe_a"])
        self.canvas.pack()
        for keys, action in ((("<Up>", "<KP_Up>"), lambda: self.move(-1)), (("<Down>", "<KP_Down>"), lambda: self.move(1)),
                             (("<Return>", "<KP_Enter>", "<z>", "<space>"), self.press_a),
                             (("<Escape>", "<x>", "<BackSpace>"), self.press_b)):
            for key in keys:
                self.root.bind(key, lambda _event, action=action: action())
        self.canvas.bind("<Motion>", self.hover)
        self.canvas.bind("<Button-1>", self.click)
        self.say("Choose a clean dump of your cartridge.\nYour original file is never changed.")

    # State --------------------------------------------------------------------------------
    def enabled(self, index):
        return not (self.ITEMS[index] == "SAVE PATCHED ROM" and self.entry is None)

    def say(self, text, kind="text"):
        self.pages, self.page, self.kind = style.pages(text, self.TEXT_WIDTH), 0, kind
        self.draw()

    @property
    def message(self):
        return " ".join(line for page in self.pages for line in page)

    def move(self, step):
        if self.busy:
            return
        self.selected = (self.selected + step) % len(self.ITEMS)
        self.draw()

    def press_a(self):
        if self.busy:
            return
        if self.page + 1 < len(self.pages):
            self.page += 1
            self.draw()
            return
        if not self.enabled(self.selected):
            self.say("Choose a ROM first.")
            return
        {"CHOOSE ROM": self.pick, "SAVE PATCHED ROM": self.save, "HELP": self.help,
         "EXIT": self.root.destroy}[self.ITEMS[self.selected]]()

    def press_b(self):
        if self.page + 1 < len(self.pages):
            self.press_a()
        elif not self.busy:
            self.selected = len(self.ITEMS) - 1
            self.draw()

    def item_at(self, x, y):
        x, y = x // self.scale, y // self.scale
        left, top, width, _ = self.MENU
        index = (y - top - 6) // 16
        if left <= x < left + width and 0 <= index < len(self.ITEMS) and y >= top + 6:
            return index
        return None

    def hover(self, event):
        index = self.item_at(event.x, event.y)
        if index is not None and index != self.selected and not self.busy:
            self.selected = index
            self.draw()

    def click(self, event):
        index = self.item_at(event.x, event.y)
        if index is not None:  # a menu choice acts at once, even mid-message
            self.selected, self.page = index, len(self.pages) - 1
            self.press_a()
        elif self.page + 1 < len(self.pages):
            self.press_a()

    # Work ---------------------------------------------------------------------------------
    def work(self, text, task, finish):
        """Run task off the UI thread; finish(result, error) runs back on it."""
        self.busy = True
        self.say(text)

        def run():
            try:
                self.results.put((finish, task(), None))
            except (OSError, ValueError) as error:
                self.results.put((finish, None, error))
        threading.Thread(target=run, daemon=True).start()
        self.root.after(50, self.poll)

    def poll(self):
        try:
            finish, result, error = self.results.get_nowait()
        except queue.Empty:
            self.root.after(50, self.poll)
            return
        self.busy = False
        finish(result, error)

    def pick(self):
        path = self.filedialog.askopenfilename(title="Choose your Pokémon ROM",
                                               filetypes=[("Nintendo DS ROM", "*.nds"), ("All files", "*")])
        if not path:
            return
        self.rom = self.entry = None
        self.shown, self.outcome = path, "Checking..."
        self.work("Checking your ROM...", lambda: identify(self.manifest, path),
                  lambda entry, error: self.identified(path, entry, error))

    def identified(self, path, entry, error):
        if error:
            self.selected, self.outcome = 0, "Refused"
            self.say(str(error), "error")
            return
        self.rom, self.entry, self.selected, self.outcome = path, entry, 1, "Ready"
        self.say(f"This is {bundle.title(entry)}. Choose SAVE PATCHED ROM to add Wonder Trade.")

    def save(self):
        suggestion = bundle.default_output(self.rom)
        output = self.filedialog.asksaveasfilename(title="Save the patched ROM", initialdir=str(suggestion.parent),
                                                   initialfile=suggestion.name, defaultextension=".nds",
                                                   confirmoverwrite=False, filetypes=[("Nintendo DS ROM", "*.nds")])
        if not output:
            return
        self.work("Patching... Your original file is not changed.",
                  lambda: bundle.patch_file(self.bundle_dir, self.rom, output), self.saved)

    def saved(self, result, error):
        if error:
            self.say(str(error), "error")
            return
        self.selected, self.outcome = len(self.ITEMS) - 1, "Patched"
        self.say(f"Saved {Path(result[1]).name}.\n\n{SAVE_ADVICE}", "done")

    def help(self):
        supported = ", ".join(bundle.title(entry) for entry in self.manifest["entries"])
        self.say("DSWT adds Wonder Trade to your own copy of a supported Pokémon game.\n\n"
                 f"Supported: {supported}. Trimmed dumps work; encrypted dumps do not.\n\n{SAVE_ADVICE}")

    # Drawing ------------------------------------------------------------------------------
    def rect(self, x, y, width, height, colour):
        s = self.scale
        colour = style.PALETTE.get(colour, colour)
        self.drawn.append((x, y, width, height, colour))
        self.canvas.create_rectangle(x * s, y * s, (x + width) * s, (y + height) * s, fill=colour, width=0)

    def snapshot(self, path, scale=3):
        """Write exactly what is drawn as a PNG, for review without a screenshot."""
        import tkinter as tk
        width, height = style.SCREEN
        image = tk.PhotoImage(master=self.root, width=width * scale, height=height * scale)
        for x, y, w, h, colour in self.drawn:
            image.put(colour, to=(x * scale, y * scale, (x + w) * scale, (y + h) * scale))
        image.write(str(path), format="png")

    def text(self, text, x, y, colour="text", shadow="shadow"):
        for run_x, run_y, length in style.text_runs(text, x + 1, y + 1):
            self.rect(run_x, run_y, length, 1, shadow)
        for run_x, run_y, length in style.text_runs(text, x, y):
            self.rect(run_x, run_y, length, 1, colour)

    def panel(self, box, border="border"):
        for x, y, width, height, colour in style.frame(*box, border):
            self.rect(x, y, width, height, colour)

    def draw(self):
        self.canvas.delete("all")
        width, height = style.SCREEN
        self.drawn = []
        self.rect(0, 0, width, height, "stripe_a")
        for y in range(16, height):  # diagonal stripes, drawn as runs so every pixel stays square
            for start in range((-y) % 16 - 16, width, 16):  # pixels where (x + y) % 16 < 8
                left, right = max(start, 0), min(start + 8, width)
                if right > left:
                    self.rect(left, y, right - left, 1, "stripe_b")
        self.rect(0, 0, width, 16, "title")
        self.text("DSWT PATCHER", 6, 4, "title_text", "title_shadow")
        version = f"v{self.manifest['version']}"
        self.text(version, width - 6 - style.measure(version), 4, "title_text", "title_shadow")

        self.panel(self.INFO)
        x, y = self.INFO[0] + 8, self.INFO[1] + 8
        inner = self.INFO[2] - 16
        self.text("ROM", x, y, "label")
        name = style.wrap(Path(self.shown).name if self.shown else "None chosen", inner)
        for index, line in enumerate(name[:2]):
            self.text(line if index == 0 or len(name) <= 2 else line[:-3] + "...", x, y + 11 + 10 * index)
        self.text("VERSION", x, y + 38, "label")
        # The game on one line and its version on the next, such as "HeartGold" and "USA Rev 0".
        game = bundle.GAMES[self.entry["game_code"][:3]][0].removeprefix("Pokémon ") if self.entry else "-"
        self.text(game, x, y + 49)
        self.text(self.entry["name"] if self.entry else "", x, y + 59)
        self.text("STATUS", x, y + 74, "label")
        self.text(self.outcome, x, y + 85, {"Refused": "error", "Patched": "done"}.get(self.outcome, "text"))

        self.panel(self.MENU)
        left, top = self.MENU[0], self.MENU[1]
        for index, item in enumerate(self.ITEMS):
            item_y = top + 9 + 16 * index
            colours = ("text", "shadow") if self.enabled(index) else ("dim", "dim_shadow")
            self.text(item, left + 18, item_y, *colours)
            if index == self.selected and not self.busy:
                for run_x, run_y, length in style.cursor(left + 9, item_y):
                    self.rect(run_x, run_y, length, 1, "cursor")

        self.panel(self.MESSAGE, "message_border")
        colour = {"error": "error", "done": "done"}.get(self.kind, "text")
        for index, line in enumerate(self.pages[self.page]):
            self.text(line, self.MESSAGE[0] + 12, self.MESSAGE[1] + 12 + 16 * index, colour)
        if self.page + 1 < len(self.pages):
            for run_x, run_y, length in style.more_arrow(self.MESSAGE[0] + self.MESSAGE[2] - 18, self.MESSAGE[1] + 34):
                self.rect(run_x, run_y, length, 1, "cursor")

    def run(self):
        self.root.mainloop()


def _dpi_aware():
    """Let Windows draw the pixels crisply instead of stretching a blurry bitmap."""
    if sys.platform == "win32":
        try:
            import ctypes
            ctypes.windll.shcore.SetProcessDpiAwareness(1)
        except (AttributeError, OSError):
            pass


def report(text, window=False):
    """Print, or show a dialog when a windowed build has no console to print to."""
    if sys.stderr is not None:
        print(text, file=sys.stderr)
    elif window:
        try:
            import tkinter
            from tkinter import messagebox
            tkinter.Tk().withdraw()
            messagebox.showerror("DSWT Patcher", text)
        except Exception:  # noqa: BLE001 - nothing else can report this
            pass


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--bundle", type=Path, default=default_bundle(), help=argparse.SUPPRESS if FROZEN else
                        "release bundle directory holding manifest.json and the patches")
    parser.add_argument("--input", type=Path, help="ROM to patch; omit to open the window")
    parser.add_argument("--output", type=Path, help="new file to write (default: beside the input)")
    parser.add_argument("--self-test", action="store_true", help="check the release bundle and exit")
    args = parser.parse_args(argv)
    if args.output and not args.input:
        parser.error("--output needs --input")
    window = not (args.self_test or args.input)
    try:
        if args.self_test:
            result = json.dumps(self_test(args.bundle))
        elif args.input:
            entry, output = bundle.patch_file(args.bundle, args.input, args.output)
            result = f"Patched {bundle.title(entry)}: {output}\n{SAVE_ADVICE}"
        else:
            Window(args.bundle).run()
            return 0
    except (OSError, ValueError) as error:
        report(f"DSWT patcher: {error}", window)
        return 1
    except KeyboardInterrupt:
        report("Interrupted; your ROM was not changed and no partial file was left.")
        return 130
    if sys.stdout is not None:
        print(result)
    return 0


if __name__ == "__main__":
    sys.exit(main())
