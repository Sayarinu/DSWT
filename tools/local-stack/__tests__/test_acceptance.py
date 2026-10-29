import copy
import importlib.util
from pathlib import Path
import signal
import sys
import tempfile
import unittest
from unittest.mock import patch


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
    def test_transient_validation_retry_preserves_packet_and_authoritative_history(self):
        runner = acceptance.Acceptance.__new__(acceptance.Acceptance)
        original = {"phase": "submit_ready", "pending_sha256": "packet", "request_sha256": "request", "inventory": [[], []]}
        unavailable = (copy.deepcopy(original), [{"wire_result": 11}])
        queued = ({"phase": "queued", "pending_sha256": ""}, [])
        replies = iter([unavailable, queued])
        history = {"operations": []}
        runner.inspect = lambda _: original
        runner.snapshot = lambda _: copy.deepcopy(history)
        def run(*args, **kwargs):
            state, events = next(replies)
            if state["phase"] == "queued":
                history["operations"].append({"request_sha256": "request"})
            return state, events
        runner.run = run
        with patch.object(acceptance.time, "sleep"):
            runner.finish_submission(Path("/client"))
        for changed in ({"pending_sha256": "different"}, {"inventory": [["unexpected"], []]}):
            history["operations"] = []
            state = dict(original, **changed)
            runner.run = lambda *args, **kwargs: (state, [{"wire_result": 11}])
            with self.subTest(changed=changed), self.assertRaisesRegex(AssertionError, "changed pending"):
                runner.finish_submission(Path("/client"))
        runner.run = lambda *args, **kwargs: unavailable
        with patch.object(acceptance.time, "monotonic", side_effect=[0, 31]), self.assertRaises(TimeoutError):
            runner.finish_submission(Path("/client"))

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
