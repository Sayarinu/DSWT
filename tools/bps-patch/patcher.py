#!/usr/bin/env python3
"""Generate, audit and safely apply private BPS candidates with pinned Flips v198."""

import argparse
import ctypes
import errno
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import signal
import stat
import subprocess
import sys
import tarfile
import tempfile
import urllib.request

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bps

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
PATCH_NAME = re.compile(r"[a-z0-9][a-z0-9.-]{0,63}\.bps")
VERSION = re.compile(r"[0-9A-Za-z][0-9A-Za-z.+-]{0,63}")
SHA256 = re.compile(r"[0-9a-f]{64}")
CRC32 = re.compile(r"[0-9a-f]{8}")
SUPPORTED = ("us-rev0", "us-rev1")
BLOCKERS = [
    "Private candidate: publication_permitted is false",
    "Europe (English) has no compatible target (LUNE-301)",
    "Redistribution rights for generated patch literals are not cleared",
]
# Nintendo DS cartridge header fields shared by a base and its modified build.
HEADER_SIZE = 0x200
GAME_CODE = slice(0x0C, 0x10)
ROM_VERSION = 0x1E
FLIPS_TIMEOUT = 30 * 60
MAX_MANIFEST = 1024 * 1024
MAX_PROFILE = 64 * 1024
OPEN_DIRECTORY = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC
UNSUPPORTED_RENAME = {errno.EINVAL, errno.ENOSYS, errno.ENOTSUP, errno.EOPNOTSUPP, errno.EXDEV, errno.EPERM}


class PatchError(ValueError):
    pass


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


configure_connection = _load("configure_connection", ROOT / "scripts/configure-connection.py")
identify_rom = _load("identify_rom", ROOT / "scripts/identify-rom.py")


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def crc32_hex(data):
    return f"{bps.crc32(data):08x}"


def to_json(value):
    return (json.dumps(value, indent=2, sort_keys=True) + "\n").encode()


def walk(patch, label):
    try:
        return bps.walk(patch)
    except bps.BPSError as error:
        raise PatchError(f"{label} is invalid: {error}") from None


def literal_bytes(patch, report):
    return b"".join(patch[item["patch_offset"]:item["patch_offset"] + item["length"]]
                    for item in report["literals"]["items"])


def read_private(path, limit, label, dir_fd=None):
    """Read a regular, singly linked, non-symlink file through one descriptor."""
    try:
        fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC, dir_fd=dir_fd)
    except OSError as error:
        if error.errno == errno.ELOOP:
            raise PatchError(f"{label} must not be a symlink") from None
        raise
    with os.fdopen(fd, "rb") as handle:
        info = os.fstat(handle.fileno())
        if not stat.S_ISREG(info.st_mode):
            raise PatchError(f"{label} must be a regular file")
        if info.st_nlink != 1:
            raise PatchError(f"{label} has another hard link; use an unlinked copy")
        if info.st_size > limit:
            raise PatchError(f"{label} exceeds {limit} bytes")
        data = handle.read(limit + 1)
    if len(data) != info.st_size:
        raise PatchError(f"{label} changed while it was read")
    return data, info


def _write_new(directory_fd, name, data, mode=0o600):
    fd = os.open(name, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC, mode, dir_fd=directory_fd)
    with os.fdopen(fd, "wb") as handle:
        handle.write(data)
        handle.flush()
        os.fchmod(handle.fileno(), mode)
        os.fsync(handle.fileno())


def _fsync_directory(name, dir_fd):
    fd = os.open(name, OPEN_DIRECTORY, dir_fd=dir_fd)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def _renameat_noreplace(source_fd, source, target_fd, target):
    """Atomic rename that fails if the target exists; returns 0 or an errno."""
    libc = ctypes.CDLL(None, use_errno=True)
    for function, flag in (("renameat2", 1), ("renameatx_np", 4)):  # RENAME_NOREPLACE / RENAME_EXCL
        call = getattr(libc, function, None)
        if call is not None:
            if call(source_fd, os.fsencode(source), target_fd, os.fsencode(target), flag) == 0:
                return 0
            return ctypes.get_errno()
    return errno.ENOSYS


