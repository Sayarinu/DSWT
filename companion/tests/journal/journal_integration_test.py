"""Cross-check the companion's synthetic save against the existing host tools."""

import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    save_test, journal_test = sys.argv[1:]
    root = Path(__file__).resolve().parents[3]
    provision = load("provision", root / "scripts/provision-save.py")
    demo = load("demo", root / "scripts/verify-demo-trade.py")
    with tempfile.TemporaryDirectory(prefix="dswt-journal-") as directory:
        path = Path(directory) / "synthetic.sav"
        path.write_bytes(bytes(524288))
        subprocess.run([save_test, "--fixture", str(path)], check=True, capture_output=True)
        for blank_value in (0, 255):
            blank = bytearray(path.read_bytes())
            for bank in (0x30000, 0x70000):
                blank[bank:bank + 65536] = bytes([blank_value]) * 65536
            path.write_bytes(blank)
            before = path.read_bytes()
            provision.check_retail(before, provision.layout_for("us-rev0"), "us-rev0")
            subprocess.run([journal_test, "--provision", str(path)], check=True)
            after = path.read_bytes()
            expected = bytearray(before)
            for bank in (0x30000, 0x70000):
                record = provision.seed_record(bytes([7]) * 32, 1)
                assert after[bank:bank + 4096] == record
                expected[bank:bank + 4096] = record
            assert after == expected, "Provisioning touched bytes outside the two records"
            provision.check_retail(after, provision.layout_for("us-rev0"), "us-rev0")
            # The provisioner has no provisioned-save verification CLI. Its retail
            # verifier and seed_record are its format oracle; demo parses EMPTY
            # journals only in baseline mode, before enrollment/trading.
            assert demo.journal(after, baseline=True) is None
            try:
                provision.provision(after, "us-rev0")
            except ValueError as error:
                assert "Reserved bank is not blank" in str(error)
            else:
                raise AssertionError("Host tool replaced a companion seed")
            print(f"PASS {blank_value:#04x}-blank provision-save.py: byte-identical generation-1 seed_record, valid retail CRCs, reprovision refusal")
            print("PASS verify-demo-trade.py: companion EMPTY journal accepted by baseline parser")
            print("PASS synthetic save: every byte outside both 4096-byte records preserved")


if __name__ == "__main__":
    main()
