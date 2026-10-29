"""Exercise the migration/replay tool and schema against a private PostgreSQL cluster."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import http.client
import os
from pathlib import Path
import shutil
import struct
import socket
import subprocess
import sys
import tempfile
import time
import unittest
import uuid


def identifier(value):
    return str(uuid.UUID(int=value))


def literal(value):
    return "'" + value.replace("'", "''") + "'"


def blob(value):
    return "decode(" + literal(value.hex()) + ", 'hex')"


class LedgerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="platinumwt-ledger-", dir="/tmp")
        cls.root = Path(cls.directory.name)
        cls.data = cls.root / "data"
        cls.pg_bin = Path(subprocess.check_output(["pg_config", "--bindir"], text=True).strip())
        cls.env = {key: value for key, value in os.environ.items()
                   if not key.startswith("PG") and key != "PLATINUMWT_DATABASE_URL"}
        cls.env.update(PGHOST=str(cls.root), PGPORT="55432", PGUSER="ledger_owner", PGCONNECT_TIMEOUT="3",
                       PGOPTIONS="-c statement_timeout=5000 -c lock_timeout=3000")
        subprocess.run([str(cls.pg_bin / "initdb"), "-D", str(cls.data), "-A", "trust",
                        "-U", "ledger_owner", "--no-locale", "--encoding=UTF8"],
                       check=True, capture_output=True, env=cls.env)
        cls.start()
        cls.serial = 0

    @classmethod
    def start(cls, *, durable=True):
        fsync = "on" if durable else "off"
        subprocess.run([str(cls.pg_bin / "pg_ctl"), "-D", str(cls.data), "-l", str(cls.root / "postgres.log"),
                        "-o", f"-k {cls.root} -p 55432 -c listen_addresses='' -c shared_buffers=16MB "
                              f"-c max_connections=16 -c fsync={fsync} -c full_page_writes=on", "-w", "start"],
                       check=True, capture_output=True, env=cls.env)

    @classmethod
    def stop(cls):
        subprocess.run([str(cls.pg_bin / "pg_ctl"), "-D", str(cls.data), "-m", "immediate", "-w", "stop"],
                       check=True, capture_output=True, env=cls.env)

    @classmethod
    def tearDownClass(cls):
        cls.stop()
        cls.directory.cleanup()

    def setUp(self):
        type(self).serial += 1
        self.database = f"case_{self.serial}"
        self.sql(f"CREATE DATABASE {self.database}", database="postgres")
        self.tool("migrate", str(self.migrations))

    def sql(self, text, *, ok=True, database=None):
        result = subprocess.run(["psql", "-X", "-A", "-t", "-q", "-v", "ON_ERROR_STOP=1",
                                 "--dbname", database or self.database], input=text,
                                capture_output=True, text=True, env=self.env)
        if ok:
            self.assertEqual(result.returncode, 0, result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, "Unexpected successful database command")
        return result.stdout.strip()

    def tool(self, *arguments, ok=True):
        env = dict(self.env, PGDATABASE=self.database)
        result = subprocess.run([str(self.binary), *arguments], env=env, capture_output=True, text=True)
        if ok:
            self.assertEqual(result.returncode, 0, result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, "Unexpected successful ledger command")
        return result.stdout + result.stderr

    def enroll(self, owner=1):
        self.sql(f"""BEGIN;
            INSERT INTO platinumwt.save_identities(save_id,enrollment_id,credential_verifier)
                VALUES ('{identifier(owner)}','{identifier(owner + 100)}',{blob(bytes([owner]) * 32)});
            INSERT INTO platinumwt.trade_events(save_id,sequence,kind,state)
                VALUES ('{identifier(owner)}',1,1,0);
            COMMIT;""")

    def submit_sql(self, owner=1, *, trade=None, asset=None, operation=None, sequence=2, counter=1):
        trade = trade or owner + 1000
        asset = asset or owner + 2000
        operation = operation or owner + 3000
        payload = bytes(136)
        digest = hashlib.sha256(payload).digest()
        return f"""
            INSERT INTO platinumwt.payloads VALUES ('{identifier(asset)}',{blob(payload)},{blob(digest)});
            INSERT INTO platinumwt.offers(trade_id,save_id,offered_asset_id,state,receipt_id,last_event) VALUES
                ('{identifier(trade)}','{identifier(owner)}','{identifier(asset)}',1,NULL,{sequence});
            INSERT INTO platinumwt.ownership VALUES
                ('{identifier(asset)}','{identifier(owner)}','{identifier(trade)}',1,{sequence});
            INSERT INTO platinumwt.operations VALUES ('{identifier(owner)}','{identifier(operation)}',
                {counter},2,{blob(bytes([3]) * 32)},1,{sequence},{blob(bytes(146))});
            INSERT INTO platinumwt.trade_events(save_id,sequence,kind,trade_id,operation_id,
                asset_id,payload_sha256,state) VALUES ('{identifier(owner)}',{sequence},2,
                '{identifier(trade)}','{identifier(operation)}','{identifier(asset)}',{blob(digest)},1);
        """

    def submit(self, owner=1):
        self.sql("BEGIN;" + self.submit_sql(owner) + "COMMIT;")

    def cancel(self, owner=1):
        trade, asset, operation, receipt = [identifier(owner + delta) for delta in (1000, 2000, 4000, 5000)]
        self.sql(f"""BEGIN;
            INSERT INTO platinumwt.receipts VALUES ('{receipt}','{trade}','{identifier(owner)}','{asset}',2,NULL);
            UPDATE platinumwt.offers SET state=3,receipt_id='{receipt}',last_event=3 WHERE trade_id='{trade}';
            UPDATE platinumwt.ownership SET location=2,last_event=3 WHERE asset_id='{asset}';
            INSERT INTO platinumwt.operations VALUES ('{identifier(owner)}','{operation}',2,4,
                {blob(bytes([4]) * 32)},1,3,{blob(bytes(146))});
            INSERT INTO platinumwt.trade_events(save_id,sequence,kind,trade_id,operation_id,asset_id,
                payload_sha256,receipt_id,state) VALUES ('{identifier(owner)}',3,4,'{trade}','{operation}',
                '{asset}',{blob(hashlib.sha256(bytes(136)).digest())},'{receipt}',3);
            COMMIT;""")

    def acknowledge(self, owner=1):
        trade, operation, receipt = [identifier(owner + delta) for delta in (1000, 6000, 5000)]
        asset = self.sql(f"SELECT asset_id FROM platinumwt.receipts WHERE receipt_id='{receipt}'")
        counter, sequence = self.sql("SELECT next_counter||':'||(event_highwater+1) FROM platinumwt.save_identities "
                                     f"WHERE save_id='{identifier(owner)}'").split(':')
        self.sql(f"""BEGIN;
            UPDATE platinumwt.offers SET state=4,last_event={sequence} WHERE trade_id='{trade}';
            UPDATE platinumwt.ownership SET location=3,last_event={sequence} WHERE asset_id='{asset}';
            INSERT INTO platinumwt.operations VALUES ('{identifier(owner)}','{operation}',{counter},6,
                {blob(bytes([6]) * 32)},1,{sequence},{blob(bytes(146))});
            INSERT INTO platinumwt.acknowledgements VALUES ('{receipt}','{identifier(owner)}','{operation}',
                {blob(hashlib.sha256(bytes(136)).digest())},18446744073709551615);
            INSERT INTO platinumwt.trade_events(save_id,sequence,kind,trade_id,operation_id,asset_id,
                payload_sha256,receipt_id,state) VALUES ('{identifier(owner)}',{sequence},5,'{trade}','{operation}',
                '{asset}',{blob(hashlib.sha256(bytes(136)).digest())},'{receipt}',4);
            COMMIT;""")

    def match_sql(self, sequences=None, *, record_attempt=True):
        match = identifier(7001)
        text = f"INSERT INTO platinumwt.matches VALUES ('{match}','{identifier(8001)}'," \
               f"'{identifier(1001)}','{identifier(1002)}');"
        if record_attempt:
            text += f"INSERT INTO platinumwt.matcher_attempts VALUES ('{identifier(8001)}','{match}');"
        for owner in (1, 2):
            sequence = (sequences or {}).get(owner, 3)
            peer = 3 - owner
            trade, receipt = identifier(owner + 1000), identifier(owner + 5000)
            asset = identifier(peer + 2000)
            text += f"""
                INSERT INTO platinumwt.receipts VALUES ('{receipt}','{trade}','{identifier(owner)}',
                    '{asset}',1,'{match}');
                UPDATE platinumwt.offers SET state=2,receipt_id='{receipt}',last_event={sequence} WHERE trade_id='{trade}';
                UPDATE platinumwt.ownership SET save_id='{identifier(owner)}',trade_id='{trade}',location=2,
                    last_event={sequence} WHERE asset_id='{asset}';
                INSERT INTO platinumwt.trade_events(save_id,sequence,kind,trade_id,asset_id,payload_sha256,
                    receipt_id,state) VALUES ('{identifier(owner)}',{sequence},3,'{trade}','{asset}',
                    {blob(hashlib.sha256(bytes(136)).digest())},'{receipt}',2);
            """
        return text

    def rejection_sql(self, owner=1, sequence=2, counter=1, operation=None):
        operation = operation or owner + 3000
        return f"""
            INSERT INTO platinumwt.operations VALUES ('{identifier(owner)}','{identifier(operation)}',{counter},2,
                {blob(bytes([3]) * 32)},8,{sequence},{blob(b'')});
            INSERT INTO platinumwt.trade_events(save_id,sequence,kind,operation_id,payload_sha256,state)
                VALUES ('{identifier(owner)}',{sequence},6,'{identifier(operation)}',
                    {blob(hashlib.sha256(bytes(136)).digest())},0);
        """

    def test_empty_and_existing_migrations_and_replay(self):
        self.assertIn("events: 0", self.tool("verify"))
        self.enroll()
        self.submit()
        before = self.tool("verify")
        self.tool("migrate", str(self.migrations))
        self.assertEqual(self.tool("verify"), before)
        self.assertEqual(self.sql("SELECT count(*) FROM platinumwt.payloads"), "1")

    def test_forward_migration_is_atomic_and_checksummed(self):
        self.enroll()
        copied = self.root / self.database
        shutil.copytree(self.migrations, copied)
        installed_count = len(list(self.migrations.glob("*.sql")))
        upgrade = copied / f"{installed_count + 1:04d}_upgrade.sql"
        upgrade.write_text("CREATE TABLE platinumwt_meta.upgrade_test(id integer); SELECT missing_function();")
        self.tool("migrate", str(copied), ok=False)
        self.assertEqual(self.sql("SELECT to_regclass('platinumwt_meta.upgrade_test') IS NULL"), "t")
        self.assertEqual(self.sql("SELECT count(*) FROM platinumwt_meta.schema_migrations"), str(installed_count))
        upgrade.write_text("CREATE TABLE platinumwt_meta.upgrade_test(id integer);")
        self.tool("migrate", str(copied))
        self.tool("verify")
        upgrade.write_text(upgrade.read_text() + "\n-- changed")
        self.assertIn("changed", self.tool("migrate", str(copied), ok=False))
        self.tool("migrate", str(self.migrations), ok=False)

    def test_matcher_upgrade_retains_existing_matches_and_replay(self):
        previous = self.root / "v1_migrations"
        previous.mkdir()
        shutil.copy(self.migrations / "0001_ledger.sql", previous)
        self.database = f"upgrade_{self.serial}"
        self.sql(f"CREATE DATABASE {self.database}", database="postgres")
        self.tool("migrate", str(previous))
        for owner in (1, 2):
            self.enroll(owner)
            self.submit(owner)
        self.sql("BEGIN;" + self.match_sql(record_attempt=False) + "COMMIT;")
        self.tool("verify")
        self.tool("migrate", str(self.migrations))
        self.assertEqual(self.sql("SELECT operation_id::text||'|'||match_id::text FROM platinumwt.matcher_attempts"),
                         f"{identifier(8001)}|{identifier(7001)}")
        self.tool("verify")

    def test_observation_upgrade_preserves_old_images_and_reports_unknown_time(self):
        previous = self.root / "v2_observation_migrations"
        previous.mkdir()
        for name in ("0001_ledger.sql", "0002_matcher.sql"):
            shutil.copy(self.migrations / name, previous)
        self.database = f"observed_{self.serial}"
        self.sql(f"CREATE DATABASE {self.database}", database="postgres")
        self.tool("migrate", str(previous))
        self.enroll(); self.submit()
        images = self.sql("SELECT md5(string_agg(after_image::text,'|' ORDER BY sequence)) FROM platinumwt.audit_journal")
        self.tool("migrate", str(self.migrations))
        self.assertEqual(self.sql("SELECT count(*) FROM platinumwt.audit_journal WHERE recorded_at IS NOT NULL"), "0")
        self.assertEqual(self.sql("SELECT md5(string_agg(after_image::text,'|' ORDER BY sequence)) FROM platinumwt.audit_journal"), images)
        self.tool("verify")
        self.cancel()
        self.assertEqual(self.sql("SELECT recorded_at IS NOT NULL FROM platinumwt.audit_journal WHERE table_name='offers' AND after_image->>'state'='3'"), "t")
        self.tool("verify")

    def test_private_metrics_observe_real_ledger_counts_without_changing_evidence(self):
        self.enroll(); self.submit()
        before = self.tool("verify")
        with socket.socket() as reserve:
            reserve.bind(("127.0.0.1", 0))
            port = reserve.getsockname()[1]
        env = dict(self.env, PGDATABASE=self.database, WT_BACKEND_ADDRESS="127.0.0.1:0", WT_METRICS_PORT=str(port),
                   WT_LEGALITY_POLICY="synthetic-policy", PLATINUMWT_DATABASE_URL=f"host={self.root} port=55432 user=ledger_owner dbname={self.database}")
        log = (self.root / f"metrics-{self.serial}.log").open("wb")
        process = subprocess.Popen([str(self.backend_binary)], env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 10
            while True:
                try:
                    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=2)
                    try:
                        connection.request("GET", "/metrics")
                        response = connection.getresponse()
                        text = response.read().decode()
                    finally:
                        connection.close()
                    if response.status == 200 and "platinumwt_database_up 1\n" in text:
                        break
                except OSError:
                    pass
                if time.monotonic() >= deadline:
                    self.fail("private ledger metrics did not become ready")
                time.sleep(0.05)
            self.assertIn("platinumwt_queued_offers 1\n", text)
            self.assertIn("platinumwt_pending_deliveries 0\n", text)
            self.assertIn("platinumwt_unknown_queue_ages 0\n", text)
            self.assertNotIn("platinumwt_oldest_queue_age_seconds NaN", text)
            self.assertNotIn(identifier(1), text)
            self.assertEqual(self.tool("verify"), before)
        finally:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill(); process.wait(timeout=5)
                self.fail("metrics sampler blocked shutdown")
            log.close()

    def test_matcher_attempts_are_immutable_and_replayed(self):
        operation = identifier(9001)
        self.sql(f"INSERT INTO platinumwt.matcher_attempts VALUES ('{operation}',NULL)")
        self.tool("verify")
        for statement in (f"DELETE FROM platinumwt.matcher_attempts WHERE operation_id='{operation}'",
                          "TRUNCATE platinumwt.matcher_attempts",
                          f"UPDATE platinumwt.matcher_attempts SET match_id=NULL WHERE operation_id='{operation}'",
                          f"INSERT INTO platinumwt.matcher_attempts VALUES ('{identifier(9002)}','{identifier(7001)}')"):
            self.sql(statement, ok=False)
        self.sql("BEGIN; SET LOCAL session_replication_role=replica; "
                 f"DELETE FROM platinumwt.matcher_attempts WHERE operation_id='{operation}'; COMMIT;")
        self.assertIn("23514", self.tool("verify", ok=False))

    def test_pair_requires_a_matching_attempt_in_writes_and_replay(self):
        for owner in (1, 2):
            self.enroll(owner)
            self.submit(owner)
        self.sql("BEGIN;" + self.match_sql(record_attempt=False) + "COMMIT;", ok=False)
        self.assertEqual(self.sql("SELECT count(*) FROM platinumwt.matches"), "0")
        self.sql(f"INSERT INTO platinumwt.matcher_attempts VALUES ('{identifier(8001)}',NULL)")
        self.sql("BEGIN;" + self.match_sql(record_attempt=False) + "COMMIT;", ok=False)
        self.tool("verify")
        # Start from a valid pair, then corrupt both projection and audit so
        # replay's graph checks, rather than only image comparison, must reject it.
        self.sql("BEGIN; SET LOCAL session_replication_role=replica; "
                 "DELETE FROM platinumwt.matcher_attempts; "
                 "DELETE FROM platinumwt.audit_journal WHERE table_name='matcher_attempts'; COMMIT;")
        self.sql("BEGIN;" + self.match_sql() + "COMMIT;")
        self.tool("verify")
        self.sql("BEGIN; SET LOCAL session_replication_role=replica; "
                 "DELETE FROM platinumwt.matcher_attempts; "
                 "DELETE FROM platinumwt.audit_journal WHERE table_name='matcher_attempts'; COMMIT;")
        self.assertIn("23514", self.tool("verify", ok=False))
        self.sql("INSERT INTO platinumwt.matcher_attempts SELECT operation_id,match_id FROM platinumwt.matches")
        self.tool("verify")
        self.sql("BEGIN; SET LOCAL session_replication_role=replica; "
                 "UPDATE platinumwt.matcher_attempts SET match_id=NULL; "
                 "UPDATE platinumwt.audit_journal SET after_image=jsonb_set(after_image,'{match_id}','null'::jsonb) "
                 "WHERE table_name='matcher_attempts'; COMMIT;")
        self.assertIn("23514", self.tool("verify", ok=False))

    def test_failed_transition_rolls_back_payload_counter_and_journal(self):
        self.enroll()
        before = self.tool("verify")
        self.sql("BEGIN;" + self.submit_sql() + "SELECT 1/0; COMMIT;", ok=False)
        self.assertEqual(self.sql("SELECT count(*) FROM platinumwt.payloads"), "0")
        self.assertEqual(self.sql("SELECT next_counter||':'||event_highwater FROM platinumwt.save_identities"), "1:1")
        self.assertEqual(self.tool("verify"), before)

    def test_pending_offer_and_operation_uniqueness(self):
        self.enroll()
        self.submit()
        before = self.tool("verify")
        self.sql("BEGIN;" + self.submit_sql(trade=11001, asset=12001, operation=13001,
                                         sequence=3, counter=2) + "COMMIT;", ok=False)
        self.sql(f"INSERT INTO platinumwt.operations SELECT * FROM platinumwt.operations", ok=False)
        self.assertEqual(self.tool("verify"), before)

    def test_payload_hash_is_not_a_global_pokemon_identity(self):
        for owner in (1, 2):
            self.enroll(owner)
            self.submit(owner)
        self.assertEqual(self.sql("SELECT count(DISTINCT sha256)||':'||count(*) FROM platinumwt.payloads"), "1:2")
        self.assertEqual(self.sql("SELECT count(*) FROM platinumwt.ownership"), "2")
        self.tool("verify")

    def test_match_commits_both_owners_and_receipts(self):
        for owner in (1, 2):
            self.enroll(owner)
            self.submit(owner)
        before = self.tool("verify")
        partial = f"INSERT INTO platinumwt.matches VALUES ('{identifier(7001)}','{identifier(8001)}'," \
                  f"'{identifier(1001)}','{identifier(1002)}');"
        self.sql("BEGIN;" + partial + "COMMIT;", ok=False)
        self.assertEqual(self.tool("verify"), before)
        self.sql("BEGIN;" + self.match_sql() + "COMMIT;")
        self.assertEqual(self.sql("SELECT count(*) FROM platinumwt.receipts"), "2")
        self.assertEqual(self.sql("SELECT asset_id FROM platinumwt.ownership WHERE save_id='" + identifier(1) + "'"),
                         identifier(2002))
        self.assertEqual(self.sql("SELECT asset_id FROM platinumwt.ownership WHERE save_id='" + identifier(2) + "'"),
                         identifier(2001))
        for owner in (1, 2):
            self.acknowledge(owner)
        self.assertEqual(self.sql("SELECT count(*) FROM platinumwt.acknowledgements"), "2")
        self.tool("verify")

    def test_match_and_acknowledge_with_unequal_owner_histories(self):
        self.enroll(1)
        for sequence in range(2, 5):
            self.sql("BEGIN;" + self.rejection_sql(1, sequence, sequence - 1, 10000 + sequence) + "COMMIT;")
        self.sql("BEGIN;" + self.submit_sql(1, sequence=5, counter=4) + "COMMIT;")
        self.enroll(2)
        self.submit(2)
        self.sql("BEGIN;" + self.match_sql({1: 6, 2: 3}) + "COMMIT;")
        for owner in (1, 2):
            self.acknowledge(owner)
        self.assertEqual(self.sql("SELECT count(*) FROM platinumwt.acknowledgements"), "2")
        self.tool("verify")

    def test_foreign_receipt_cannot_be_acknowledged(self):
        for owner in (1, 2):
            self.enroll(owner)
            self.submit(owner)
        self.cancel(1)
        self.sql(f"INSERT INTO platinumwt.acknowledgements VALUES ('{identifier(5001)}','{identifier(2)}',"
                 f"'{identifier(3002)}',{blob(bytes(32))},1)", ok=False)
        self.assertEqual(self.sql("SELECT count(*) FROM platinumwt.acknowledgements"), "0")
        self.tool("verify")

    def test_concurrent_deposits_have_one_winner(self):
        self.enroll()
        commands = ["BEGIN;" + self.submit_sql(trade=11000 + n, asset=12000 + n, operation=13000 + n) +
                    "COMMIT;" for n in (1, 2)]
        def attempt(command):
            return subprocess.run(["psql", "-X", "-q", "-v", "ON_ERROR_STOP=1", "--dbname", self.database],
                                  input=command, text=True, capture_output=True, env=self.env).returncode
        with ThreadPoolExecutor(max_workers=2) as executor:
            codes = list(executor.map(attempt, commands))
        self.assertEqual(sum(code == 0 for code in codes), 1)
        self.assertEqual(self.sql("SELECT count(*) FROM platinumwt.offers"), "1")
        self.assertEqual(self.sql("SELECT count(*) FROM platinumwt.payloads"), "1")
        self.assertEqual(self.sql("SELECT next_counter FROM platinumwt.save_identities"), "2")
        self.tool("verify")

    def test_cancel_acknowledge_retains_all_evidence(self):
        self.enroll()
        self.submit()
        self.cancel()
        self.acknowledge()
        self.assertEqual(self.sql("SELECT state FROM platinumwt.offers"), "4")
        for table in ("payloads", "ownership", "receipts", "acknowledgements"):
            self.assertEqual(self.sql(f"SELECT count(*) FROM platinumwt.{table}"), "1")
        self.assertEqual(self.sql("SELECT count(*) FROM platinumwt.operations"), "3")
        self.tool("verify")

    def test_no_ownerless_or_extra_unassigned_asset(self):
        self.enroll()
        self.submit()
        self.sql(f"INSERT INTO platinumwt.payloads VALUES ('{identifier(777)}',{blob(bytes(136))},"
                 f"{blob(hashlib.sha256(bytes(136)).digest())})", ok=False)
        self.sql(f"""BEGIN;
            INSERT INTO platinumwt.payloads SELECT '{identifier(778)}',pk4,sha256 FROM platinumwt.payloads;
            INSERT INTO platinumwt.ownership SELECT '{identifier(778)}',save_id,trade_id,location,last_event
                FROM platinumwt.ownership;
            COMMIT;""", ok=False)
        self.assertEqual(self.sql("SELECT count(*) FROM platinumwt.payloads"), "1")
        self.tool("verify")

    def test_no_partial_checkpoint_event_or_ack(self):
        self.enroll()
        self.submit()
        self.sql("UPDATE platinumwt.save_identities SET next_counter=99", ok=False)
        self.sql("UPDATE platinumwt.save_identities SET event_highwater=1", ok=False)
        self.sql(f"INSERT INTO platinumwt.trade_events(save_id,sequence,kind,state) "
                 f"VALUES ('{identifier(1)}',4,1,0)", ok=False)
        self.cancel()
        self.sql("UPDATE platinumwt.offers SET state=4,last_event=3", ok=False)
        self.tool("verify")

    def test_rejected_submission_retains_digest_without_creating_asset(self):
        self.enroll()
        self.sql("BEGIN;" + self.rejection_sql() + "COMMIT;")
        self.assertEqual(self.sql("SELECT count(*) FROM platinumwt.payloads"), "0")
        self.assertEqual(self.sql("SELECT next_counter||':'||event_highwater FROM platinumwt.save_identities"), "2:2")
        self.assertEqual(self.sql("SELECT result FROM platinumwt.operations"), "8")
        self.tool("verify")

    def test_rejected_submission_cannot_create_queued_ownership(self):
        self.enroll()
        text = self.submit_sql()
        success = f"{blob(bytes([3]) * 32)},1,2,{blob(bytes(146))}"
        rejection = f"{blob(bytes([3]) * 32)},8,2,{blob(b'')}"
        self.assertEqual(text.count(success), 1)
        text = text.replace(success, rejection).replace(f"'{identifier(1)}',2,2,", f"'{identifier(1)}',2,6,")
        self.sql("BEGIN;" + text + "COMMIT;", ok=False)
        self.assertEqual(self.sql("SELECT count(*) FROM platinumwt.payloads"), "0")
        self.assertEqual(self.sql("SELECT next_counter FROM platinumwt.save_identities"), "1")
        self.tool("verify")

    def test_replay_detects_ownerless_payload_after_audited_bypass(self):
        payload = bytes(136)
        self.sql("BEGIN; SET LOCAL session_replication_role=replica; "
                 "ALTER TABLE platinumwt.payloads ENABLE ALWAYS TRIGGER audit_rows; "
                 f"INSERT INTO platinumwt.payloads VALUES ('{identifier(777)}',{blob(payload)},"
                 f"{blob(hashlib.sha256(payload).digest())}); "
                 "ALTER TABLE platinumwt.payloads ENABLE TRIGGER audit_rows; COMMIT;")
        self.assertIn("23514", self.tool("verify", ok=False))

    def test_operation_has_exactly_one_outcome_event(self):
        self.enroll()
        self.submit()
        before = self.tool("verify")
        self.sql("INSERT INTO platinumwt.trade_events SELECT save_id,3,version,kind,trade_id,operation_id,"
                 "asset_id,payload_sha256,receipt_id,state FROM platinumwt.trade_events WHERE sequence=2", ok=False)
        self.assertEqual(self.sql("SELECT count(*) FROM platinumwt.trade_events WHERE operation_id IS NOT NULL"), "1")
        self.assertEqual(self.tool("verify"), before)

    def test_checkpoint_gaps_fail_on_write_and_replay(self):
        self.sql(f"INSERT INTO platinumwt.save_identities VALUES ('{identifier(1)}','{identifier(101)}',"
                 f"{blob(bytes(32))},1,5)", ok=False)
        self.enroll()
        self.sql("BEGIN; UPDATE platinumwt.save_identities SET event_highwater=5;" +
                 self.rejection_sql(sequence=6) + "COMMIT;", ok=False)
        self.tool("verify")
        # Bypass only the initial guard to prove the replay fold checks bootstrap state independently.
        self.sql("ALTER TABLE platinumwt.save_identities DISABLE TRIGGER identity_initial")
        try:
            self.sql(f"BEGIN; INSERT INTO platinumwt.save_identities VALUES ('{identifier(2)}',"
                     f"'{identifier(102)}',{blob(bytes(32))},1,5);" + self.rejection_sql(2, 6) + "COMMIT;")
        finally:
            self.sql("ALTER TABLE platinumwt.save_identities ENABLE TRIGGER identity_initial")
        self.assertIn("23514", self.tool("verify", ok=False))

    def test_acknowledged_trade_cannot_regress_on_write_or_replay(self):
        self.enroll()
        self.submit()
        self.cancel()
        self.acknowledge()
        before = self.tool("verify")
        regression = "UPDATE platinumwt.offers SET state=3,last_event=3; " \
                     "UPDATE platinumwt.ownership SET location=2,last_event=3;"
        self.sql("BEGIN;" + regression + "COMMIT;", ok=False)
        self.assertEqual(self.tool("verify"), before)
        # Retain the audit images while simulating a privileged bypass of write guards.
        self.sql("BEGIN; SET LOCAL session_replication_role=replica; "
                 "ALTER TABLE platinumwt.offers ENABLE ALWAYS TRIGGER audit_rows; "
                 "ALTER TABLE platinumwt.ownership ENABLE ALWAYS TRIGGER audit_rows; " + regression +
                 "ALTER TABLE platinumwt.offers ENABLE TRIGGER audit_rows; "
                 "ALTER TABLE platinumwt.ownership ENABLE TRIGGER audit_rows; COMMIT;")
        self.assertIn("23514", self.tool("verify", ok=False))

    def test_immutable_payload_operations_and_retained_journal(self):
        self.enroll()
        self.submit()
        for table in ("save_identities", "payloads", "offers", "ownership", "operations", "trade_events", "audit_journal"):
            self.sql(f"DELETE FROM platinumwt.{table}", ok=False)
            self.sql(f"TRUNCATE platinumwt.{table} CASCADE", ok=False)
        for table in ("payloads", "operations", "trade_events", "audit_journal"):
            self.sql(f"UPDATE platinumwt.{table} SET " +
                     ("version=version" if table in ("trade_events", "audit_journal") else
                      "pk4=pk4" if table == "payloads" else "counter=counter"), ok=False)
        self.tool("verify")

    def test_immediate_restart_preserves_committed_pending_trade(self):
        self.enroll()
        self.submit()
        before = self.tool("verify")
        self.stop()
        self.start()
        self.assertEqual(self.tool("verify"), before)
        self.assertEqual(self.sql("SELECT state FROM platinumwt.offers"), "1")

    def test_replay_detects_tampered_projection_without_repair(self):
        self.enroll()
        self.sql("ALTER TABLE platinumwt.save_identities DISABLE TRIGGER audit_rows; "
                 "ALTER TABLE platinumwt.save_identities DISABLE TRIGGER identity_columns; "
                 f"UPDATE platinumwt.save_identities SET credential_verifier={blob(bytes([7]) * 32)}; "
                 "ALTER TABLE platinumwt.save_identities ENABLE TRIGGER audit_rows; "
                 "ALTER TABLE platinumwt.save_identities ENABLE TRIGGER identity_columns;")
        before = self.sql("SELECT encode(credential_verifier,'hex') FROM platinumwt.save_identities")
        self.assertIn("23514", self.tool("verify", ok=False))
        self.assertEqual(self.sql("SELECT encode(credential_verifier,'hex') FROM platinumwt.save_identities"), before)

    def test_replay_detects_missing_preimage(self):
        self.enroll()
        self.sql("ALTER TABLE platinumwt.audit_journal DISABLE TRIGGER retain_audit; "
                 "DELETE FROM platinumwt.audit_journal WHERE sequence=(SELECT min(sequence) FROM platinumwt.audit_journal); "
                 "ALTER TABLE platinumwt.audit_journal ENABLE TRIGGER retain_audit;")
        self.assertIn("23514", self.tool("verify", ok=False))

    def test_replay_rejects_audited_change_of_immutable_payload(self):
        self.enroll()
        self.submit()
        payload = bytes([1]) + bytes(135)
        # Simulate privileged corruption while retaining its audit event.
        self.sql("BEGIN; SET LOCAL session_replication_role=replica; "
                 "ALTER TABLE platinumwt.payloads ENABLE ALWAYS TRIGGER audit_rows; "
                 f"UPDATE platinumwt.payloads SET pk4={blob(payload)},sha256={blob(hashlib.sha256(payload).digest())}; "
                 "ALTER TABLE platinumwt.payloads ENABLE TRIGGER audit_rows; COMMIT;")
        self.assertEqual(self.sql("SELECT count(*) FROM platinumwt.audit_journal "
                                  "WHERE table_name='payloads' AND before_image IS NOT NULL"), "1")
        self.assertIn("23514", self.tool("verify", ok=False))

    def test_full_u64_and_fractional_values(self):
        self.assertEqual(self.sql("SELECT 18446744073709551615::platinumwt.u64"), "18446744073709551615")
        for value in ("-1", "18446744073709551616", "1.5"):
            with self.subTest(value=value):
                self.sql(f"SELECT ({value})::platinumwt.u64", ok=False)

    def test_cli_errors_do_not_expose_server_diagnostics(self):
        copied = self.root / self.database
        shutil.copytree(self.migrations, copied)
        version = len(list(self.migrations.glob("*.sql"))) + 1
        (copied / f"{version:04d}_secret.sql").write_text("DO $$ BEGIN RAISE EXCEPTION 'PRIVATE_SENTINEL'; END $$;")
        output = self.tool("migrate", str(copied), ok=False)
        self.assertIn("P0001", output)
        self.assertNotIn("PRIVATE_SENTINEL", output)

    def test_unsafe_database_settings_are_rejected(self):
        self.stop()
        self.start(durable=False)
        try:
            self.assertEqual(self.sql("SHOW fsync"), "off")
            self.assertIn("durable database settings", self.tool("verify", ok=False))
        finally:
            self.stop()
            self.start()
            self.assertEqual(self.sql("SHOW fsync"), "on")

    def test_native_identity_contract(self):
        result = subprocess.run([str(self.identity_binary)], env=dict(self.env, PGDATABASE=self.database),
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        rows = self.sql("SELECT replace(save_id::text,'-','')||'|'||encode(credential_verifier,'hex') "
                        "FROM platinumwt.save_identities").splitlines()
        for row in rows:
            save_id, verifier = row.split('|')
            expected = {hashlib.sha256(b'PlatinumWT save credential v1\x00' + bytes.fromhex(save_id) +
                                       bytes([value]) * 32).hexdigest() for value in (0x51, 0x52)}
            self.assertTrue(verifier in expected, "Stored verifier differs from the protocol formula")
        self.tool("verify")

    def test_native_matcher_contract(self):
        environment = dict(self.env, PGDATABASE=self.database)
        result = subprocess.run([str(self.matcher_binary)], env=environment,
                                capture_output=True, text=True, timeout=45)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.tool("verify")
        self.stop()
        self.start()
        result = subprocess.run([str(self.matcher_binary), "--verify-restart"], env=environment,
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.tool("verify")

    def test_https_trade_gateway(self):
        from gateway_scenario import exercise
        exercise(self)
        self.tool("verify")

    def test_native_trade_contract(self):
        environment = dict(self.env, PGDATABASE=self.database)
        result = subprocess.run([str(self.trade_binary)], env=environment, capture_output=True, text=True, timeout=45)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        # Independent v1 wire oracle: credentials are excluded from request
        # identity, and saved outcomes use the documented snapshot field order.
        rows = self.sql("""SELECT replace(op.save_id::text,'-',''),replace(op.operation_id::text,'-',''),
            op.counter,op.opcode,encode(op.request_sha256,'hex'),op.event_sequence,encode(op.outcome,'hex'),
            replace(e.trade_id::text,'-',''),replace(o.offered_asset_id::text,'-',''),encode(p.sha256,'hex'),
            coalesce(replace(r.asset_id::text,'-',''),''),coalesce(encode(q.sha256,'hex'),''),
            coalesce(replace(r.receipt_id::text,'-',''),''),coalesce(r.delivery_kind,0),e.state,
            coalesce(encode(a.applied_sha256,'hex'),''),coalesce(a.applied_generation,0),
            encode(CASE WHEN op.opcode=2 THEN p.pk4 ELSE q.pk4 END,'hex')
            FROM platinumwt.operations op JOIN platinumwt.trade_events e
                ON (op.save_id,op.event_sequence)=(e.save_id,e.sequence)
            JOIN platinumwt.offers o ON o.trade_id=e.trade_id
            JOIN platinumwt.payloads p ON p.asset_id=o.offered_asset_id
            LEFT JOIN platinumwt.receipts r ON r.receipt_id=e.receipt_id
            LEFT JOIN platinumwt.payloads q ON q.asset_id=r.asset_id
            LEFT JOIN platinumwt.acknowledgements a ON a.save_id=op.save_id AND a.operation_id=op.operation_id
            WHERE op.result=1""").splitlines()
        self.assertGreater(len(rows), 0)
        for row in rows:
            (save, operation, counter, opcode, request_hash, sequence, outcome, trade, asset, digest,
             received, received_digest, receipt, kind, state, applied, generation, pk4) = row.split('|')
            opcode, sequence = int(opcode), int(sequence)
            raw = lambda value, size: bytes.fromhex(value) if value else bytes(size)
            # A historical submission has no receipt even if its live offer
            # later matched or returned. Its event joins the immutable payload.
            if opcode == 2:
                body = struct.pack('<Q', 1) + bytes.fromhex(pk4) + bytes.fromhex(digest)
            elif opcode == 4:
                body = bytes.fromhex(trade)
            else:
                body = (bytes.fromhex(trade + receipt + received_digest + applied) +
                        struct.pack('<Q', int(generation)))
            frame = struct.pack('<4sBBH16s16s32sQ', b'WTDS', 1, opcode, len(body),
                                bytes.fromhex(operation), bytes.fromhex(save), bytes([0x51]) * 32, int(counter)) + body
            self.assertEqual(hashlib.sha256(frame[:40] + frame[72:]).hexdigest(), request_hash)
            expected = struct.pack('<Q16sBB16s32s16s32s16sQ', sequence, bytes.fromhex(trade), int(state), int(kind),
                                   bytes.fromhex(asset), bytes.fromhex(digest), raw(received, 16),
                                   raw(received_digest, 32), raw(receipt, 16), sequence)
            self.assertEqual(bytes.fromhex(outcome), expected)
        self.tool("verify")
        self.stop()
        self.start()
        result = subprocess.run([str(self.trade_binary), "--verify-restart"], env=environment,
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.tool("verify")


def main():
    if os.geteuid() == 0:
        os.execvp("runuser", ["runuser", "-u", "postgres", "--", sys.executable, __file__, *sys.argv[1:]])
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("migrations", type=Path)
    parser.add_argument("identity_binary", type=Path)
    parser.add_argument("trade_binary", type=Path)
    parser.add_argument("matcher_binary", type=Path)
    parser.add_argument("server_binary", type=Path)
    parser.add_argument("backend_binary", type=Path)
    parser.add_argument("gateway_fixture", type=Path)
    args = parser.parse_args()
    LedgerTests.binary = args.binary.resolve()
    LedgerTests.migrations = args.migrations.resolve()
    LedgerTests.identity_binary = args.identity_binary.resolve()
    LedgerTests.trade_binary = args.trade_binary.resolve()
    LedgerTests.matcher_binary = args.matcher_binary.resolve()
    LedgerTests.server_binary = args.server_binary.resolve()
    LedgerTests.backend_binary = args.backend_binary.resolve()
    LedgerTests.gateway_fixture = args.gateway_fixture.resolve()
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(LedgerTests))
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