class Stage:
    """A private directory beside a destination; only this object's directory is ever removed."""

    def __init__(self, parent, prefix):
        self.parent = Path(parent).resolve(strict=True)
        if "," in str(self.parent):
            raise PatchError("Paths containing commas cannot be mounted for the pinned tool")
        self.parent_fd = os.open(self.parent, OPEN_DIRECTORY)
        self.name = f".{prefix}-{secrets.token_hex(8)}"
        os.mkdir(self.name, 0o700, dir_fd=self.parent_fd)
        self.fd = os.open(self.name, OPEN_DIRECTORY, dir_fd=self.parent_fd)
        self.identity = os.fstat(self.fd)
        self.path = self.parent / self.name

    def check(self):
        current = os.stat(self.path, follow_symlinks=False)
        if (current.st_dev, current.st_ino) != (self.identity.st_dev, self.identity.st_ino):
            raise PatchError("Private staging directory was replaced")

    def write(self, name, data, mode=0o600):
        _write_new(self.fd, name, data, mode)

    def read(self, name, limit):
        return read_private(name, limit, f"Staged {name}", self.fd)[0]

    def remove(self, *names):
        for name in names:
            os.unlink(name, dir_fd=self.fd)

    def write_tree(self, name, files):
        """Write {relative: (data, mode)} into a new staged directory and flush it."""
        os.mkdir(name, 0o755, dir_fd=self.fd)
        directories = sorted({str(Path(relative).parent) for relative in files} - {"."})
        top = os.open(name, OPEN_DIRECTORY, dir_fd=self.fd)
        try:
            for directory in directories:
                os.mkdir(directory, 0o700, dir_fd=top)
            for relative, (data, mode) in files.items():
                _write_new(top, relative, data, mode)
            for directory in directories:
                _fsync_directory(directory, top)
            os.fsync(top)
        finally:
            os.close(top)

    def publish(self, name, destination):
        """Atomically move a verified staged entry to a new name; never replaces an existing path."""
        self.check()
        result = _renameat_noreplace(self.fd, name, self.parent_fd, destination)
        if result == errno.EEXIST or result == errno.ENOTEMPTY:
            raise PatchError(f"Output {destination} appeared during patching; it was preserved")
        if result in UNSUPPORTED_RENAME:
            raise PatchError("The output filesystem cannot publish without replacement; choose a local directory")
        if result:
            raise OSError(result, os.strerror(result), destination)
        os.fsync(self.parent_fd)

    def close(self, quiet=False):
        try:
            self.check()
            shutil.rmtree(self.name, dir_fd=self.parent_fd)
        except FileNotFoundError:
            pass
        except PatchError:
            # A replaced directory is not ours to remove.
            if not quiet:
                raise
        finally:
            os.close(self.fd)
            os.close(self.parent_fd)

    def __enter__(self):
        return self

    def __exit__(self, kind, *_):
        self.close(quiet=kind is not None)


class Flips:
    """Runs the pinned image without network access, against one private staging directory."""

    def __init__(self, record):
        self.record = record

    def run(self, stage, *arguments):
        stage.check()
        name = "platinumwt-flips-" + secrets.token_hex(8)
        command = ["docker", "run", "--rm", "--name", name, "--network", "none", "--read-only",
                   "--cap-drop", "ALL", "--security-opt", "no-new-privileges", "--pids-limit", "16",
                   "--memory", "8g", "--user", f"{os.getuid()}:{os.getgid()}",
                   "--mount", f"type=bind,source={stage.path},target=/work", "--workdir", "/work",
                   self.record["image_id"], *arguments]
        try:
            result = subprocess.run(command, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=FLIPS_TIMEOUT)
        except subprocess.TimeoutExpired:
            raise PatchError("Pinned Flips timed out") from None
        finally:
            subprocess.run(["docker", "rm", "--force", name], stdin=subprocess.DEVNULL, capture_output=True)
        stage.check()
        if result.returncode:
            detail = (result.stdout + result.stderr).strip().splitlines()
            raise PatchError("Pinned Flips failed" + (f": {detail[-1]}" if detail else ""))

    def create(self, stage, source, target, patch):
        self.run(stage, "--create", "--bps-delta", "--exact", f"/work/{source}", f"/work/{target}", f"/work/{patch}")

    def apply(self, stage, patch, source, target):
        self.run(stage, "--apply", "--exact", f"/work/{patch}", f"/work/{source}", f"/work/{target}")


