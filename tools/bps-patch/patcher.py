#!/usr/bin/env python3
"""Generate checked release bundles with pinned Flips v198, audit patches and apply them."""

import argparse
import base64
import ctypes
import errno
import hashlib
import importlib.util
import io
import ipaddress
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
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bps
import bundle

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
VERSION = re.compile(r"[0-9A-Za-z][0-9A-Za-z.+-]{0,63}")
HEADER_SIZE, GAME_CODE, ROM_VERSION = bundle.HEADER_SIZE, bundle.GAME_CODE, bundle.ROM_VERSION
# A literal may not hold any window of this many bytes found verbatim in a retail
# base; with this stride, any copied run of 47 bytes or more is caught.
VERBATIM_WINDOW, VERBATIM_STRIDE = 32, 16
RESERVED_SUFFIXES = ("localhost", "local", "test", "example", "invalid", "internal", "lan", "home.arpa", "onion")
# Release files copied into the player archive beside the manifest and patches.
RELEASE_DOCUMENTS = {"INSTALL.md": "patches/INSTALL.md", "LICENSE": "LICENSE", "NOTICE": "NOTICE"}
FLIPS_TIMEOUT = 30 * 60
MAX_MANIFEST = 1024 * 1024
MAX_PROFILE = 64 * 1024
OPEN_DIRECTORY = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC
UNSUPPORTED_RENAME = {errno.EINVAL, errno.ENOSYS, errno.ENOTSUP, errno.EOPNOTSUPP, errno.EXDEV}


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


def _libc_rename():
    # The host may be Linux (renameat2) or macOS (renameatx_np) whatever the Docker platform is.
    libc = ctypes.CDLL(None, use_errno=True)
    for function, flag in (("renameat2", 1), ("renameatx_np", 4)):  # RENAME_NOREPLACE / RENAME_EXCL
        call = getattr(libc, function, None)
        if call is not None:
            call.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint]
            call.restype = ctypes.c_int
            return call, flag
    return None, 0


_RENAME = _libc_rename()


def _renameat_noreplace(source_fd, source, target_fd, target):
    """Atomic rename that fails if the target exists; returns 0 or an errno."""
    call, flag = _RENAME
    if call is None:
        return errno.ENOSYS
    if call(source_fd, os.fsencode(source), target_fd, os.fsencode(target), flag) == 0:
        return 0
    return ctypes.get_errno()


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
        directories = sorted({str(parent) for relative in files for parent in Path(relative).parents} - {"."})
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
        name = "dswt-flips-" + secrets.token_hex(8)
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
    tools = root / "game" / "tools"
    return pin, dockerfile, key, tools, tools / f"flips-{pin['version']}-{key}"


def installed_flips(root=ROOT):
    """Return the prepared tool's provenance after re-verifying its source, notices, binary and image."""
    pin, _, _, tools, destination = _tool_location(root)
    if any(path.is_symlink() for path in (root / "game", tools, destination)) or not destination.is_dir():
        raise PatchError("Pinned Flips is not prepared; run: python3 tools/bps-patch/patcher.py prepare")
    try:
        record = json.loads(read_private(destination / "provenance.json", MAX_MANIFEST, "Flips provenance")[0])
        expected = pin["binaries"].get(record["build"]["platform"])
        archive_name, notices, image_id = record["archive"]["file"], record["notices"], record["image_id"]
        binary_record = record["binary"]
    except (ValueError, KeyError, TypeError, AttributeError):
        raise PatchError(f"Flips provenance in {destination} is malformed; move it aside and rerun prepare") from None
    if not isinstance(archive_name, str) or "/" in archive_name or notices != pin["notices"]:
        raise PatchError("Flips provenance does not cover the pinned archive and notices")
    archive = read_private(destination / archive_name, pin["size"], "Flips source archive")[0]
    if sha256(archive) != pin["sha256"]:
        raise PatchError("Retained Flips source archive does not match its pin")
    for name, digest in notices.items():
        if sha256(read_private(destination / "notices" / name, MAX_MANIFEST, "Flips notice")[0]) != digest:
            raise PatchError(f"Retained Flips notice {name} changed")
    binary = read_private(destination / "flips", 16 * 1024 * 1024, "Flips binary")[0]
    if expected is None or binary_record != expected or sha256(binary) != expected["sha256"]:
        raise PatchError("Retained Flips binary does not match its pin")
    with tempfile.TemporaryDirectory(prefix=".flips-check-", dir=tools) as temporary:
        if sha256(_image_binary(image_id, Path(temporary))) != expected["sha256"]:
            raise PatchError("Pinned Flips image does not contain the pinned binary")
    return record


