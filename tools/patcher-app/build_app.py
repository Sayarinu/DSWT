#!/usr/bin/env python3
"""Build the player app for this platform around a verified release archive.

The archive is the `patcher.py package` output. Its patches, manifest and documents
are checked, frozen into one app with the pinned PyInstaller, self-tested, and
zipped with the license notices as dswt-patcher-VERSION-PLATFORM.zip.
"""

import argparse
import importlib.metadata
import json
import os
from pathlib import Path, PurePosixPath
import platform
import re
import subprocess
import sys
import tempfile
import zipfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / "tools/bps-patch"))
import bps  # noqa: E402
import bundle  # noqa: E402

APP = "DSWT Patcher"
VERSION = re.compile(r"[0-9A-Za-z][0-9A-Za-z.+-]{0,63}")
DOCUMENTS = ("INSTALL.md", "LICENSE", "NOTICE")
MAX_ARCHIVE = 256 * 1024 * 1024


class BuildError(ValueError):
    pass


def platform_name():
    return {"win32": "windows", "darwin": "macos"}.get(sys.platform, "linux")


def read_archive(path, version=None):
    """Return {name: bytes} for a release archive after checking every member and patch."""
    if Path(path).stat().st_size > MAX_ARCHIVE:
        raise BuildError("Release archive is too large")
    with zipfile.ZipFile(path) as archive:
        infos = archive.infolist()
        prefixes = {PurePosixPath(info.filename).parts[0] for info in infos}
        if len(prefixes) != 1 or not re.fullmatch(r"dswt-[0-9A-Za-z.+-]{1,64}", next(iter(prefixes))):
            raise BuildError("Release archive must hold one dswt-VERSION directory")
        members = {}
        for info in infos:
            parts = PurePosixPath(info.filename).parts
            if info.is_dir() or len(parts) != 2 or parts[1] in members or info.file_size > bps.MAX_PATCH:
                raise BuildError(f"Unexpected archive member {info.filename!r}")
            members[parts[1]] = archive.read(info)
    try:
        manifest = bundle.load_manifest(members.get("manifest.json", b""))
    except bundle.BundleError as error:
        raise BuildError(str(error)) from None
    if next(iter(prefixes)) != f"dswt-{manifest['version']}" or (version and manifest["version"] != version):
        raise BuildError("Release archive version does not match")
    expected = {"manifest.json", *DOCUMENTS, *(entry["patch"]["file"] for entry in manifest["entries"])}
    if set(members) != expected:
        raise BuildError(f"Release archive members differ: {sorted(set(members) ^ expected)}")
    for entry in manifest["entries"]:
        patch = members[entry["patch"]["file"]]
        if len(patch) != entry["patch"]["size"] or bundle.sha256(patch) != entry["patch"]["sha256"]:
            raise BuildError(f"{entry['patch']['file']} does not match the manifest")
        bps.walk(patch)
    return manifest, members


def _license(distribution, pattern):
    for file in importlib.metadata.distribution(distribution).files or ():
        if re.search(pattern, file.name, re.I):
            return Path(file.locate()).read_bytes()
    raise BuildError(f"No license file found for {distribution}")


def third_party_notices():
    """License texts for what the frozen app contains besides DSWT's own code."""
    notices = {"licenses/PyInstaller-COPYING.txt": _license("pyinstaller", r"^COPYING")}
    candidates = [Path(sys.base_prefix) / "LICENSE.txt",
                  Path(sys.base_prefix) / "lib" / f"python{sys.version_info.major}.{sys.version_info.minor}" / "LICENSE.txt"]
    python_license = next((path for path in candidates if path.is_file()), None)
    if python_license is None:
        raise BuildError("No Python license found beside this interpreter")
    notices["licenses/Python-LICENSE.txt"] = python_license.read_bytes()
    try:
        import tkinter
        library = Path(tkinter.Tcl().eval("info library"))
        for name in ("license.terms", "../license.terms"):
            if (library / name).is_file():
                notices["licenses/Tcl-Tk-license.terms"] = (library / name).read_bytes()
                break
    except Exception:  # noqa: BLE001 - reported below
        pass
    if "licenses/Tcl-Tk-license.terms" not in notices:
        notices["licenses/Tcl-Tk-license.terms"] = (
            b"Tcl/Tk is distributed under the Tcl/Tk license: https://www.tcl-lang.org/software/tcltk/license.html\n")
    return notices


def freeze(manifest, members, work):
    """Run PyInstaller and return the path of the built app (a .app directory on macOS)."""
    data = work / "bundle"
    data.mkdir()
    for name, content in members.items():
        (data / name).write_bytes(content)
    command = [sys.executable, "-m", "PyInstaller", "--noconfirm", "--clean", "--log-level", "WARN",
               "--name", APP, "--windowed", "--paths", str(ROOT / "tools/bps-patch"),
               "--add-data", f"{data}{os.pathsep}bundle",
               "--distpath", str(work / "dist"), "--workpath", str(work / "build"), "--specpath", str(work),
               "--osx-bundle-identifier", "games.dswt.patcher"]
    if sys.platform != "darwin":
        command.append("--onefile")
    subprocess.run([*command, str(HERE / "dswt_patcher.py")], check=True)
    if sys.platform == "darwin":
        return work / "dist" / f"{APP}.app"
    return work / "dist" / (f"{APP}.exe" if sys.platform == "win32" else APP)


def executable(app):
    return app / "Contents" / "MacOS" / APP if app.suffix == ".app" else app


def _number(value):
    out = bytearray()
    while True:
        low, value = value & 0x7F, value >> 7
        if not value:
            return bytes(out + bytes([0x80 | low]))
        out.append(low)
        value -= 1