def _docker(*arguments):
    result = subprocess.run(["docker", *arguments], stdin=subprocess.DEVNULL, capture_output=True, text=True)
    if result.returncode:
        raise PatchError(f"docker {arguments[0]} failed: {result.stderr.strip()[-400:]}")
    return result.stdout.strip()


def _private_dir(path):
    if path.is_symlink():
        raise PatchError(f"{path} must not be a symlink")
    path.mkdir(exist_ok=True)
    return path


def _image_binary(image_id, directory):
    container = _docker("create", image_id)
    try:
        _docker("cp", f"{container}:/flips", str(directory / "flips"))
        _docker("cp", f"{container}:/provenance/toolchain.txt", str(directory / "toolchain.txt"))
    finally:
        _docker("rm", container)
    return (directory / "flips").read_bytes()


def _tool_location(root):
    pin_bytes = (HERE / "flips.json").read_bytes()
    dockerfile = (HERE / "Dockerfile").read_bytes()
    pin = json.loads(pin_bytes)
    key = sha256(pin_bytes + b"\0" + dockerfile)[:16]
    tools = _private_dir(_private_dir(root / "game") / "tools")
    return pin, dockerfile, key, tools, tools / f"flips-{pin['version']}-{key}"


def installed_flips(root=ROOT):
    """Return the prepared tool's provenance after re-verifying its source, notices, binary and image."""
    pin, _, _, tools, destination = _tool_location(root)
    if destination.is_symlink() or not destination.is_dir():
        raise PatchError("Pinned Flips is not prepared; run: python3 tools/bps-patch/patcher.py prepare")
    record = json.loads(read_private(destination / "provenance.json", MAX_MANIFEST, "Flips provenance")[0])
    expected = pin["binaries"].get(record["build"]["platform"])
    archive = read_private(destination / record["archive"]["file"], pin["size"], "Flips source archive")[0]
    if sha256(archive) != pin["sha256"] or record["archive"]["sha256"] != pin["sha256"]:
        raise PatchError("Retained Flips source archive does not match its pin")
    for name, digest in record["notices"].items():
        if sha256(read_private(destination / "notices" / name, MAX_MANIFEST, "Flips notice")[0]) != digest:
            raise PatchError(f"Retained Flips notice {name} changed")
    binary = read_private(destination / "flips", 16 * 1024 * 1024, "Flips binary")[0]
    if expected is None or record["binary"] != expected or sha256(binary) != expected["sha256"]:
        raise PatchError("Retained Flips binary does not match its pin")
    with tempfile.TemporaryDirectory(prefix=".flips-check-", dir=tools) as temporary:
        if sha256(_image_binary(record["image_id"], Path(temporary))) != expected["sha256"]:
            raise PatchError("Pinned Flips image does not contain the pinned binary")
    return record


