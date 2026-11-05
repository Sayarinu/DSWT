"""Inspect OCI exports without unpacking an untrusted filesystem onto the host."""

import gzip
import hashlib
import json
from pathlib import PurePosixPath
import posixpath
import re
import shutil
import tarfile


PRIVATE_SENTINEL = b"DSWT_PRIVATE_INPUT_SENTINEL"
SOURCE = "usr/share/dswt-validator/source/"
KEY_LABEL = rb"(?:RSA |EC |DSA |OPENSSH |ENCRYPTED )?PRIVATE KEY"
KEY_PREFIX = re.compile(rb"-----BEGIN " + KEY_LABEL + rb"-----\s*"
                        rb"(?:(?:Proc-Type:|DEK-Info:)[^\r\n]*\r?\n\s*)*(?:[A-Za-z0-9+/=]\s*){32}")
KEY_BLOCK = re.compile(rb"-----BEGIN (" + KEY_LABEL + rb")-----[A-Za-z0-9+/=\r\n]+-----END \1-----")


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def safe_path(value):
    if not isinstance(value, str) or not value or "\\" in value or "\x00" in value:
        raise ValueError("Invalid archive path")
    while value.startswith("./"):
        value = value[2:]
    if not value or value.startswith("/") or ".." in value.split("/"):
        raise ValueError("Unsafe archive path: " + value)
    return str(PurePosixPath(value))


def prohibited(path):
    parts = path.lower().split("/")
    name = parts[-1]
    return (any(p in (".git", ".aws", ".codex", ".agents", "pokeplatinum", "nitrosdk") for p in parts)
            or name == ".env" or name.startswith(".env.")
            or name in ("bios7.bin", "bios9.bin", "firmware.bin")
            or name.endswith((".nds", ".srl", ".sav", ".dsv", ".pk4", ".nef")))


def private_marker(data):
    # Header-only parser/template strings are not encoded key material.
    return PRIVATE_SENTINEL in data or KEY_PREFIX.search(data)


class HashReader:
    def __init__(self, stream):
        self.stream = stream
        self.digest = hashlib.sha256()
        self.size, self.last_nonzero, self.tail = 0, -1, b""
        self.key_offsets = set()

    def read(self, size=-1):
        data = self.stream.read(size)
        self.digest.update(data)
        check = self.tail + data
        if PRIVATE_SENTINEL in check:
            raise ValueError("Private input marker in image layer bytes")
        self.key_offsets.update(self.size - len(self.tail) + match.start() for match in KEY_PREFIX.finditer(check))
        self.tail = check[-4096:]
        nonzero = len(data.rstrip(b"\0"))
        if nonzero:
            self.last_nonzero = self.size + nonzero - 1
        self.size += len(data)
        return data