def prepare_flips(root=ROOT, archive=None):
    """Build the pinned CLI once into ignored game/tools and return its verified provenance record."""
    pin, dockerfile, key, tools, destination = _tool_location(root)
    if destination.exists() or destination.is_symlink():
        return installed_flips(root)
    _private_dir(_private_dir(root / "game") / "tools")
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
        tag = f"dswt-flips:{pin['version']}-{key}"
        _docker("build", "--pull=false", "--tag", tag, "--iidfile", str(stage / "image-id"), str(context))
        image_id = (stage / "image-id").read_text().strip()
        binary = _image_binary(image_id, stage)
        if sha256(binary) != pin["binaries"][platform]["sha256"]:
            raise PatchError(f"Built Flips binary {sha256(binary)} does not match the {platform} pin")
        for name, digest in pin["notices"].items():
            target = stage / "notices" / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(context / "source" / name, target)
            if sha256(target.read_bytes()) != digest:
                raise PatchError(f"Flips notice {name} does not match its pin")
        shutil.rmtree(context)
        record = {
            "tool": "flips", "version": pin["version"], "revision": pin["revision"],
            "archive": {"url": pin["url"], "size": pin["size"], "sha256": pin["sha256"], "file": downloaded.name},
            "license": pin["license"],
            "notices": pin["notices"],
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
    if bundle.cartridge(data) is None:
        raise PatchError(f"{variant['name']} does not have a Pokémon Platinum header")
    return variant


def _profile_binding(profile_bytes):
    try:
        profile = json.loads(profile_bytes)
        configure_connection.header(profile)
    except (ValueError, TypeError) as error:
        raise PatchError(f"Invalid connection profile: {error}") from None
    hostname = profile["hostname"].lower().rstrip(".")
    if (any(hostname == suffix or hostname.endswith("." + suffix) for suffix in RESERVED_SUFFIXES)
            or not ipaddress.ip_address(profile["ipv4"]).is_global):
        raise PatchError("A release must use the public production profile, not a local or test endpoint")
    pins = [bytes.fromhex(profile[name]) for name in ("current_spki_sha256", "next_spki_sha256")]
    return f"{profile['hostname']}:{profile['port']}".encode(), pins


def forbidden_secrets(paths):
    """Return (label, bytes) for each private key file: its bytes, DER body and 32-byte scalars."""
    found = []
    for path in paths:
        data = read_private(path, MAX_PROFILE, "Private key")[0]
        label = Path(path).name
        found.append((label, data))
        lines = data.decode("ascii", "replace").splitlines()
        body = "".join(line.strip() for line in lines if line.strip() and not line.startswith("-----"))
        if body:
            found.append((label, body.encode()))
            try:
                der = base64.b64decode(body, validate=True)
            except ValueError:
                raise PatchError(f"{label} is not a PEM private key") from None
            found.append((label, der))
            position = der.find(b"\x04\x20")
            while position >= 0 and position + 34 <= len(der):
                found.append((label, der[position + 2:position + 34]))
                position = der.find(b"\x04\x20", position + 1)
    return [(label, value) for label, value in found if len(value) >= 16]


def release_check(variant, bases, target, patch, report, secrets, root):
    """Fail if a patch or its target carries anything beyond the project's own build output."""
    problems = []
    for label, value in secrets:
        if value in target or value in patch:
            problems.append(f"material from private key {label}")
    for marker in sorted({str(root), str(Path.home()), "/home/", "/Users/", "C:\\Users\\", "-----BEGIN", "PRIVATE KEY"}):
        encoded = marker.encode()
        if target.count(encoded) > max(base.count(encoded) for base in bases):
            problems.append(f"build-host or key text {marker!r}")
    for item in report["literals"]["items"]:
        if item["length"] < VERBATIM_WINDOW + VERBATIM_STRIDE - 1:
            continue
        literal = patch[item["patch_offset"]:item["patch_offset"] + item["length"]]
        windows = (literal[k:k + VERBATIM_WINDOW] for k in range(0, len(literal) - VERBATIM_WINDOW + 1, VERBATIM_STRIDE))
        if any(window in base for window in windows for base in bases):
            problems.append(f"retail ROM content copied into a literal at target offset {item['target_offset']:#x}")
            break
    if problems:
        raise PatchError(f"Release check failed for {variant['id']}: " + "; ".join(problems))


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
    """Create the patch twice, require identical bytes, walk it and round-trip it with both appliers."""
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
    try:
        portable = bps.apply(base, patches[0])
    except bps.BPSError as error:
        raise PatchError(f"Portable applier rejected the patch for {prefix}: {error}") from None
    if portable != target:
        raise PatchError(f"Portable applier did not reproduce the target for {prefix}")
    stage.remove(*(f"{prefix}.{suffix}" for suffix in ("source", "target", "a.bps", "b.bps", "roundtrip")))
    return patches[0], report


def create(pairs, profile_path, output, version, flips, source, catalog=None, root=ROOT, secret_paths=None):
    """Generate one checked release bundle and publish its directory atomically without replacement."""
    if not VERSION.fullmatch(version):
        raise PatchError("Version must be 1-64 characters of [0-9A-Za-z.+-]")
    if source.get("dirty") is not False or not re.fullmatch(r"[0-9a-f]{40}", source.get("commit", "")):
        raise PatchError("Generate only from a clean, committed source tree")
    catalog = catalog or json.loads((root / "patches/compatibility.json").read_text())
    profile_bytes = read_private(profile_path, MAX_PROFILE, "Connection profile")[0]
    authority, pins = _profile_binding(profile_bytes)
    if secret_paths is None:
        secret_paths = sorted(Path(profile_path).resolve().parent.glob("*.key"))
    secrets_found = forbidden_secrets(secret_paths)
    parent, name = _new_output(output, root)
    inputs = []
    for base_path, target_path in pairs:
        base = read_private(base_path, bps.MAX_FILE, "Base ROM")[0]
        variant = _match(catalog, base)
        if any(item[0]["id"] == variant["id"] for item in inputs):
            raise PatchError(f"{variant['id']} was supplied more than once")
        target = read_private(target_path, bps.MAX_FILE, "Target ROM")[0]
        _check_target(variant, base, target, authority, pins)
        inputs.append((variant, base, target))
    bases = [base for _, base, _ in inputs]
    entries, files = [], {}
    with Stage(parent, f"{name}.partial") as stage:
        for variant, base, target in inputs:
            patch, report = _generate(stage, flips, variant, base, target)
            release_check(variant, bases, target, patch, report, secrets_found, root)
            literals = literal_bytes(patch, report)
            prefix, filename = variant["id"], variant["planned_patch"]
            cartridge = bundle.cartridge(base)
            files[filename] = (patch, 0o644)
            files[f"audit/{prefix}.json"] = (to_json(report), 0o600)
            files[f"audit/{prefix}.literals.bin"] = (literals, 0o600)
            entries.append({
                "variant": prefix, "name": variant["name"],
                "game_code": cartridge["game_code"], "revision": cartridge["revision"], "language": cartridge["language"],
                "input": {"size": len(base), "sha256": variant["sha256"], "crc32": report["source"]["crc32"]},
                "patch": {"file": filename, "size": len(patch), "sha256": report["patch"]["sha256"], "crc32": report["patch"]["crc32"]},
                "output": {"size": len(target), "sha256": sha256(target), "crc32": report["target"]["crc32"]},
                "actions": report["actions"],
                "literals": {"count": report["literals"]["count"], "bytes": report["literals"]["bytes"],
                             "sha256": sha256(literals)},
            })
        entries.sort(key=lambda entry: entry["variant"])
        manifest = {
            "schema_version": bundle.SCHEMA_VERSION, "kind": bundle.KIND, "version": version,
            "source": {"commit": source["commit"], "dirty": False},
            "upstream": {"repository": source["upstream_repository"], "revision": source["upstream_revision"]},
            "connection_profile_sha256": sha256(profile_bytes),
            "tool": tool_summary(flips.record),
            "release_checks": {
                "private_keys_checked": len(secret_paths),
                "verbatim_retail_run_limit": VERBATIM_WINDOW + VERBATIM_STRIDE - 1,
                "build_host_paths": "absent", "public_profile": True,
                "appliers": ["flips", "bps.py"],
            },
            "entries": entries,
        }
        files["manifest.json"] = (to_json(manifest), 0o644)
        bundle.load_manifest(files["manifest.json"][0])
        stage.write_tree("bundle", files)
        stage.publish("bundle", name)
    return manifest


def load_manifest(data):
    try:
        return bundle.load_manifest(data)
    except bundle.BundleError as error:
        raise PatchError(str(error)) from None


def apply(manifest_path, input_path, output_path):
    """Patch one verified input to a new output with the portable applier."""
    manifest_path = Path(manifest_path)
    if manifest_path.name != "manifest.json":
        raise PatchError("Pass the bundle's manifest.json")
    try:
        entry, _ = bundle.patch_file(manifest_path.parent, input_path, output_path)
    except bundle.BundleError as error:
        raise PatchError(str(error)) from None
    return entry


def _zip_entry(name, data):
    info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = 0o644 << 16
    return info, data


def package(bundle_dir, output, root=ROOT):
    """Write the reproducible player archive: manifest, patches and release documents, never audit data."""
    bundle_dir = Path(bundle_dir)
    manifest_bytes = read_private(bundle_dir / "manifest.json", MAX_MANIFEST, "Manifest")[0]
    manifest = load_manifest(manifest_bytes)
    prefix = f"dswt-{manifest['version']}/"
    members = {"manifest.json": manifest_bytes}
    for entry in manifest["entries"]:
        patch = read_private(bundle_dir / entry["patch"]["file"], bps.MAX_PATCH, entry["patch"]["file"])[0]
        if len(patch) != entry["patch"]["size"] or sha256(patch) != entry["patch"]["sha256"]:
            raise PatchError(f"{entry['patch']['file']} does not match the manifest")
        walk(patch, entry["patch"]["file"])
        members[entry["patch"]["file"]] = patch
    for name, relative in RELEASE_DOCUMENTS.items():
        members[name] = (root / relative).read_bytes()
    parent, name = _new_output(output, root)
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w") as archive:
        for member in sorted(members):
            archive.writestr(*_zip_entry(prefix + member, members[member]))
    with Stage(parent, f"{name}.partial") as stage:
        stage.write("archive.zip", buffer.getvalue(), 0o644)
        stage.publish("archive.zip", name)
    return {"archive": str(parent / name), "sha256": sha256(buffer.getvalue()), "size": len(buffer.getvalue())}


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
    create_parser = commands.add_parser("create", help="generate a checked release bundle under game/")
    create_parser.add_argument("--pair", nargs=2, action="append", required=True, metavar=("BASE", "TARGET"), type=Path)
    create_parser.add_argument("--profile", required=True, type=Path)
    create_parser.add_argument("--output", required=True, type=Path)
    create_parser.add_argument("--version", required=True)
    create_parser.add_argument("--forbid-key", action="append", type=Path, dest="keys",
                               help="private key that must not appear (default: every *.key beside the profile)")
    package_parser = commands.add_parser("package", help="write the player archive for a bundle under game/")
    package_parser.add_argument("bundle", type=Path)
    package_parser.add_argument("--output", required=True, type=Path)
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
            manifest = create(args.pair, args.profile, args.output, args.version, Flips(installed_flips()),
                              source_state(), secret_paths=args.keys)
            result = {"output": str(args.output), "entries": manifest["entries"]}
        elif args.command == "package":
            result = package(args.bundle, args.output)
        else:
            entry = apply(args.manifest, args.input, args.output)
            result = {"variant": entry["variant"], "output": str(args.output), "sha256": entry["output"]["sha256"]}
    except KeyboardInterrupt:
        print("Interrupted; inputs and any existing output were preserved", file=sys.stderr)
        return 130
    except (OSError, ValueError, subprocess.SubprocessError, tarfile.TarError, zipfile.BadZipFile) as error:
        print(f"{args.command} failed: {error}", file=sys.stderr)
        return 1
    print(to_json(result).decode(), end="")
    return 0


if __name__ == "__main__":
    sys.exit(main())