def prepare_flips(root=ROOT, archive=None):
    """Build the pinned CLI once into ignored game/tools and return its verified provenance record."""
    pin, dockerfile, key, tools, destination = _tool_location(root)
    if destination.exists() or destination.is_symlink():
        return installed_flips(root)
    platform = _docker("version", "--format", "{{.Server.Os}}/{{.Server.Arch}}")
    if platform not in pin["binaries"]:
        raise PatchError(f"No pinned Flips binary for Docker platform {platform}")
    with tempfile.TemporaryDirectory(prefix=".flips-", dir=tools) as temporary:
        stage = Path(temporary)
        downloaded = stage / f"flips-{pin['revision']}.tar.gz"
        if archive is None:
            with urllib.request.urlopen(pin["url"], timeout=60) as response, downloaded.open("xb") as output:
                count = 0
                while block := response.read(1024 * 1024):
                    count += len(block)
                    if count > pin["size"]:
                        raise PatchError("Flips archive exceeds its pinned size")
                    output.write(block)
        else:
            if Path(archive).stat().st_size != pin["size"]:
                raise PatchError("Flips archive size mismatch")
            shutil.copyfile(archive, downloaded)
        if sha256(downloaded.read_bytes()) != pin["sha256"]:
            raise PatchError("Flips archive checksum mismatch")
        context = stage / "context"
        context.mkdir()
        with tarfile.open(downloaded) as source:
            if source.pax_headers.get("comment") != pin["revision"]:
                raise PatchError("Flips archive revision mismatch")
            source.extractall(context, filter="data")
        (context / f"Flips-{pin['revision']}").rename(context / "source")
        (context / "Dockerfile").write_bytes(dockerfile)
        tag = f"platinumwt-flips:{pin['version']}-{key}"
        _docker("build", "--pull=false", "--tag", tag, "--iidfile", str(stage / "image-id"), str(context))
        image_id = (stage / "image-id").read_text().strip()
        binary = _image_binary(image_id, stage)
        if sha256(binary) != pin["binaries"][platform]["sha256"]:
            raise PatchError(f"Built Flips binary {sha256(binary)} does not match the {platform} pin")
        for name in pin["notices"]:
            target = stage / "notices" / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(context / "source" / name, target)
        shutil.rmtree(context)
        record = {
            "tool": "flips", "version": pin["version"], "revision": pin["revision"],
            "archive": {"url": pin["url"], "size": pin["size"], "sha256": pin["sha256"], "file": downloaded.name},
            "license": pin["license"],
            "notices": {name: sha256((stage / "notices" / name).read_bytes()) for name in pin["notices"]},
            "build": {"dockerfile_sha256": sha256(dockerfile), "command": "make TARGET=cli CFLAGS=\"-O2 -ffile-prefix-map=/src=.\" LFLAGS=-static",
                      "toolchain": (stage / "toolchain.txt").read_text().split(), "platform": platform},
            "binary": {"size": len(binary), "sha256": sha256(binary)},
            "image_id": image_id, "image_tag": tag,
        }
        (stage / "provenance.json").write_bytes(to_json(record))
        os.chmod(stage, 0o755)
        stage.rename(destination)
    return installed_flips(root)


def tool_summary(record):
    return {key: record[key] for key in ("tool", "version", "revision", "archive", "license", "notices", "build", "binary")}


def _match(catalog, data):
    try:
        variant = identify_rom.match(len(data), sha256(data), catalog)
    except ValueError as error:
        raise PatchError(f"Base ROM: {error}") from None
    if variant["id"] not in SUPPORTED:
        raise PatchError(f"{variant['name']} is outside the supported US Rev 0/1 subset")
    return variant


def _profile_binding(profile_bytes):
    try:
        profile = json.loads(profile_bytes)
        configure_connection.header(profile)
    except (ValueError, TypeError) as error:
        raise PatchError(f"Invalid connection profile: {error}") from None
    pins = [bytes.fromhex(profile[name]) for name in ("current_spki_sha256", "next_spki_sha256")]
    return f"{profile['hostname']}:{profile['port']}".encode(), pins


def _new_output(path, root=None):
    """Resolve a not-yet-existing output to (parent, name); optionally confine it below root/game."""
    path = Path(path)
    if path.name in ("", ".", "..") or os.path.lexists(path):
        raise PatchError(f"Output {path} already exists or is not a new file name; it was preserved")
    parent = path.parent.resolve(strict=True)
    if not parent.is_dir():
        raise PatchError("Output directory does not exist")
    if root is not None and not parent.is_relative_to((root / "game").resolve(strict=True)):
        raise PatchError("Generated bundles and evidence must stay under the ignored game/ directory")
    return parent, path.name


def _verify_report(report, source, target_size, target_crc):
    if report["metadata"]["size"]:
        raise PatchError("BPS metadata is not supported")
    if report["source"]["size"] != len(source) or report["source"]["crc32"] != crc32_hex(source):
        raise PatchError("Patch was not made for this input")
    if report["target"]["size"] != target_size or report["target"]["crc32"] != target_crc:
        raise PatchError("Patch target does not match the expected output")


