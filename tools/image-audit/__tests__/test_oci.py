import gzip
import io
import json
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import oci


def tar_bytes(files):
    output = io.BytesIO()
    with tarfile.open(fileobj=output, mode="w") as archive:
        for path, data in files:
            entry = tarfile.TarInfo(path)
            if isinstance(data, tuple):
                entry.type, entry.linkname = data
                archive.addfile(entry)
            else:
                entry.size = len(data)
                archive.addfile(entry, io.BytesIO(data))
    return output.getvalue()


def make_export(path, layers, *, wrong_diff=False, wrong_subject=False, corrupt=False, nested=False, environment=None):
    blobs = {}

    def blob(data, kind):
        digest = oci.sha256(data)
        blobs["blobs/sha256/" + digest] = data
        return {"mediaType": kind, "digest": "sha256:" + digest, "size": len(data)}

    def document(value, kind):
        return blob(json.dumps(value).encode(), "application/vnd.oci.image." + kind + ".v1+json")

    config = document({"os": "linux", "architecture": "arm64", "config": {"User": "65532:65532", "Env": environment or []},
                       "rootfs": {"type": "layers", "diff_ids": ["sha256:" + ("0" * 64 if wrong_diff else oci.sha256(x)) for x in layers]}}, "config")
    manifest = document({"schemaVersion": 2, "config": config,
                         "layers": [blob(gzip.compress(x), "application/vnd.oci.image.layer.v1.tar+gzip") for x in layers]}, "manifest")
    provenance = blob(json.dumps({"predicateType": "https://slsa.dev/provenance/v0.2", "predicate": {"buildType": "test"},
                                 "subject": [{"digest": {"sha256": "0" * 64 if wrong_subject else manifest["digest"][7:]}}]}).encode(), "application/vnd.in-toto+json")
    attestation = document({"schemaVersion": 2, "config": document({}, "config"), "layers": [provenance]}, "manifest")
    attestation["annotations"] = {"vnd.docker.reference.digest": manifest["digest"], "vnd.docker.reference.type": "attestation-manifest"}
    image_index = {"schemaVersion": 2, "manifests": [manifest, attestation]}
    index = json.dumps({"schemaVersion": 2, "manifests": [document(image_index, "index")]} if nested else image_index).encode()
    if corrupt:
        first = next(iter(blobs))
        blobs[first] = b"x" * len(blobs[first])
    path.write_bytes(tar_bytes([("oci-layout", b'{"imageLayoutVersion":"1.0.0"}'), ("index.json", index), *blobs.items()]))
    return "sha256:" + oci.sha256(json.dumps(image_index).encode())


class OciTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / "image.tar"

    def inspect(self, layers, *, public_policy=None, **kwargs):
        make_export(self.path, layers, **kwargs)
        archive = oci.Archive(self.path, public_policy)
        try:
            return archive.inspect("linux/arm64")
        finally:
            archive.close()

    def test_all_layers_and_whiteouts_produce_final_files(self):
        lower = tar_bytes([("keep", b"same"), ("removed", b"old"), ("dir/old", b"old")])
        upper = tar_bytes([("dir/new", b"new"), ("dir/.wh..wh..opq", b""), (".wh.removed", b"")])
        image, metadata = self.inspect([lower, upper])
        self.assertEqual(set(image.files), {"keep", "dir/new"})
        self.assertEqual(image.files["dir/new"]["sha256"], oci.sha256(b"new"))
        self.assertEqual(len(metadata["layers"]), 2)
        self.assertEqual(metadata["layers"][1]["whiteouts"], 2)
        self.assertNotEqual(metadata["index_digest"], metadata["manifest_digest"])
        self.assertNotEqual(metadata["manifest_digest"], metadata["config_digest"])

    def test_removed_private_content_and_prohibited_names_are_still_rejected(self):
        for name, data in [("innocent.txt", oci.PRIVATE_SENTINEL), ("private.sav", b"payload"),
                           ("config.txt", (b'-----' + b'BEGIN PRIVATE KEY-----\n') + b"A" * 64)]:
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, "Prohibited|Private input"):
                self.inspect([tar_bytes([(name, data)]), tar_bytes([(".wh." + name, b"")])])

    def test_header_templates_differ_from_encoded_private_key_material(self):
        self.inspect([tar_bytes([("parser", (b'-----' + b'BEGIN PRIVATE KEY-----\x00format string'))])])
        for label in (b"RSA ", b"EC ", b"DSA ", b"OPENSSH ", b"ENCRYPTED ", b""):
            for newline in (b"", b"\n"):
                key = b"-----BEGIN " + label + b"PRIVATE KEY-----" + newline + b"A" * 64
                with self.subTest(label=label, newline=newline), self.assertRaisesRegex(ValueError, "Private input"):
                    self.inspect([tar_bytes([("unknown", key)])])
        wrapped = (b'-----' + b'BEGIN PRIVATE KEY-----\n') + (b"A" * 16 + b"\n") * 4
        with self.assertRaisesRegex(ValueError, "Private input"):
            self.inspect([tar_bytes([("unknown", wrapped)])])
        for whitespace in (b" ", b"\t", b"\r\n", b"\v", b"\f"):
            key = (b'-----' + b'BEGIN RSA PRIVATE KEY-----\n') + whitespace.join([b"A"] * 64) + (b'\n-----' + b'END RSA PRIVATE KEY-----')
            with self.subTest(whitespace=whitespace), self.assertRaisesRegex(ValueError, "Private input"):
                self.inspect([tar_bytes([("unknown", key)])])

    def test_encoded_private_key_in_json_metadata_is_rejected_after_unescaping(self):
        key = ('-----' + 'BEGIN PRIVATE KEY-----\n') + "A" * 64 + ('\n-----' + 'END PRIVATE KEY-----')
        with self.assertRaisesRegex(ValueError, "decoded OCI metadata"):
            self.inspect([tar_bytes([("ordinary", b"payload")])], environment=["KEY=" + key])

    def test_public_selftest_requires_exact_file_path_hash_and_all_key_blocks(self):
        # Synthetic text is not a usable key; real public key bytes are never vendored.
        key = (b'-----' + b'BEGIN EC PRIVATE KEY-----') + b"A" * 64 + (b'-----' + b'END EC PRIVATE KEY-----')
        binary = b"synthetic library\0" + key + b"\0"
        path = "usr/lib/example.so"
        policy = {"libraries": {path: {"sha256": oci.sha256(binary)}}, "keys": {oci.sha256(key): "test_constant"}}
        image, metadata = self.inspect([tar_bytes([(path, binary)])], public_policy=policy)
        self.assertEqual(metadata["public_selftests"][0]["keys"], [oci.sha256(key)])
        for name, data in (("wrong-location", binary), (path, binary + b"changed"),
                           (path, binary.replace(b"A" * 64, b"B" * 64))):
            with self.subTest(path=name), self.assertRaisesRegex(ValueError, "Private input|self-test"):
                self.inspect([tar_bytes([(name, data)])], public_policy=policy)
        with self.assertRaisesRegex(ValueError, "hardlink"):
            self.inspect([tar_bytes([(path, binary), ("alias", (tarfile.LNKTYPE, path))])], public_policy=policy)

    def test_hardlink_keeps_its_bytes_when_original_path_is_removed(self):
        image, _ = self.inspect([tar_bytes([("original", b"retained bytes"), ("alias", (tarfile.LNKTYPE, "original"))]),
                                 tar_bytes([(".wh.original", b"")])])
        self.assertEqual(set(image.files), {"alias"})
        self.assertEqual(image.files["alias"]["sha256"], oci.sha256(b"retained bytes"))

    def test_nonzero_bytes_after_layer_or_outer_tar_eof_are_rejected(self):
        layer = tar_bytes([("ordinary", b"payload")])
        # Digests are deliberately correct: the rejection must inspect beyond EOF.
        for trailing in (b"private bytes", oci.PRIVATE_SENTINEL, tar_bytes([("hidden", b"secret")])):
            with self.subTest(where="layer", length=len(trailing)), self.assertRaisesRegex(ValueError, "trailing data|Private input"):
                self.inspect([layer + trailing])
            make_export(self.path, [layer])
            with self.path.open("ab") as output:
                output.write(trailing)
            with self.subTest(where="outer", length=len(trailing)), self.assertRaisesRegex(ValueError, "trailing data|Private input"):
                oci.Archive(self.path)

    def test_digest_diff_id_and_provenance_subject_are_verified(self):
        for flag, error in [("corrupt", "blob digest"), ("wrong_diff", "diff ID"), ("wrong_subject", "subject")]:
            with self.subTest(flag=flag), self.assertRaisesRegex(ValueError, error):
                self.inspect([tar_bytes([("ordinary", b"payload")])], **{flag: True})

    def test_traversal_duplicate_and_link_writes_are_rejected(self):
        cases = [[tar_bytes([("../escape", b"bad")])], [tar_bytes([("same", b"a"), ("same", b"b")])],
                 [tar_bytes([("directory", (tarfile.SYMTYPE, "/outside"))]), tar_bytes([("directory/file", b"bad")])]]
        for layers in cases:
            with self.subTest(layers=len(layers)), self.assertRaises(ValueError):
                self.inspect(layers)

    def test_wrong_platform_is_rejected(self):
        make_export(self.path, [tar_bytes([("ordinary", b"payload")])])
        archive = oci.Archive(self.path)
        try:
            with self.assertRaisesRegex(ValueError, "platform"):
                archive.inspect("linux/amd64")
        finally:
            archive.close()

    def test_layout_wrapper_is_distinct_from_the_deployable_image_index(self):
        expected = make_export(self.path, [tar_bytes([("ordinary", b"payload")])], nested=True)
        archive = oci.Archive(self.path)
        try:
            _, metadata = archive.inspect("linux/arm64")
            self.assertEqual(metadata["index_digest"], expected)
            self.assertNotEqual(metadata["layout_index_digest"], metadata["index_digest"])
        finally:
            archive.close()

    def test_scanner_directory_must_preserve_exact_oci_export_bytes(self):
        make_export(self.path, [tar_bytes([("ordinary", b"payload")])], nested=True)
        archive = oci.Archive(self.path)
        destination = self.path.parent / "layout"
        try:
            archive.inspect("linux/arm64")
            archive.write_layout(destination)
            archive.verify_layout(destination)
            index = destination / "index.json"
            original = index.read_bytes()
            index.write_bytes(original.replace(b"schemaVersion", b"schemaVersioX"))
            with self.assertRaisesRegex(ValueError, "layout bytes"):
                archive.verify_layout(destination)
            index.write_bytes(original)
            (destination / "unexpected").write_bytes(b"unreviewed")
            with self.assertRaisesRegex(ValueError, "file set"):
                archive.verify_layout(destination)
        finally:
            archive.close()


if __name__ == "__main__":
    unittest.main()