class Image:
    def __init__(self, public_selftests=None):
        self.files = {}
        self.layers = []
        self.public_policy = public_selftests or {"libraries": {}, "keys": {}}
        self.public_selftests = []

    def apply_layer(self, stream, expected_diff_id, digest):
        reader = HashReader(stream)
        records, whiteouts, seen = [], [], set()
        allowed_key_offsets = set()
        with tarfile.open(fileobj=reader, mode="r|") as archive:
            for member in archive:
                if member.name in (".", "./") and member.isdir():
                    continue
                path = safe_path(member.name)
                if path in seen:
                    raise ValueError("Duplicate path within layer: " + path)
                seen.add(path)
                if prohibited(path):
                    raise ValueError("Prohibited file in image layer: " + path)
                parent, _, name = path.rpartition("/")
                if name.startswith(".wh."):
                    if not member.isfile() or member.size:
                        raise ValueError("Invalid OCI whiteout")
                    whiteouts.append((parent, name))
                    continue
                entry = {"path": path, "mode": member.mode, "uid": member.uid, "gid": member.gid}
                if member.isdir():
                    entry["type"] = "directory"
                elif member.issym():
                    if "\x00" in member.linkname:
                        raise ValueError("Invalid symlink")
                    entry.update(type="symlink", target=member.linkname)
                elif member.islnk():
                    entry.update(type="hardlink", target=safe_path(member.linkname))
                elif member.isfile():
                    public = self.public_policy["libraries"].get(path)
                    capture = (path.startswith(SOURCE) or path.startswith("usr/share/dswt/")
                               or path.startswith("usr/local/share/dswt/migrations/")
                               or path in ("app/PKHeX.Core.dll", "var/lib/dpkg/status") or public is not None)
                    chunks, tail, size, content_hash = [], b"", 0, hashlib.sha256()
                    source = archive.extractfile(member)
                    while chunk := source.read(1024 * 1024):
                        content_hash.update(chunk)
                        size += len(chunk)
                        check = tail + chunk
                        if PRIVATE_SENTINEL in check or (public is None and KEY_PREFIX.search(check)):
                            raise ValueError("Private input marker in image layer: " + path)
                        tail = check[-4096:]
                        if capture:
                            chunks.append(chunk)
                    if size != member.size:
                        raise ValueError("Truncated layer file")
                    entry.update(type="file", size=size, sha256=content_hash.hexdigest())
                    if capture:
                        entry["data"] = b"".join(chunks)
                    if public is not None:
                        data = entry.pop("data")
                        keys = {match.start(): sha256(match.group()) for match in KEY_BLOCK.finditer(data)}
                        if entry["sha256"] != public["sha256"] or set(keys.values()) != set(self.public_policy["keys"]):
                            raise ValueError("Public self-test library/hash mismatch: " + path)
                        if {match.start() for match in KEY_PREFIX.finditer(data)} != set(keys):
                            raise ValueError("Unknown encoded key in public self-test library: " + path)
                        allowed_key_offsets.update(member.offset_data + offset for offset in keys)
                        self.public_selftests.append({"path": path, "sha256": entry["sha256"],
                                                      "layer": digest, "keys": sorted(keys.values())})
                else:
                    raise ValueError("Unsupported device or special file in layer: " + path)
                records.append(entry)
            logical_end = archive.offset
        while reader.read(1024 * 1024):
            pass
        if reader.size - logical_end < 1024 or reader.last_nonzero >= logical_end:
            raise ValueError("Layer tar has missing terminator or nonzero trailing data")
        if reader.key_offsets != allowed_key_offsets:
            raise ValueError("Unknown encoded private key in layer bytes")
        actual = "sha256:" + reader.digest.hexdigest()
        if actual != expected_diff_id:
            raise ValueError("Layer uncompressed diff ID mismatch")
        # Whiteouts affect lower layers only, regardless of tar entry order.
        for parent, name in whiteouts:
            target = parent + "/" if parent else ""
            opaque = name == ".wh..wh..opq"
            if not opaque:
                target += name[4:]
            for path in list(self.files):
                if (opaque and path.startswith(target)) or (not opaque and (path == target or path.startswith(target + "/"))):
                    del self.files[path]
        for entry in records:
            path = entry["path"]
            for ancestor in PurePosixPath(path).parents:
                prior = self.files.get(str(ancestor))
                if prior and prior["type"] != "directory":
                    raise ValueError("Layer writes through a non-directory: " + path)
            if entry["type"] != "directory":
                for child in list(self.files):
                    if child.startswith(path + "/"):
                        del self.files[child]
            self.files[path] = entry
        def resolve_link(path, resolving):
            entry = self.files.get(path)
            if entry is None or path in resolving:
                raise ValueError("Missing or cyclic image hardlink")
            if entry["type"] == "hardlink":
                target = resolve_link(entry["target"], resolving | {path})
                if target["type"] != "file":
                    raise ValueError("Image hardlink must reference a regular file")
                if any(target["sha256"] == record["sha256"] for record in self.public_selftests):
                    raise ValueError("Public self-test library at an unapproved hardlink path: " + path)
                entry.update({key: target[key] for key in ("size", "sha256", "data") if key in target})
                entry["type"] = "file"
                del entry["target"]
            return entry
        for path in self.files:
            resolve_link(path, set())
        self.layers.append({"digest": digest, "diff_id": actual, "entries": len(seen),
                            "whiteouts": len(whiteouts)})

    def data(self, path):
        entry = self.files.get(path)
        if not entry or entry["type"] != "file" or "data" not in entry:
            raise ValueError("Missing required regular image file: " + path)
        return entry["data"]

    def inventory(self):
        return {path: {k: v for k, v in entry.items() if k not in ("data", "path")}
                for path, entry in sorted(self.files.items())}

    def resolve_file(self, path):
        path = safe_path(path)
        for _ in range(40):
            parts = path.split("/")
            for index in range(len(parts)):
                prefix = "/".join(parts[:index + 1])
                entry = self.files.get(prefix)
                if entry and entry["type"] == "symlink":
                    target = posixpath.join(posixpath.dirname(prefix), entry["target"])
                    path = posixpath.normpath("/" + posixpath.join(target, *parts[index + 1:])).lstrip("/")
                    break
            else:
                entry = self.files.get(path)
                if not entry or entry["type"] != "file":
                    raise ValueError("Missing required image file: " + path)
                return path, entry
        raise ValueError("Image symlink resolution limit exceeded")