def _check_target(variant, base, target, authority, pins):
    if (target == base or len(target) < HEADER_SIZE or target[GAME_CODE] != base[GAME_CODE]
            or target[ROM_VERSION] != base[ROM_VERSION]):
        raise PatchError(f"Target is not a modified build of {variant['name']}")
    if authority not in target or any(target.count(pin) != 1 or pin in base for pin in pins):
        raise PatchError(f"Target for {variant['id']} was not built with this connection profile")


def _generate(stage, flips, variant, base, target):
    """Create the patch twice, require identical bytes, walk it and round-trip it to the target."""
    prefix = variant["id"]
    stage.write(f"{prefix}.source", base)
    stage.write(f"{prefix}.target", target)
    patches = []
    for attempt in ("a", "b"):
        flips.create(stage, f"{prefix}.source", f"{prefix}.target", f"{prefix}.{attempt}.bps")
        patches.append(stage.read(f"{prefix}.{attempt}.bps", bps.MAX_PATCH))
    if patches[0] != patches[1]:
        raise PatchError(f"Pinned Flips produced different patches for {prefix}")
    report = walk(patches[0], f"Generated patch for {prefix}")
    _verify_report(report, base, len(target), crc32_hex(target))
    flips.apply(stage, f"{prefix}.a.bps", f"{prefix}.source", f"{prefix}.roundtrip")
    if stage.read(f"{prefix}.roundtrip", bps.MAX_FILE) != target:
        raise PatchError(f"Round trip for {prefix} did not reproduce the target")
    stage.remove(*(f"{prefix}.{suffix}" for suffix in ("source", "target", "a.bps", "b.bps", "roundtrip")))
    return patches[0], report


def create(pairs, profile_path, output, version, flips, source, catalog=None, root=ROOT):
    """Generate one private bundle and publish its directory atomically without replacement."""
    if not VERSION.fullmatch(version):
        raise PatchError("Version must be 1-64 characters of [0-9A-Za-z.+-]")
    if source.get("dirty") is not False or not re.fullmatch(r"[0-9a-f]{40}", source.get("commit", "")):
        raise PatchError("Generate only from a clean, committed source tree")
    catalog = catalog or json.loads((root / "patches/compatibility.json").read_text())
    profile_bytes = read_private(profile_path, MAX_PROFILE, "Connection profile")[0]
    authority, pins = _profile_binding(profile_bytes)
    parent, name = _new_output(output, root)
    entries, files = [], {}
    with Stage(parent, f"{name}.partial") as stage:
        for base_path, target_path in pairs:
            base = read_private(base_path, bps.MAX_FILE, "Base ROM")[0]
            variant = _match(catalog, base)
            if any(entry["variant"] == variant["id"] for entry in entries):
                raise PatchError(f"{variant['id']} was supplied more than once")
            target = read_private(target_path, bps.MAX_FILE, "Target ROM")[0]
            _check_target(variant, base, target, authority, pins)
            patch, report = _generate(stage, flips, variant, base, target)
            literals = literal_bytes(patch, report)
            prefix, filename = variant["id"], variant["planned_patch"]
            files[filename] = (patch, 0o644)
            files[f"audit/{prefix}.json"] = (to_json(report), 0o600)
            files[f"audit/{prefix}.literals.bin"] = (literals, 0o600)
            entries.append({
                "variant": prefix, "name": variant["name"],
                "input": {"size": len(base), "sha256": variant["sha256"], "crc32": report["source"]["crc32"]},
                "patch": {"file": filename, "size": len(patch), "sha256": report["patch"]["sha256"], "crc32": report["patch"]["crc32"]},
                "output": {"size": len(target), "sha256": sha256(target), "crc32": report["target"]["crc32"]},
                "actions": report["actions"],
                "literals": {"count": report["literals"]["count"], "bytes": report["literals"]["bytes"],
                             "sha256": sha256(literals), "inventory": f"audit/{prefix}.json"},
            })
        entries.sort(key=lambda entry: entry["variant"])
        manifest = {
            "schema_version": 1, "kind": "platinumwt-private-bps-bundle", "version": version,
            "source": {"commit": source["commit"], "dirty": False},
            "upstream": {"repository": source["upstream_repository"], "revision": source["upstream_revision"]},
            "connection_profile_sha256": sha256(profile_bytes),
            "tool": tool_summary(flips.record),
            "supported_subset": [entry["variant"] for entry in entries],
            "publication_permitted": False, "distribution_blocked": True, "distribution_blockers": BLOCKERS,
            "entries": entries,
        }
        files["manifest.json"] = (to_json(manifest), 0o644)
        stage.write_tree("bundle", files)
        stage.publish("bundle", name)
    return manifest


