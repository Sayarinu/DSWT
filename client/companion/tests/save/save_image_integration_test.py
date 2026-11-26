"""Independent hash/retail-format checks using only C-generated synthetic data."""

import hashlib
import importlib.util
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


def main():
    save_test, sha_probe = sys.argv[1:]
    root = Path(__file__).resolve().parents[4]
    spec = importlib.util.spec_from_file_location("demo", root / "scripts/verify-demo-trade.py")
    demo = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(demo)
    with tempfile.TemporaryDirectory(prefix="dswt-save-test-") as directory:
        path = Path(directory) / "synthetic.bin"
        path.write_bytes(bytes(524288))
        digest = subprocess.check_output([save_test, "--fixture", str(path)], text=True).strip()
        raw = path.read_bytes()
        for bank in (0, 0x40000):
            body = b"".join(raw[bank + start:bank + start + size - 20] for start, size in demo.BLOCKS)
            assert digest == hashlib.sha256(body).hexdigest()
            party, boxes = demo.inventory(raw, bank)
            assert len(party) == 1 and struct.unpack_from("<H", party[0], 8)[0] == 25
            assert boxes == []
            for block, (start, size) in enumerate(demo.BLOCKS):
                global_counter, block_counter = struct.unpack_from("<II", raw, bank + start + size - 20)
                assert global_counter == 42 + bool(bank)
                assert block_counter == 72 + 10 * block + bool(bank)
                assert raw[start:start + size - 20] == raw[0x40000 + start:0x40000 + start + size - 20]
        print("PASS hashlib: both committed bodies match client Hash ranges; SHA-256=" + digest)
        print("PASS verify-demo-trade.inventory: both retail halves, CRCs, party and all 540 box slots")
        # The full demo verifier requires a completed exchange, committed journal
        # records and emulator publication sidecars. This ticket owns none of those.
        print("SCOPE: demo inventory/body checks only; no trade journal or host sidecars")

        vectors = [b"", b"abc", b"a" * 1000000]
        vectors += [bytes((i * 17 + 9) % 256 for i in range(size))
                    for size in (1, 55, 56, 63, 64, 65, 119, 120, 127, 128, 129, 53016, 74192)]
        for data in vectors:
            path.write_bytes(data)
            actual = subprocess.check_output([sha_probe, str(path)], text=True).strip()
            assert actual == hashlib.sha256(data).hexdigest(), len(data)
        print(f"PASS SHA-256: {len(vectors)} independent hashlib vectors, padding boundaries and one million bytes")


if __name__ == "__main__":
    main()