class Archive:
    def __init__(self, path, public_selftests=None):
        self.public_policy = public_selftests
        with open(path, "rb") as source:
            tail = b""
            while chunk := source.read(1024 * 1024):
                if private_marker(tail + chunk):
                    raise ValueError("Private input marker in OCI archive bytes")
                tail = chunk[-4096:]
        self.tar = tarfile.open(path, "r:")
        try:
            self.load()
        except Exception:
            self.tar.close()
            raise

    def load(self):
        self.entries, self.verified = {}, set()
        for entry in self.tar.getmembers():
            path = safe_path(entry.name)
            if entry.isdir():
                continue
            if not entry.isfile() or path in self.entries:
                raise ValueError("Invalid or duplicate OCI archive entry")
            if path not in ("oci-layout", "index.json") and not re.fullmatch(r"blobs/sha256/[0-9a-f]{64}", path):
                raise ValueError("Unexpected OCI archive entry: " + path)
            self.entries[path] = entry
        logical_end = self.tar.offset
        self.tar.fileobj.seek(logical_end)
        padding = 0
        while chunk := self.tar.fileobj.read(1024 * 1024):
            if chunk.strip(b"\0"):
                raise ValueError("OCI tar has nonzero trailing data")
            padding += len(chunk)
        if padding < 1024:
            raise ValueError("OCI tar has no complete terminator")
        if self.json_file("oci-layout") != {"imageLayoutVersion": "1.0.0"}:
            raise ValueError("Unsupported OCI layout")
        self.index_bytes = self.read("index.json")
        self.index = self.json_file("index.json")

    def close(self):
        self.tar.close()

    def write_layout(self, destination):
        # Trivy accepts an OCI directory, not BuildKit's OCI tar export.
        destination.mkdir()
        for path, entry in self.entries.items():
            target = destination / path
            target.parent.mkdir(parents=True, exist_ok=True)
            with self.tar.extractfile(entry) as source, target.open("xb") as output:
                shutil.copyfileobj(source, output)
        self.verify_layout(destination)

    def verify_layout(self, destination):
        members = list(destination.rglob("*"))
        if any(path.is_symlink() for path in members):
            raise ValueError("Scanner OCI layout contains a symlink")
        paths = {path.relative_to(destination).as_posix() for path in members if not path.is_dir()}
        if paths != set(self.entries):
            raise ValueError("Scanner OCI layout file set differs from audited export")
        for path in paths:
            target = destination / path
            if target.is_symlink() or not target.is_file() or target.stat().st_size != self.entries[path].size:
                raise ValueError("Scanner OCI layout file differs from audited export")
            expected = path[13:] if path.startswith("blobs/sha256/") else sha256(self.read(path))
            with target.open("rb") as source:
                if hashlib.file_digest(source, "sha256").hexdigest() != expected:
                    raise ValueError("Scanner OCI layout bytes differ from audited export")

    def read(self, path):
        entry = self.entries[path]
        if entry.size > 16 * 1024 * 1024:
            raise ValueError("Oversized OCI metadata")
        return self.tar.extractfile(entry).read()

    def json_file(self, path):
        document = json.loads(self.read(path))
        pending = [document]
        while pending:
            value = pending.pop()
            if isinstance(value, str) and private_marker(value.encode()):
                raise ValueError("Private input marker in decoded OCI metadata")
            if isinstance(value, dict):
                pending.extend(value)
                pending.extend(value.values())
            elif isinstance(value, list):
                pending.extend(value)
        return document

    def blob_path(self, descriptor):
        digest = descriptor.get("digest", "")
        if not re.fullmatch(r"sha256:[0-9a-f]{64}", digest):
            raise ValueError("Unsupported OCI digest")
        path = "blobs/sha256/" + digest[7:]
        entry = self.entries.get(path)
        if entry is None or entry.size != descriptor.get("size"):
            raise ValueError("OCI descriptor size/blob mismatch")
        if path not in self.verified:
            hasher = hashlib.sha256()
            with self.tar.extractfile(entry) as stream:
                while chunk := stream.read(1024 * 1024):
                    hasher.update(chunk)
            if hasher.hexdigest() != digest[7:]:
                raise ValueError("OCI blob digest mismatch")
            self.verified.add(path)
        return path

    def inspect(self, platform):
        manifests, attestations = [], []

        def walk(document, descriptor=None):
            if document.get("schemaVersion") != 2:
                raise ValueError("Unsupported OCI schema")
            if "manifests" in document:
                for child in document["manifests"]:
                    walk(self.json_file(self.blob_path(child)), child)
                return
            config = self.json_file(self.blob_path(document["config"]))
            for layer in document["layers"]:
                self.blob_path(layer)
            if any(layer["mediaType"] == "application/vnd.in-toto+json" for layer in document["layers"]):
                for layer in document["layers"]:
                    if layer["mediaType"] != "application/vnd.in-toto+json":
                        raise ValueError("Mixed attestation/image layers")
                    reference = descriptor.get("annotations", {}).get("vnd.docker.reference.digest")
                    attestations.append((self.json_file(self.blob_path(layer)), reference))
            else:
                manifests.append((descriptor, document, config))

        walk(self.index)
        if len(manifests) != 1:
            raise ValueError("Export exactly one image platform per audit")
        descriptor, manifest, config = manifests[0]
        if config.get("os") + "/" + config.get("architecture") != platform:
            raise ValueError("Image platform mismatch")
        diff_ids = config.get("rootfs", {}).get("diff_ids", [])
        if config.get("rootfs", {}).get("type") != "layers" or len(diff_ids) != len(manifest["layers"]):
            raise ValueError("OCI rootfs/layer mismatch")
        image = Image(self.public_policy)
        for layer, diff_id in zip(manifest["layers"], diff_ids):
            kind = layer["mediaType"]
            stream = self.tar.extractfile(self.entries[self.blob_path(layer)])
            if kind == "application/vnd.oci.image.layer.v1.tar+gzip":
                stream = gzip.GzipFile(fileobj=stream)
            elif kind != "application/vnd.oci.image.layer.v1.tar":
                raise ValueError("Unsupported OCI compression/media type")
            with stream:
                image.apply_layer(stream, diff_id, layer["digest"])
        provenances = []
        for statement, reference in attestations:
            if statement.get("predicateType") not in ("https://slsa.dev/provenance/v0.2", "https://slsa.dev/provenance/v1"):
                continue
            if reference != descriptor["digest"]:
                raise ValueError("Attestation descriptor does not bind the image manifest")
            if not any(s.get("digest", {}).get("sha256") == descriptor["digest"][7:]
                       for s in statement.get("subject", [])):
                raise ValueError("Provenance subject does not bind the image manifest")
            provenances.append(statement)
        if not provenances:
            raise ValueError("Missing OCI provenance")
        if self.verified != {p for p in self.entries if p.startswith("blobs/")}:
            raise ValueError("Unreferenced OCI blobs were not audited")
        layout_digest = "sha256:" + sha256(self.index_bytes)
        roots = self.index["manifests"]
        image_index = roots[0]["digest"] if len(roots) == 1 and roots[0]["mediaType"] == "application/vnd.oci.image.index.v1+json" else layout_digest
        metadata = {"layout_index_digest": layout_digest, "index_digest": image_index,
                    "manifest_digest": descriptor["digest"], "config_digest": manifest["config"]["digest"],
                    "platform": platform, "layers": image.layers, "provenance": provenances,
                    "public_selftests": image.public_selftests,
                    "config": config.get("config", {})}
        return image, metadata