def _require(condition, message):
    if not condition:
        raise PatchError(f"Malformed manifest: {message}")


def load_manifest(data):
    try:
        manifest = json.loads(data)
    except ValueError:
        raise PatchError("Malformed manifest: not JSON") from None
    _require(isinstance(manifest, dict) and manifest.get("schema_version") == 1
             and manifest.get("kind") == "platinumwt-private-bps-bundle", "unsupported schema")
    _require(manifest.get("publication_permitted") is False and manifest.get("distribution_blocked") is True,
             "a private bundle must be blocked from publication and distribution")
    entries = manifest.get("entries")
    _require(isinstance(entries, list) and entries, "no entries")
    seen = {"variant": set(), "file": set(), "input": set()}
    for entry in entries:
        _require(isinstance(entry, dict) and isinstance(entry.get("variant"), str), "entry without variant")
        for part, limit in (("input", bps.MAX_FILE), ("patch", bps.MAX_PATCH), ("output", bps.MAX_FILE)):
            value = entry.get(part)
            _require(isinstance(value, dict) and type(value.get("size")) is int and 0 < value["size"] <= limit
                     and isinstance(value.get("sha256"), str) and SHA256.fullmatch(value["sha256"])
                     and isinstance(value.get("crc32"), str) and CRC32.fullmatch(value["crc32"]), f"invalid {part}")
        filename = entry["patch"].get("file")
        _require(isinstance(filename, str) and PATCH_NAME.fullmatch(filename), "unsafe patch filename")
        for key, value in (("variant", entry["variant"]), ("file", filename),
                           ("input", (entry["input"]["size"], entry["input"]["sha256"]))):
            _require(value not in seen[key], f"duplicate {key}")
            seen[key].add(value)
    return manifest


def apply(manifest_path, input_path, output_path, flips):
    """Patch one verified input to a new output, published atomically without replacement."""
    manifest_path = Path(manifest_path)
    manifest_bytes, manifest_info = read_private(manifest_path, MAX_MANIFEST, "Manifest")
    manifest = load_manifest(manifest_bytes)
    source, source_info = read_private(input_path, bps.MAX_FILE, "Input ROM")
    digest = sha256(source)
    matches = [e for e in manifest["entries"] if e["input"]["size"] == len(source) and e["input"]["sha256"] == digest]
    if len(matches) != 1:
        raise PatchError(f"Input is not a supported unmodified base: size={len(source)}, SHA-256={digest}")
    entry = matches[0]
    patch, patch_info = read_private(manifest_path.parent / entry["patch"]["file"], bps.MAX_PATCH, "Patch")
    identities = {(info.st_dev, info.st_ino) for info in (manifest_info, source_info, patch_info)}
    if len(identities) != 3:
        raise PatchError("Manifest, patch and input must be distinct files")
    if len(patch) != entry["patch"]["size"] or sha256(patch) != entry["patch"]["sha256"]:
        raise PatchError("Patch does not match the manifest")
    if crc32_hex(source) != entry["input"]["crc32"]:
        raise PatchError("Input CRC32 does not match the manifest")
    report = walk(patch, "Patch")
    _verify_report(report, source, entry["output"]["size"], entry["output"]["crc32"])
    parent, name = _new_output(output_path)
    with Stage(parent, f"{name}.partial") as stage:
        stage.write("source.bin", source)
        stage.write("patch.bps", patch)
        flips.apply(stage, "patch.bps", "source.bin", "target.bin")
        stage.check()
        target = stage.read("target.bin", bps.MAX_FILE)
        if (len(target) != entry["output"]["size"] or crc32_hex(target) != entry["output"]["crc32"]
                or sha256(target) != entry["output"]["sha256"]):
            raise PatchError("Patched output failed verification; nothing was published")
        stage.write("output.nds", target, 0o644)
        stage.publish("output.nds", name)
    return entry