def synthetic_bundle(directory):
    """A one-entry bundle for a made-up trimmed cartridge, so the frozen app can patch for real."""
    source = bytearray(os.urandom(4096))
    source[0x0C:0x10], source[0x1E] = b"CPUE", 7
    source[0x80:0x84] = (4096 - 256).to_bytes(4, "little")
    source[-256:] = b"\xff" * 256
    source = bytes(source)
    target = source[:0x200] + os.urandom(4096 - 0x200)
    body = (b"BPS1" + _number(len(source)) + _number(len(target)) + _number(0)
            + _number(0x200 - 1 << 2) + _number(len(target) - 0x200 - 1 << 2 | 1) + target[0x200:]
            + bps.crc32(source).to_bytes(4, "little") + bps.crc32(target).to_bytes(4, "little"))
    patch = body + bps.crc32(body).to_bytes(4, "little")
    digests = lambda data: {"size": len(data), "sha256": bundle.sha256(data), "crc32": bundle.crc32_hex(data)}
    manifest = {"schema_version": bundle.SCHEMA_VERSION, "kind": bundle.KIND, "version": "smoke", "entries": [{
        "variant": "smoke", "name": "Smoke test", "game_code": "CPUE", "revision": 7, "language": "English",
        "input": digests(source), "patch": dict(digests(patch), file="smoke.bps"), "output": digests(target)}]}
    directory.mkdir()
    (directory / "manifest.json").write_text(json.dumps(manifest))
    (directory / "smoke.bps").write_bytes(patch)
    return source[:-256], target


def smoke_test(app, work):
    """The frozen app must pass its self-test, patch a trimmed dump and refuse a non-ROM without writing."""
    def run(*arguments):
        return subprocess.run([str(executable(app)), *arguments], capture_output=True, timeout=300)
    result = run("--self-test")
    if result.returncode:
        raise BuildError(f"Frozen app failed its self-test: {result.stderr.decode(errors='replace')[-400:]}")
    probe = work / "probe"
    probe.mkdir()
    trimmed, target = synthetic_bundle(work / "smoke-bundle")
    (probe / "rom.nds").write_bytes(trimmed)
    result = run("--bundle", str(work / "smoke-bundle"), "--input", str(probe / "rom.nds"))
    output = probe / "rom (Wonder Trade).nds"
    if result.returncode or not output.is_file() or output.read_bytes() != target:
        raise BuildError(f"Frozen app did not patch a trimmed dump: {result.stderr.decode(errors='replace')[-400:]}")
    if run("--bundle", str(work / "smoke-bundle"), "--input", str(probe / "rom.nds")).returncode != 1:
        raise BuildError("Frozen app replaced an existing output")
    (probe / "not-a-rom.nds").write_bytes(b"not a ROM" * 100)
    result = run("--input", str(probe / "not-a-rom.nds"))
    if result.returncode != 1 or sorted(path.name for path in probe.iterdir()) != sorted(
            ["rom.nds", output.name, "not-a-rom.nds"]):
        raise BuildError("Frozen app did not refuse a non-ROM cleanly")


def package(app, manifest, members, output):
    """Zip the app with the release documents and third-party notices."""
    name = f"dswt-patcher-{manifest['version']}-{platform_name()}.zip"
    target = Path(output) / name
    if target.exists():
        raise BuildError(f"{target} already exists")
    Path(output).mkdir(parents=True, exist_ok=True)
    files = {doc: members[doc] for doc in DOCUMENTS}
    files.update(third_party_notices())
    with zipfile.ZipFile(target, "x", zipfile.ZIP_DEFLATED) as archive:
        if app.is_dir():
            for path in sorted(app.rglob("*")):
                relative = path.relative_to(app.parent).as_posix()
                if path.is_symlink():
                    info = zipfile.ZipInfo(relative)
                    info.external_attr = (0o120777 << 16)
                    archive.writestr(info, os.readlink(path))
                elif path.is_file():
                    info = zipfile.ZipInfo.from_file(path, relative)
                    info.external_attr = ((0o100755 if os.access(path, os.X_OK) else 0o100644) << 16)
                    info.compress_type = zipfile.ZIP_DEFLATED
                    archive.writestr(info, path.read_bytes())
        else:
            info = zipfile.ZipInfo.from_file(app, app.name)
            info.external_attr = 0o100755 << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, app.read_bytes())
        for relative, content in sorted(files.items()):
            info = zipfile.ZipInfo(relative, date_time=(1980, 1, 1, 0, 0, 0))
            info.external_attr = 0o100644 << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, content)
    return target


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", required=True, type=Path, help="dswt-VERSION.zip from patcher.py package")
    parser.add_argument("--version", help="require this release version")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--check-only", action="store_true", help="verify the archive without building")
    args = parser.parse_args(argv)
    if args.version and not VERSION.fullmatch(args.version):
        parser.error("invalid version")
    try:
        manifest, members = read_archive(args.archive, args.version)
        if args.check_only:
            print(json.dumps({"version": manifest["version"], "variants": [e["variant"] for e in manifest["entries"]]}))
            return 0
        with tempfile.TemporaryDirectory(prefix="dswt-app-") as temporary:
            work = Path(temporary)
            app = freeze(manifest, members, work)
            smoke_test(app, work)
            target = package(app, manifest, members, args.output)
    except (OSError, ValueError, zipfile.BadZipFile, subprocess.SubprocessError) as error:
        print(f"build failed: {error}", file=sys.stderr)
        return 1
    print(json.dumps({"app": str(target), "sha256": bundle.sha256(target.read_bytes()),
                      "platform": platform_name(), "machine": platform.machine()}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
