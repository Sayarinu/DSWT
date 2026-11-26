"""Stage European inputs from a synthetic cartridge image; no retail data is used."""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest


spec = importlib.util.spec_from_file_location(
    "prepare_europe", Path(__file__).resolve().parents[1] / "prepare-europe.py"
)
prepare_europe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prepare_europe)


def narc(members):
    table = struct.pack("<4sIHH", b"BTAF", 12 + 8 * len(members), len(members), 0)
    offset, images = 0, b""
    for member in members:
        table += struct.pack("<II", offset, offset + len(member))
        images += member
        offset += len(member)
    names = struct.pack("<4sIIHH", b"BTNF", 16, 4, 0, 1)
    body = table + names + struct.pack("<4sI", b"GMIF", 8 + len(images)) + images
    return struct.pack("<4sHHIHH", b"NARC", 0xFFFE, 0x0100, 16 + len(body), 16, 3) + body


def cartridge(files, header_tail):
    """Build a minimal NDS image whose NitroFS holds files ({'/dir/name': bytes})."""
    tree = {}
    for path in sorted(files):
        *dirs, name = path.strip("/").split("/")
        node = tree
        for directory in dirs:
            node = node.setdefault(directory, {})
        node[name] = path
    directories = []

    def collect(node):
        directories.append(node)
        for value in node.values():
            if isinstance(value, dict):
                collect(value)

    collect(tree)
    numbers = {id(node): number for number, node in enumerate(directories)}
    order, first, subtables = [], [], []
    for node in directories:
        here = [(name, value) for name, value in node.items() if not isinstance(value, dict)]
        first.append(len(order))
        order += [value for _, value in here]
        table = b"".join(bytes([0x80 | len(name)]) + name.encode() + struct.pack("<H", 0xF000 | numbers[id(value)])
                         for name, value in node.items() if isinstance(value, dict))
        table += b"".join(bytes([len(name)]) + name.encode() for name, _ in here)
        subtables.append(table + b"\0")
    fnt, cursor = b"", 8 * len(directories)
    for number in range(len(directories)):
        fnt += struct.pack("<IHH", cursor, first[number], len(directories) if number == 0 else 0xF000)
        cursor += len(subtables[number])
    fnt += b"".join(subtables)
    fnt_offset = prepare_europe.HEADER_AREA
    fat_offset = fnt_offset + len(fnt) + (-len(fnt) % 4)
    data_offset = fat_offset + 8 * len(order)
    fat, data = b"", b""
    for path in order:
        start = data_offset + len(data)
        fat += struct.pack("<II", start, start + len(files[path]))
        data += files[path]
    header = bytearray(prepare_europe.HEADER_FIELDS)
    header[0x0C:0x10] = b"CPUE"
    header[0x1E] = prepare_europe.REVISION
    struct.pack_into("<IIII", header, 0x40, fnt_offset, len(fnt), fat_offset, len(fat))
    image = bytes(header) + header_tail
    return image + fnt + bytes(fat_offset - fnt_offset - len(fnt)) + fat + data


class PrepareEuropeTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix="dswt-europe-test-")
        self.addCleanup(temp.cleanup)
        self.workspace = Path(temp.name) / "workspace"
        platinum = self.workspace / "platinum.us"
        platinum.mkdir(parents=True)
        (self.workspace / ".git").mkdir()
        (self.workspace / "generated").mkdir()
        self.banks = [f"bank_{index}" for index in range(700)]
        (self.workspace / "generated/text_banks.txt").write_text(
            "".join(f"TEXT_BANK_{name.upper()}\n" for name in self.banks))
        (platinum / "rom_header_template_rev1.sbin").write_bytes(b"T" * prepare_europe.HEADER_AREA)
        (platinum / "rom_rev1.ini").write_text("[header]\nrevision   = 1\nsecure-crc = 0xF8B8\n")
        sources = {
            "/data/slot.narc": "res/prebuilt/data/slot.narc",
            "/data/other.bin": "res/prebuilt/data/other.bin",
            "/demo/title/op_demo.narc": "res/prebuilt/demo/title/op_demo.narc",
            "/fielddata/land_data/land_data.narc": "res/field/maps/data/land_data.narc",
            "/fielddata/script/scr_seq.narc": "res/field/scripts/scr_seq.narc",
            "/msgdata/pl_msg.narc": "res/text/pl_msg.narc",
        }
        (platinum / "filesys.csv").write_text(
            "Source File,Target File\n" + "".join(f"{source},{target}\n" for target, source in sources.items()))
        (platinum / "filesys.sha1").write_text(
            "".join(f"{'0' * 40} *{source}\n" for source in sources.values()))
        self.text = [f"bank {index}".encode() for index in range(700)]
        self.files = {target: f"european {target}".encode() for target in sources}
        self.files["/msgdata/pl_msg.narc"] = narc(self.text)
        self.tail = bytes(range(256)) * ((prepare_europe.HEADER_AREA - prepare_europe.HEADER_FIELDS) // 256) \
            + bytes((prepare_europe.HEADER_AREA - prepare_europe.HEADER_FIELDS) % 256)
        self.rom = cartridge(self.files, self.tail)
        self.variant = {"sha1": "e" * 40}

    def test_stages_only_the_european_differences(self):
        staged = prepare_europe.stage(self.workspace, self.rom, self.variant)
        self.assertEqual(staged["rom_header_template.sbin"],
                         b"T" * prepare_europe.HEADER_FIELDS + self.tail)
        self.assertIn(b"revision   = 10\n", staged["rom.ini"])
        self.assertEqual(staged["rom.sha1"], b"e" * 40 + b" *pokeplatinum.us.nds\n")
        for path in prepare_europe.FILES:
            self.assertEqual(staged["files" + path], self.files[path])
        self.assertNotIn("files/data/other.bin", staged)
        texts = {name: content for name, content in staged.items() if name.startswith("text/")}
        self.assertEqual(texts, {f"text/bank_{index}": self.text[index] for index in prepare_europe.TEXT_BANKS})
        layout = staged["filesys.csv"].decode().splitlines()
        self.assertIn("../.git/dswt-europe/files/data/slot.narc,/data/slot.narc", layout)
        self.assertIn("res/prebuilt/data/other.bin,/data/other.bin", layout)
        self.assertIn("res/text/pl_msg.narc,/msgdata/pl_msg.narc", layout)
        checks = staged["filesys.sha1"].decode().splitlines()
        self.assertIn(f"{hashlib.sha1(self.files['/data/slot.narc']).hexdigest()} "
                      "*../.git/dswt-europe/files/data/slot.narc", checks)
        self.assertIn(f"{hashlib.sha1(self.files['/msgdata/pl_msg.narc']).hexdigest()} *res/text/pl_msg.narc",
                      checks)
        self.assertIn("0" * 40 + " *res/prebuilt/data/other.bin", checks)

    def test_refuses_dswt_changes_to_whole_european_archives(self):
        (self.workspace / ".git/dswt-overlays.json").write_text(json.dumps(["res/field/scripts/scripts_x.s"]))
        with self.assertRaisesRegex(RuntimeError, "per member"):
            prepare_europe.stage(self.workspace, self.rom, self.variant)

    def test_unverified_dump_writes_nothing(self):
        dump = self.workspace.parent / "other.nds"
        dump.write_bytes(self.rom)
        with self.assertRaisesRegex(ValueError, "not the verified"):
            prepare_europe.prepare(dump, self.workspace)
        self.assertFalse((self.workspace / prepare_europe.OUTPUT).exists())


if __name__ == "__main__":
    unittest.main()