def audit(patch_path, evidence=None, root=ROOT):
    patch = read_private(patch_path, bps.MAX_PATCH, "Patch")[0]
    report = walk(patch, "Patch")
    if evidence is not None:
        parent, name = _new_output(evidence, root)
        offset = report["metadata"]["offset"]
        metadata = patch[offset:offset + report["metadata"]["size"]]
        with Stage(parent, f"{name}.partial") as stage:
            stage.write_tree("evidence", {"report.json": (to_json(report), 0o600),
                                          "literals.bin": (literal_bytes(patch, report), 0o600),
                                          "metadata.bin": (metadata, 0o600)})
            stage.publish("evidence", name)
    summary = {key: value for key, value in report.items() if key != "literals"}
    summary["literals"] = {key: report["literals"][key] for key in ("count", "bytes")}
    return summary


def source_state(root=ROOT):
    def git(*arguments):
        return subprocess.run(["git", "-C", str(root), *arguments], capture_output=True, text=True, check=True).stdout.strip()
    upstream = json.loads((root / "client/upstream.json").read_text())
    return {"commit": git("rev-parse", "HEAD"), "dirty": bool(git("status", "--porcelain")),
            "upstream_repository": upstream["repository"], "upstream_revision": upstream["revision"]}


def _terminate(*_):
    raise KeyboardInterrupt


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    prepare_parser = commands.add_parser("prepare", help="build and verify the pinned Flips CLI in game/tools")
    prepare_parser.add_argument("--archive", type=Path, help="local copy of the exact pinned source archive")
    create_parser = commands.add_parser("create", help="generate a private bundle under game/")
    create_parser.add_argument("--pair", nargs=2, action="append", required=True, metavar=("BASE", "TARGET"), type=Path)
    create_parser.add_argument("--profile", required=True, type=Path)
    create_parser.add_argument("--output", required=True, type=Path)
    create_parser.add_argument("--version", required=True)
    apply_parser = commands.add_parser("apply", help="patch a verified base to a new file")
    apply_parser.add_argument("--manifest", required=True, type=Path)
    apply_parser.add_argument("--input", required=True, type=Path)
    apply_parser.add_argument("--output", required=True, type=Path)
    audit_parser = commands.add_parser("audit", help="walk one patch and optionally extract private literal evidence")
    audit_parser.add_argument("patch", type=Path)
    audit_parser.add_argument("--evidence", type=Path)
    args = parser.parse_args(argv)
    signal.signal(signal.SIGTERM, _terminate)
    try:
        if args.command == "prepare":
            result = tool_summary(prepare_flips(archive=args.archive))
        elif args.command == "audit":
            result = audit(args.patch, args.evidence)
        elif args.command == "create":
            manifest = create(args.pair, args.profile, args.output, args.version, Flips(installed_flips()), source_state())
            result = {"output": str(args.output), "entries": manifest["entries"]}
        else:
            entry = apply(args.manifest, args.input, args.output, Flips(installed_flips()))
            result = {"variant": entry["variant"], "output": str(args.output), "sha256": entry["output"]["sha256"]}
    except KeyboardInterrupt:
        print("Interrupted; inputs and any existing output were preserved", file=sys.stderr)
        return 130
    except (OSError, ValueError, subprocess.SubprocessError, tarfile.TarError) as error:
        print(f"{args.command} failed: {error}", file=sys.stderr)
        return 1
    print(to_json(result).decode(), end="")
    return 0


if __name__ == "__main__":
    sys.exit(main())
