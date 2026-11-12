#!/usr/bin/env python3
"""DSWT patcher: add Wonder Trade to a clean dump of your own Pokémon Platinum cartridge.

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
    """One-screen patcher: choose a ROM, see what it is, save the patched copy."""

    def __init__(self, bundle_dir):
        import tkinter as tk
        from tkinter import filedialog, ttk
        self.tk, self.filedialog = tk, filedialog
        self.bundle_dir = bundle_dir
        self.manifest = load(bundle_dir)
        self.results = queue.Queue()
        self.rom = None
        self.root = tk.Tk()
        self.root.title(f"DSWT Patcher {self.manifest['version']}")
        self.root.resizable(False, False)
        frame = ttk.Frame(self.root, padding=16)
        frame.grid()
        ttk.Label(frame, text="Add Wonder Trade to Pokémon Platinum", font=("TkDefaultFont", 13, "bold")).grid(
            sticky="w", columnspan=2)
        supported = ", ".join(entry["name"] for entry in self.manifest["entries"])
        ttk.Label(frame, wraplength=440, justify="left", text=(
            "Choose a clean dump of your own cartridge (.nds). Your original file is never changed.\n"
            f"Supported: {supported}.")).grid(sticky="w", columnspan=2, pady=(6, 12))
        self.choose = ttk.Button(frame, text="Choose ROM…", command=self.pick)
        self.choose.grid(row=2, column=0, sticky="w")
        self.patch_button = ttk.Button(frame, text="Save patched ROM…", command=self.save, state="disabled")
        self.patch_button.grid(row=2, column=1, sticky="e")
        self.status = tk.StringVar(value="No ROM chosen.")
        self.label = tk.Label(frame, textvariable=self.status, wraplength=440, justify="left", anchor="w")
        self.label.grid(sticky="we", columnspan=2, pady=(12, 0))
        self.normal = self.label.cget("fg")

    def show(self, text, kind="info"):
        self.status.set(text)
        self.label.configure(fg={"error": "#b00020", "done": "#1b5e20"}.get(kind, self.normal))

    def busy(self, text):
        self.show(text)
        self.choose.configure(state="disabled")
        self.patch_button.configure(state="disabled")

    def work(self, task, finish):
        """Run task off the UI thread; finish(result, error) runs back on it."""
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
        self.choose.configure(state="normal")
        finish(result, error)

    def pick(self):
        path = self.filedialog.askopenfilename(title="Choose your Pokémon Platinum ROM",
                                               filetypes=[("Nintendo DS ROM", "*.nds"), ("All files", "*")])
        if not path:
            return
        self.rom = None
        self.busy("Checking your ROM…")
        self.work(lambda: identify(self.manifest, path), lambda entry, error: self.identified(path, entry, error))

    def identified(self, path, entry, error):
        if error:
            self.show(str(error), "error")
            return
        self.rom = path
        self.patch_button.configure(state="normal")
        self.show(f"{Path(path).name}: {entry['name']}, ready to patch.")

    def save(self):
        suggestion = bundle.default_output(self.rom)
        output = self.filedialog.asksaveasfilename(title="Save the patched ROM", initialdir=str(suggestion.parent),
                                                   initialfile=suggestion.name, defaultextension=".nds",
                                                   confirmoverwrite=False, filetypes=[("Nintendo DS ROM", "*.nds")])
        if not output:
            return
        self.busy("Patching…")
        self.work(lambda: bundle.patch_file(self.bundle_dir, self.rom, output), self.saved)

    def saved(self, result, error):
        if error:
            self.patch_button.configure(state="normal")
            self.show(str(error), "error")
            return
        self.show(f"Saved {result[1]}.\n\n{SAVE_ADVICE}", "done")

    def run(self):
        self.root.mainloop()


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
            result = f"Patched {entry['name']}: {output}\n{SAVE_ADVICE}"
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
