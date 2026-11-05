from pathlib import Path
import subprocess
import sys
from types import SimpleNamespace
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import recovery
import stack


SAVE = "11111111-1111-4111-8111-111111111111"
DIGEST = "d" * 64


def snapshot(applied=(DIGEST,), **identities):
    return {"tables": {name: {"rows": 1, "sha256": "a" * 64} for name in recovery.TABLES},
            "identities": identities or {SAVE: [3, 4]}, "applied": {SAVE: list(applied)}}


def case(**checks):
    return {"checks": dict({"promoted": True, "ledger_replay": "pass", "migrations": "pass",
                            "behind_highwater": [], "differences": []}, **checks)}


class DecisionTests(unittest.TestCase):
    highwater = [{"save_id": SAVE, "next_counter": 3, "event_cursor": 4, "holding": [DIGEST]}]

    def test_only_an_exact_replayed_restore_is_accepted(self):
        self.assertEqual(recovery.decide(case()), "accepted")
        for checks, reason in (({"promoted": False}, "did not reach"), ({"ledger_replay": "fail"}, "ledger_replay"),
                               ({"migrations": "fail"}, "migrations"), ({"behind_highwater": ["x"]}, "behind"),
                               ({"differences": ["offers"]}, "differ")):
            with self.subTest(checks=checks):
                self.assertIn(reason, recovery.decide(case(**checks)))

    def test_client_highwater_rejects_lagging_identities_or_held_payloads(self):
        self.assertEqual(recovery.behind(self.highwater, snapshot()), [])
        self.assertEqual(recovery.behind(self.highwater, snapshot(**{SAVE: [4, 9]})), [])
        for restored in (snapshot(**{SAVE: [2, 4]}), snapshot(**{SAVE: [3, 3]}), snapshot(applied=())):
            with self.subTest(restored=restored):
                self.assertEqual(len(recovery.behind(self.highwater, restored)), 1)
        missing = snapshot()
        missing["identities"] = {}
        self.assertIn("identity missing", recovery.behind(self.highwater, missing)[0])

    def test_a_paused_cluster_must_equal_its_settled_clients(self):
        self.assertTrue(recovery.matches_clients(self.highwater, snapshot()))
        self.assertFalse(recovery.matches_clients(self.highwater, snapshot(**{SAVE: [4, 5]})))

    def test_each_controlled_restore_has_one_expected_reason(self):
        reasons = {"accepted": case(), "missing-wal": case(promoted=False),
                   "behind-highwater": case(behind_highwater=["x"], differences=["offers"])}
        for name, result in reasons.items():
            self.assertEqual(recovery.decide(result), recovery.EXPECTED[name])
        # A behind restore rejected only for differing rows means the independent highwater check failed.
        self.assertNotEqual(recovery.decide(case(differences=["offers"])), recovery.EXPECTED["behind-highwater"])

    def test_any_table_or_checkpoint_difference_is_reported(self):
        reference = snapshot()
        self.assertEqual(recovery.differences(reference, snapshot()), [])
        changed = snapshot()
        changed["tables"]["receipts"] = {"rows": 1, "sha256": "b" * 64}
        self.assertEqual(recovery.differences(reference, changed), ["receipts"])
        self.assertEqual(recovery.differences(reference, snapshot(**{SAVE: [3, 5]})), ["identities"])
        self.assertEqual(recovery.differences(reference, snapshot(applied=())), ["applied"])

    def test_capture_digests_every_retained_table_and_migrations(self):
        queries = []

        def sql(query):
            queries.append(query)
            if "location = 3" in query:
                return '{"%s": ["%s"]}' % (SAVE, DIGEST)
            return '{"%s": ["3", "4"]}' % SAVE if "json_object_agg" in query else "2 " + "c" * 64
        result = recovery.capture(sql)
        self.assertEqual(set(result["tables"]), set(recovery.TABLES) | {"dswt_meta.schema_migrations"})
        self.assertEqual(result["identities"], {SAVE: [3, 4]})
        self.assertEqual(result["applied"], {SAVE: [DIGEST]})
        self.assertTrue(all("bytea_output = 'hex'" in query for query in queries[:-2]))

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
        local = SimpleNamespace(manifest={"project": "dswt-local-" + "0" * 32}, run=run)
        rehearsal = recovery.Rehearsal(local, Path("/workspace/recovery/20261029T000000-abcd1234"))
        return rehearsal, calls

    def test_restore_volumes_are_new_and_owned(self):
        rehearsal, calls = self.rehearsal("")
        name = rehearsal.volume("accepted-data")
        self.assertEqual(name, "dswt-local-" + "0" * 32 + "-recovery-abcd1234-accepted-data")
        self.assertIn(stack.LABEL + "=dswt-local-" + "0" * 32, calls[-1])
        self.assertIn(recovery.RECOVERY_LABEL + "=abcd1234", calls[-1])
        rehearsal.cleanup(True)
        self.assertEqual(calls[-1], ["volume", "rm", name])

    def test_an_existing_volume_is_never_reused(self):
        rehearsal, calls = self.rehearsal("dswt-local-x-data\n")
        with self.assertRaises(AssertionError):
            rehearsal.volume("accepted-data")
        self.assertFalse(any(call[:2] == ["volume", "create"] for call in calls))

    def test_backup_reads_the_active_cluster_only_through_its_read_only_socket(self):
        rehearsal, calls = self.rehearsal("")
        rehearsal.live = lambda query: ""
        rehearsal.backup("owned-backup", "owned-archive")
        rehearsal.fetch_wal("owned-archive", "0/9000000")
        active = rehearsal.local.manifest["project"] + "-"
        for call in calls:
            mounts = [call[index + 1] for index, item in enumerate(call) if item == "--mount"]
            self.assertEqual(call[call.index("--network") + 1], "none")
            for mount in mounts:
                if f"source={active}" in mount:
                    self.assertEqual(mount, f"type=volume,source={rehearsal.socket},target=/var/run/postgresql,readonly")


if __name__ == "__main__":
    unittest.main()
