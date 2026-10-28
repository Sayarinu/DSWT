import copy
import importlib.util
from pathlib import Path
import signal
import sys
import tempfile
import unittest


DIRECTORY = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(DIRECTORY))
spec = importlib.util.spec_from_file_location("local_acceptance", DIRECTORY / "acceptance.py")
acceptance = importlib.util.module_from_spec(spec)
spec.loader.exec_module(acceptance)


class SupervisorTests(unittest.TestCase):
    def test_exact_checkpoint_kills_stopped_child_and_missing_marker_fails(self):
        with tempfile.TemporaryDirectory() as temporary:
            log = Path(temporary) / "killed.jsonl"
            script = 'import os,signal; print(\'{"checkpoint":"commit","hit":true}\',flush=True); os.kill(os.getpid(),signal.SIGSTOP)'
            code, events = acceptance.supervise([sys.executable, "-c", script], log, checkpoint="commit", kill=True, timeout=5)
            self.assertEqual(code, -signal.SIGKILL)
            self.assertEqual(events, [{"checkpoint": "commit", "hit": True}])
            with self.assertRaisesRegex(AssertionError, "not reached"):
                acceptance.supervise([sys.executable, "-c", "pass"], Path(temporary) / "missing.jsonl", checkpoint="commit")

    def test_timeout_terminates_child(self):
        with tempfile.TemporaryDirectory() as temporary, self.assertRaises(TimeoutError):
            acceptance.supervise([sys.executable, "-c", "import time; time.sleep(10)"], Path(temporary) / "timeout.jsonl", timeout=0.1)


class OracleTests(unittest.TestCase):
    def test_duplicate_inventory_counter_and_receipt_mutations_fail(self):
        state = {"phase": "idle", "pending_sha256": "", "inventory": [["a", "a"], ["a", "a"]], "next_counter": 3, "event_cursor": 4}
        snapshot = {"identity": [{"next_counter": "3", "event_highwater": "4"}],
                    "operations": [{"opcode": 2, "result": 1, "counter": "1"}, {"opcode": 6, "result": 1, "counter": "2"}],
                    "offers": [{"state": 4}], "receipts": [{"location": 3, "delivered_sha256": "a", "applied_sha256": "a"}]}
        runner = acceptance.Acceptance.__new__(acceptance.Acceptance)
        runner.directory = Path("/case")
        runner.evidence = []
        runner.inspect = lambda _: state
        runner.snapshot = lambda _: snapshot
        runner.settled(Path("/case/a"), ["a", "a"], [(2, 1), (6, 1)])
        with self.assertRaisesRegex(AssertionError, "multiset"):
            runner.settled(Path("/case/a"), ["a"], [(2, 1), (6, 1)])
        for change in (lambda value: value["identity"][0].update(next_counter="4"),
                       lambda value: value["receipts"][0].update(location=2),
                       lambda value: value["receipts"][0].update(applied_sha256="b")):
            changed = copy.deepcopy(snapshot)
            change(changed)
            runner.snapshot = lambda _: changed
            with self.assertRaises(AssertionError):
                runner.settled(Path("/case/a"), ["a", "a"], [(2, 1), (6, 1)])


if __name__ == "__main__":
    unittest.main()
