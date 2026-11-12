# DSWT Patcher and releases

`dswt_patcher.py` is the player app: a one-window patcher drawn like a DS-era
menu, with a command-line mode (`--input ROM [--output FILE]`, `--self-test`).
[`menu_style.py`](menu_style.py) holds its original pixel font and frames, drawn on a
256x192 screen scaled by a whole number; nothing comes from any game. Arrow keys
move the cursor, Enter/Z/Space is A, Escape/X/Backspace is B, and the mouse works. It embeds one
release bundle and uses the portable [`bundle.py`](../bps-patch/README.md) checks,
so it needs no Docker or other tools. Run it from source with
`python3 tools/patcher-app/dswt_patcher.py --bundle DIR`.

## Release

1. Merge to `main`, then build the bases and targets and generate the bundle
   from that commit with the production profile, as in
   [`tools/bps-patch`](../bps-patch/README.md). Its release checks must pass.
2. Package it and upload it to a new draft release tagged at the bundle's
   source commit (needs `gh` signed in with write access):

   ```sh
   python3 tools/bps-patch/patcher.py package game/bundles/0.1.0 --output game/bundles/dswt-0.1.0.zip
   python3 tools/patcher-app/release.py draft --version 0.1.0 --archive game/bundles/dswt-0.1.0.zip
   ```

3. Run the **Release patcher** workflow on `main` with that version. It verifies
   the draft's archive, then builds the app on Linux, macOS and Windows with the
   hash-pinned PyInstaller in [`requirements.txt`](requirements.txt). Each frozen app
   must pass its self-test, patch a synthetic trimmed dump end to end, refuse to
   replace a file and refuse a non-ROM. The workflow then uploads the apps and
   `SHA256SUMS` and publishes the release.

Only the fetch and publish jobs can write, and only to releases;
`tools/local-stack/policy.py` holds the workflow to that. ROMs never leave the
maintainer's machine: CI sees only the packaged patches. The apps are unsigned,
and the macOS build targets the runner's Apple silicon. Each app zip carries
`LICENSE`, `NOTICE`, `INSTALL.md` and the Python, PyInstaller and Tcl/Tk license
texts.

```sh
python3 -m unittest discover -s tools/patcher-app/__tests__
```

Set `DSWT_TEST_WINDOW=1` on a machine with Tk and a display to also drive the window.
