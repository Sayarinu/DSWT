from pathlib import Path
import subprocess
import sys
from types import SimpleNamespace
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import recovery
import stack


SAVE = "11111111-1111-4111-8111-111111111111"


def snapshot(**identities):
    return {"tables": {name: {"rows": 1, "sha256": "a" * 64} for name in recovery.TABLES},
            "identities": identities or {SAVE: [3, 4]}}


def case(**checks):
    return {"checks": dict({"promoted": True, "ledger_replay": "pass", "migrations": "pass",
                            "behind_highwater": [], "differences": []}, **checks)}


class DecisionTests(unittest.TestCase):
    highwater = [{"save_id": SAVE, "next_counter": 3, "event_cursor": 4}]

    def test_only_an_exact_replayed_restore_is_accepted(self):
        self.assertEqual(recovery.decide(case()), "accepted")
        for checks, reason in (({"promoted": False}, "did not reach"), ({"ledger_replay": "fail"}, "ledger_replay"),
                               ({"migrations": "fail"}, "migrations"), ({"behind_highwater": ["x"]}, "behind"),
                               ({"differences": ["offers"]}, "differ")):
            with self.subTest(checks=checks):
                self.assertIn(reason, recovery.decide(case(**checks)))

    def test_client_highwater_rejects_lagging_or_missing_identities(self):
        self.assertEqual(recovery.behind(self.highwater, {SAVE: [3, 4]}), [])
        self.assertEqual(recovery.behind(self.highwater, {SAVE: [4, 9]}), [])
        for identities in ({SAVE: [2, 4]}, {SAVE: [3, 3]}, {}):
            with self.subTest(identities=identities):
                self.assertEqual(len(recovery.behind(self.highwater, identities)), 1)

    def test_any_table_or_checkpoint_difference_is_reported(self):
        reference = snapshot()
        self.assertEqual(recovery.differences(reference, snapshot()), [])
        changed = snapshot()
        changed["tables"]["receipts"] = {"rows": 1, "sha256": "b" * 64}
        self.assertEqual(recovery.differences(reference, changed), ["receipts"])
        self.assertEqual(recovery.differences(reference, snapshot(**{SAVE: [3, 5]})), ["identities"])

    def test_capture_digests_every_retained_table_and_migrations(self):
        queries = []

        def sql(query):
            queries.append(query)
            return '{"%s": ["3", "4"]}' % SAVE if "json_object_agg" in query else "2 " + "c" * 64
        result = recovery.capture(sql)
        self.assertEqual(set(result["tables"]), set(recovery.TABLES) | {"platinumwt_meta.schema_migrations"})
        self.assertEqual(result["identities"], {SAVE: [3, 4]})
        self.assertTrue(all("bytea_output = 'hex'" in query for query in queries[:-1]))

    def test_only_complete_wal_segment_names_are_listed(self):
        names = ["000000010000000000000003", "000000010000000000000002", "000000010000000000000004.partial",
                 "00000002.history", "archive_status"]
        self.assertEqual(recovery.segments(names), ["000000010000000000000002", "000000010000000000000003"])


class VolumeTests(unittest.TestCase):
    def rehearsal(self, existing):
        calls = []

        def run(arguments, **kwargs):
            calls.append(arguments)
            return subprocess.CompletedProcess(arguments, 0, existing if arguments[:2] == ["volume", "ls"] else "", "")
        local = SimpleNamespace(manifest={"project": "platinumwt-local-" + "0" * 32}, run=run)
        rehearsal = recovery.Rehearsal(local, Path("/workspace/recovery/20261029T000000-abcd1234"))
        return rehearsal, calls

    def test_restore_volumes_are_new_and_owned(self):
        rehearsal, calls = self.rehearsal("")
        name = rehearsal.volume("accepted-data")
        self.assertEqual(name, "platinumwt-local-" + "0" * 32 + "-recovery-abcd1234-accepted-data")
        self.assertIn(stack.LABEL + "=platinumwt-local-" + "0" * 32, calls[-1])
        self.assertIn(recovery.RECOVERY_LABEL + "=abcd1234", calls[-1])
        rehearsal.cleanup(True)
        self.assertEqual(calls[-1], ["volume", "rm", name])

    def test_an_existing_volume_is_never_reused(self):
        rehearsal, calls = self.rehearsal("platinumwt-local-x-data\n")
        with self.assertRaises(AssertionError):
            rehearsal.volume("accepted-data")
        self.assertFalse(any(call[:2] == ["volume", "create"] for call in calls))

    def test_the_active_data_volume_is_never_mounted_writable(self):
        rehearsal, calls = self.rehearsal("")
        rehearsal.tool("true", f"type=volume,source={rehearsal.socket},target=/var/run/postgresql,readonly")
        mounts = [calls[-1][index + 1] for index, item in enumerate(calls[-1]) if item == "--mount"]
        self.assertEqual(mounts, [f"type=volume,source={rehearsal.socket},target=/var/run/postgresql,readonly"])
        self.assertNotIn("-data", rehearsal.socket)
        self.assertIn("none", calls[-1][calls[-1].index("--network") + 1])


if __name__ == "__main__":
    unittest.main()
