#!/usr/bin/env python3
"""Run the native C trade client against the isolated, strict local service stack."""

import argparse
from collections import Counter
from contextlib import contextmanager
import fcntl
import hashlib
import json
import os
from pathlib import Path
import platform
import selectors
import shutil
import signal
import subprocess
import sys
import time
import uuid

import stack


CASES = ("repeat", "cancel", "storage", "reply-loss", "interruption", "older-copy", "outages", "validator-recovery", "illegal")


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def save_json(path, value):
    temporary = path.with_suffix(".new")
    with temporary.open("x") as output:
        json.dump(value, output, indent=2)
        output.write("\n")
        output.flush()
        os.fsync(output.fileno())
    temporary.replace(path)


def supervise(arguments, log, *, checkpoint=None, kill=False, timeout=90):
    """Read the marker before killing the stopped process; never race a timer."""
    events, hit = [], False
    process = subprocess.Popen(arguments, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0)
    pending = b""
    selector = selectors.DefaultSelector()
    selector.register(process.stdout, selectors.EVENT_READ)
    deadline = time.monotonic() + timeout
    try:
        with log.open("xb") as output:
            while selector.get_map():
                if time.monotonic() >= deadline:
                    raise TimeoutError("native client deadline; see " + str(log))
                for key, _ in selector.select(min(1, max(0, deadline - time.monotonic()))):
                    data = os.read(key.fileobj.fileno(), 65536)
                    if not data:
                        selector.unregister(key.fileobj)
                        continue
                    output.write(data)
                    pending += data
                    while b"\n" in pending:
                        line, pending = pending.split(b"\n", 1)
                        event = json.loads(line)
                        events.append(event)
                        if event.get("hit"):
                            require(event.get("checkpoint") == checkpoint, "wrong fault checkpoint")
                            hit = True
                            if kill:
                                process.kill()
            code = process.wait(timeout=5)
        if checkpoint:
            require(hit, "checkpoint was not reached: " + checkpoint)
        if kill:
            require(code == -signal.SIGKILL, "supervisor did not kill the client")
        return code, events
    finally:
        selector.close()
        if process.poll() is None:
            process.kill()
        process.wait(timeout=5)
        process.stdout.close()


class Acceptance:
    def __init__(self, local, directory):
        self.local, self.directory = local, directory
        self.profile = json.loads((local.workspace / local.manifest["profile"]).read_text())
        require(self.profile["hostname"] == "localhost" and self.profile["port"] == stack.https_port(local.manifest),
                "unexpected local profile")
        self.probe = local.workspace / "native-build/trade_probe"
        require(not self.probe.parent.is_symlink(), "native build directory must not be linked")
        self.fixtures = directory / "fixtures"
        self.serial = 0
        self.evidence = []
        self.case_directory = directory

    def sql(self, query):
        result = self.local.compose("exec", "-T", "database", "psql", "-XAt", "-v", "ON_ERROR_STOP=1",
                                    "-U", "platinumwt_local", "-d", "platinumwt_e2e", "-c", query,
                                    capture=True, timeout=30)
        return result.stdout.strip()

    def rows(self, query):
        return json.loads(self.sql("SELECT coalesce(json_agg(row_to_json(item)), '[]'::json) FROM (" + query + ") item"))

    def build(self):
        self.local.volumes()
        self.local.database_identity()
        self.local.ready()
        require(self.sql("SELECT count(*) FROM platinumwt.offers WHERE state <> 4") == "0",
                "retained pending offers exist; recover their original clients before starting another run")
        require(not (self.local.workspace / "control/matcher-hold").exists(), "matcher is already held by another test")
        with (self.directory / "native-build.log").open("x") as log:
            source = subprocess.run([sys.executable, str(stack.ROOT / "scripts/prepare-tls.py")],
                                    check=True, text=True, stdout=subprocess.PIPE, stderr=log, timeout=120).stdout.strip()
            for args in (["cmake", "-S", str(stack.ROOT / "client/tests"), "-B", str(self.probe.parent), "-DMBEDTLS_SOURCE=" + source],
                         ["cmake", "--build", str(self.probe.parent), "--target", "trade_probe", "--parallel", "2"]):
                subprocess.run(args, check=True, stdout=log, stderr=subprocess.STDOUT, timeout=300)
        self.fixtures.mkdir(mode=0o700)
        image = self.local.manifest["project"] + "-fixtures"
        with (self.directory / "fixtures-build.log").open("x") as log:
            subprocess.run(self.local.docker + ["build", "--platform", self.local.env["PWT_LOCAL_PLATFORM"], "--target", "validator-tests",
                            "--file", str(stack.ROOT / "validator/Dockerfile"), "--tag", image, str(stack.ROOT)],
                           env=self.local.env, check=True, stdout=log, stderr=subprocess.STDOUT, timeout=900)
            subprocess.run(self.local.docker + ["run", "--rm", "--network", "none", "--read-only", "--tmpfs", "/tmp",
                            "--user", str(os.getuid()) + ":" + str(os.getgid()), "--memory", "512m", "--cpus", "2",
                            "--mount", "type=bind,source=" + str(self.fixtures) + ",target=/fixtures",
                            image, "--export-fixtures", "/fixtures"], env=self.local.env, check=True,
                           stdout=log, stderr=subprocess.STDOUT, timeout=60)
        manifest = json.loads((self.fixtures / "manifest.json").read_text())
        for name, digest in manifest["sha256"].items():
            require(sha(self.fixtures / name) == digest, "fixture exporter digest mismatch")
        self.fixture_manifest = manifest
        self.fixture_image = self.local.run(["image", "inspect", "--format", "{{.Id}}", image], capture=True).stdout.strip()

    def client(self, name, fixture="starter.pk4"):
        path = self.case_directory / name
        subprocess.run([str(self.probe), "init", str(path), str(self.fixtures / fixture)], check=True, timeout=10)
        return path

    def inspect(self, client):
        output = subprocess.run([str(self.probe), "inspect", str(client)], check=True, capture_output=True, text=True, timeout=10)
        return json.loads(output.stdout)

    def run(self, client, action="reconcile", *, fault=None, checkpoint=None, failure=False, allow_unavailable=False):
        self.serial += 1
        arguments = [str(self.probe), "run", str(client), str(self.profile["port"]), self.profile["hostname"],
                     self.profile["current_spki_sha256"], self.profile["next_spki_sha256"], action]
        if fault:
            arguments += [fault, checkpoint, "1"]
        log = self.case_directory / (str(self.serial) + "-" + client.name + "-" + action + ".jsonl")
        code, events = supervise(arguments, log, checkpoint=checkpoint, kill=fault == "kill")
        require(code in (0, 1) or (fault == "kill" and code == -signal.SIGKILL),
                "Unexpected probe exit (memory/process failure): " + str(code) + "; see " + str(log))
        unavailable = allow_unavailable and code == 1 and any(event.get("wire_result") == 11 for event in events)
        require((code != 0) == (failure or bool(fault)) or unavailable, "unexpected native exit " + str(code) + "; see " + str(log))
        summary = self.inspect(client)
        self.evidence.append({"client": str(client.relative_to(self.directory)), "action": action,
                              "checkpoint": checkpoint, "fault": fault, "exit": code,
                              "log": str(log.relative_to(self.directory)), "state": summary})
        return summary, events

    def snapshot(self, client):
        state = self.inspect(client)
        save = str(uuid.UUID(hex=state["save_id"]))
        require(save != str(uuid.UUID(int=0)), "client is not enrolled")
        where = " WHERE save_id='" + save + "'"
        return {
            "identity": self.rows("SELECT save_id::text,next_counter::text,event_highwater::text FROM platinumwt.save_identities" + where),
            "operations": self.rows("SELECT operation_id::text,counter::text,opcode,result,encode(request_sha256,'hex') AS request_sha256 FROM platinumwt.operations" + where + " ORDER BY counter"),
            "offers": self.rows("SELECT t.trade_id::text,t.offered_asset_id::text,t.state,encode(p.sha256,'hex') AS offered_sha256 FROM platinumwt.offers t JOIN platinumwt.payloads p ON p.asset_id=t.offered_asset_id WHERE t.save_id='" + save + "' ORDER BY t.last_event"),
            "receipts": self.rows("SELECT r.receipt_id::text,r.trade_id::text,r.asset_id::text,r.delivery_kind,o.location,encode(p.sha256,'hex') AS delivered_sha256,encode(a.applied_sha256,'hex') AS applied_sha256 FROM platinumwt.receipts r JOIN platinumwt.ownership o USING(asset_id,save_id,trade_id) JOIN platinumwt.payloads p USING(asset_id) LEFT JOIN platinumwt.acknowledgements a USING(receipt_id,save_id) WHERE r.save_id='" + save + "' ORDER BY r.receipt_id"),
        }

    def settled(self, client, expected, operations):
        state = self.inspect(client)
        snapshot = self.snapshot(client)
        require(state["phase"] == "idle" and not state["pending_sha256"], "client did not settle")
        require(state["inventory"][0] == state["inventory"][1]
                and Counter(state["inventory"][0]) == Counter(expected), "inventory multiset differs from expected ownership")
        require(len(snapshot["identity"]) == 1, "identity missing")
        identity = snapshot["identity"][0]
        require(int(identity["next_counter"]) == state["next_counter"] == len(operations) + 1
                and int(identity["event_highwater"]) == state["event_cursor"], "client/server checkpoint mismatch")
        require([(row["opcode"], row["result"]) for row in snapshot["operations"]] == operations
                and [int(row["counter"]) for row in snapshot["operations"]] == list(range(1, len(operations) + 1)),
                "mutation count/order changed")
        require(all(row["state"] == 4 for row in snapshot["offers"]), "offer is still pending")
        require(len(snapshot["offers"]) == len(snapshot["receipts"]), "offer/receipt cardinality differs")
        require(all(row["location"] == 3 and row["delivered_sha256"] == row["applied_sha256"] for row in snapshot["receipts"]),
                "receipt ownership or applied digest mismatch")
        self.evidence.append({"client": str(client.relative_to(self.directory)), "authoritative": snapshot})
        return snapshot

    def paired(self, first, second):
        a, b = self.snapshot(first), self.snapshot(second)
        for own, peer in ((a, b), (b, a)):
            offered = own["offers"][-1]
            receipt = next(row for row in own["receipts"] if row["trade_id"] == offered["trade_id"])
            require(receipt["asset_id"] == peer["offers"][-1]["offered_asset_id"] and receipt["delivery_kind"] == 1,
                    "match did not transfer the peer's exact asset")

    def wait_matched(self, *clients):
        deadline = time.monotonic() + 15
        while True:
            if all(self.snapshot(client)["offers"][-1]["state"] == 2 for client in clients):
                return
            if time.monotonic() >= deadline:
                raise TimeoutError("matcher did not pair case clients")
            time.sleep(0.05)

    @contextmanager
    def hold(self):
        path = self.local.workspace / "control/matcher-hold"
        with path.open("x"):
            pass
        try:
            yield
        finally:
            path.unlink()

    def repeat(self):
        a, b = self.client("a"), self.client("b", "wild.pk4")
        for round_number in range(2):
            self.run(a, "submit"); self.run(b, "submit")
            self.wait_matched(a, b)
            self.run(a, "collect"); self.run(b, "collect")
            self.paired(a, b)
            for client, name in ((a, "wild.pk4" if round_number == 0 else "starter.pk4"),
                                 (b, "starter.pk4" if round_number == 0 else "wild.pk4")):
                self.settled(client, [self.fixture_manifest["sha256"][name]], [(2, 1), (6, 1)] * (round_number + 1))

    def cancel(self):
        with self.hold():
            a = self.client("a")
            original = (a / "inventory-0.bin").read_bytes()
            self.run(a, "submit"); self.run(a, "cancel")
            state = self.settled(a, [self.fixture_manifest["sha256"]["starter.pk4"]], [(2, 1), (4, 1), (6, 1)])
            require(state["receipts"][0]["delivery_kind"] == 2 and state["receipts"][0]["asset_id"] == state["offers"][0]["offered_asset_id"], "cancel changed asset")
            require((a / "inventory-0.bin").read_bytes() == original, "cancel did not restore exact encrypted image")

    def storage(self):
        a, b = self.client("a"), self.client("b", "wild.pk4")
        self.run(a, "submit"); self.run(b, "submit"); self.wait_matched(a, b)
        (a / "full").touch(mode=0o600)
        state, _ = self.run(a, "collect")
        require(state["phase"] == "received" and state["inventory"] == [[], []], "full storage applied incoming payload")
        before = self.snapshot(a)
        self.run(a, "collect")
        require(self.snapshot(a) == before and len(before["operations"]) == 1 and before["receipts"][0]["location"] == 2,
                "full destination ACKed or changed ownership")
        require(before["receipts"][0]["delivered_sha256"] == state["incoming_sha256"], "retained received digest mismatch")
        (a / "full").unlink()
        self.run(a, "collect"); self.run(b, "collect"); self.paired(a, b)
        self.settled(a, [self.fixture_manifest["sha256"]["wild.pk4"]], [(2, 1), (6, 1)])
        self.settled(b, [self.fixture_manifest["sha256"]["starter.pk4"]], [(2, 1), (6, 1)])

    def assert_retry(self, client, action, opcode):
        before = self.inspect(client)
        database = self.snapshot(client) if before["save_id"] != "0" * 32 else None
        seed_before = (client / "journal.bin").read_bytes()[1444:1476]
        after, _ = self.run(client, action, fault="kill", checkpoint="wire." + str(opcode) + ".send")
        require(before["pending_sha256"] and before["pending_sha256"] == after["pending_sha256"], "retry changed exact pending packet")
        require((client / "journal.bin").read_bytes()[1444:1476] != seed_before, "copied process reused entropy")
        if database:
            require(self.snapshot(client) == database, "pre-send retry changed authoritative state")
            committed = [row for row in database["operations"] if row["operation_id"] == str(uuid.UUID(hex=before["operation_id"]))]
            require(len(committed) == 1 and committed[0]["request_sha256"] == before["request_sha256"], "committed request hash mismatch")

    def reply_loss(self):
        with self.hold():
            a = self.client("a")
            state, events = self.run(a, fault="kill", checkpoint="wire.1.reply")
            require(any(row.get("wire_operation") == 1 and row["wire_result"] == 1 for row in events), "enrollment did not commit")
            enrollment = str(uuid.UUID(hex=state["operation_id"]))
            require(self.sql("SELECT count(*) FROM platinumwt.save_identities WHERE enrollment_id='" + enrollment + "'") == "1", "enrollment missing")
            self.assert_retry(a, "reconcile", 1)
            self.run(a)
            require(self.sql("SELECT save_id::text FROM platinumwt.save_identities WHERE enrollment_id='" + enrollment + "'")
                    == str(uuid.UUID(hex=self.inspect(a)["save_id"])), "enrollment retry created a replacement identity")
            self.run(a, "submit", fault="kill", checkpoint="wire.2.reply")
            self.assert_retry(a, "reconcile", 2)
            self.run(a)
            self.run(a, "cancel", fault="error", checkpoint="wire.4.reply")
            self.assert_retry(a, "cancel", 4)
            self.run(a, "cancel", fault="kill", checkpoint="wire.6.reply")
            before = self.snapshot(a)
            inventory = self.inspect(a)["inventory"]
            self.assert_retry(a, "reconcile", 6)
            self.run(a)
            require(self.snapshot(a) == before and self.inspect(a)["inventory"] == inventory, "ACK replay changed inventory or server history")
            self.settled(a, [self.fixture_manifest["sha256"]["starter.pk4"]], [(2, 1), (4, 1), (6, 1)])

    def interruption(self):
        checkpoints = ("journal.escrow.1.body.after", "journal.escrow.1.commit.after",
                       "journal.escrow.0.body.after", "journal.escrow.0.commit.after", "inventory.remove.0.body.after",
                       "inventory.remove.0.commit.after", "journal.received.0.commit.after", "inventory.receive.0.body.after",
                       "inventory.receive.0.commit.after", "journal.applied.1.commit.after")
        for index, checkpoint in enumerate(checkpoints):
            for fault in ("kill", "error"):
                a, b = self.client(str(index) + fault + "a"), self.client(str(index) + fault + "b", "wild.pk4")
                self.run(a); self.run(b)
                removal = checkpoint.startswith(("journal.escrow", "inventory.remove"))
                self.run(a, "submit", **({"fault": fault, "checkpoint": checkpoint} if removal else {}))
                if removal:
                    self.run(a, "submit")
                self.run(b, "submit"); self.wait_matched(a, b)
                self.run(a, "collect", **({"fault": fault, "checkpoint": checkpoint} if not removal else {}))
                if not removal:
                    self.run(a, "collect")
                self.run(b, "collect"); self.paired(a, b)
                self.settled(a, [self.fixture_manifest["sha256"]["wild.pk4"]], [(2, 1), (6, 1)])
                self.settled(b, [self.fixture_manifest["sha256"]["starter.pk4"]], [(2, 1), (6, 1)])

    def older_copy(self):
        a, b = self.client("a"), self.client("b", "wild.pk4")
        self.run(a)
        old = self.case_directory / "older-enrolled"
        shutil.copytree(a, old)
        self.run(a, "submit"); self.run(b, "submit"); self.wait_matched(a, b)
        copied = self.case_directory / "copied-pending"
        shutil.copytree(a, copied)
        self.run(a, "collect"); self.run(b, "collect"); self.paired(a, b)
        before = self.snapshot(a)
        for client in (old, copied):
            inventory = self.inspect(client)["inventory"]
            state, events = self.run(client, "submit", failure=True)
            require(any(row.get("summary") and row.get("blocked") for row in events), "stale state did not fail closed in the engine")
            require(state["inventory"] == inventory and self.snapshot(a) == before, "stale state authorized a transfer")
        self.settled(a, [self.fixture_manifest["sha256"]["wild.pk4"]], [(2, 1), (6, 1)])
        self.settled(b, [self.fixture_manifest["sha256"]["starter.pk4"]], [(2, 1), (6, 1)])

    def outages(self):
        with self.hold():
            for service in ("validator", "database", "backend"):
                a = self.client(service)
                self.run(a)
                pending, _ = self.run(a, "submit", fault="kill", checkpoint="wire.2.send")
                before = self.snapshot(a)
                self.local.compose("stop", service, capture=True, timeout=45)
                try:
                    state, events = self.run(a, failure=True)
                    require(state["pending_sha256"] == pending["pending_sha256"] and state["inventory"] == [[], []], "outage released escrow")
                    require(any(row.get("wire_result") == 11
                                or (row.get("transport_stage") == "http" and row.get("result", 0) < 0 and row.get("tls_flags") == 0)
                                for row in events), "outage did not fail at the authenticated service boundary")
                finally:
                    self.restart_service(service)
                require(self.snapshot(a) == before, "outage consumed mutation counter")
                self.finish_submission(a); self.run(a, "cancel")
                self.settled(a, [self.fixture_manifest["sha256"]["starter.pk4"]], [(2, 1), (4, 1), (6, 1)])

    def restart_service(self, service):
        self.local.compose("start", service, capture=True, timeout=45)
        deadline = time.monotonic() + 30
        while True:
            try:
                self.local.ready()
                return
            except (OSError, ValueError, subprocess.SubprocessError):
                if time.monotonic() >= deadline:
                    raise
                time.sleep(0.1)

    def finish_submission(self, client):
        original, before = self.inspect(client), self.snapshot(client)
        require(original["phase"] == "submit_ready" and original["pending_sha256"], "Expected an exact pending submission")
        deadline = time.monotonic() + 30
        while True:
            state, events = self.run(client, allow_unavailable=True)
            if state["phase"] == "queued" and not state["pending_sha256"]:
                after = self.snapshot(client)
                require(len(after["operations"]) == len(before["operations"]) + 1
                        and after["operations"][-1]["request_sha256"] == original["request_sha256"], "Retry committed a different submission")
                return
            require(any(event.get("wire_result") == 11 for event in events)
                    and state["pending_sha256"] == original["pending_sha256"] and state["inventory"] == original["inventory"]
                    and self.snapshot(client) == before, "Dependency recovery changed pending ownership/request")
            # A new health-check channel can recover before the backend's channel leaves backoff.
            if time.monotonic() >= deadline:
                raise TimeoutError("Backend validation did not recover; exact submission remains pending")
            time.sleep(0.1)

    def validator_recovery(self):
        a, b, pending = self.client("a"), self.client("b", "wild.pk4"), self.client("pending")
        self.run(pending)
        self.run(a, "submit"); self.run(b, "submit"); self.wait_matched(a, b)
        original, _ = self.run(pending, "submit", fault="kill", checkpoint="wire.2.send")
        before = self.snapshot(pending)
        self.local.compose("stop", "validator", capture=True, timeout=45)
        try:
            require(not self.local.compose("ps", "--status", "running", "-q", "validator", capture=True, timeout=10).stdout.strip(),
                    "validator did not stop")
            state, events = self.run(pending, failure=True)
            require(state["pending_sha256"] == original["pending_sha256"] and state["inventory"] == [[], []],
                    "validator outage changed pending submission or released escrow")
            require(any(event.get("wire_result") == 11 for event in events), "new submission was not retryable")
            require(self.snapshot(pending) == before, "unavailable validation consumed a mutation")
            self.run(a, "collect"); self.run(b, "collect"); self.paired(a, b)
            self.settled(a, [self.fixture_manifest["sha256"]["wild.pk4"]], [(2, 1), (6, 1)])
            self.settled(b, [self.fixture_manifest["sha256"]["starter.pk4"]], [(2, 1), (6, 1)])
            require(not self.local.compose("ps", "--status", "running", "-q", "validator", capture=True, timeout=10).stdout.strip(),
                    "collection unexpectedly restarted validator")
        finally:
            self.restart_service("validator")
        require(self.inspect(pending)["pending_sha256"] == original["pending_sha256"], "restart changed exact retry packet")
        self.finish_submission(pending); self.run(pending, "cancel")
        final = self.settled(pending, [self.fixture_manifest["sha256"]["starter.pk4"]], [(2, 1), (4, 1), (6, 1)])
        require(final["operations"][0]["request_sha256"] == original["request_sha256"], "retry accepted a different submission")

    def illegal(self):
        a = self.client("a", "illegal-rng.pk4")
        original = (a / "inventory-0.bin").read_bytes()
        self.run(a, "submit")
        self.settled(a, [self.fixture_manifest["sha256"]["illegal-rng.pk4"]], [(2, 8)])
        require((a / "inventory-0.bin").read_bytes() == original, "strict rejection did not restore exact original")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace", type=Path, default=stack.ROOT / "game/local-e2e")
    parser.add_argument("--case", choices=("all", *CASES), default="all")
    args = parser.parse_args()
    if sys.version_info < (3, 12):
        parser.error("Python 3.12+ is required for the pinned source preparer; use a separate interpreter")
    os.umask(0o077)
    workspace = stack.workspace_path(args.workspace)
    stack.read_manifest(workspace)
    descriptor = os.open(workspace / ".lock", os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600)
    with os.fdopen(descriptor, "a+") as lock:
        stack.private_file(workspace / ".lock")
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        local = stack.Stack(workspace, stack.read_manifest(workspace))
        runs = workspace / "acceptance"
        runs.mkdir(mode=0o700, exist_ok=True)
        require(not runs.is_symlink(), "acceptance directory must not be linked")
        directory = runs / (time.strftime("%Y%m%dT%H%M%S") + "-" + uuid.uuid4().hex[:8])
        directory.mkdir(mode=0o700)
        runner = Acceptance(local, directory)
        report = {"version": 1, "evidence_layer": "native-client", "status": "running", "cases": [],
                  "source_commit": subprocess.check_output(["git", "-C", str(stack.ROOT), "rev-parse", "HEAD"], text=True).strip(),
                  "source_dirty": bool(subprocess.check_output(["git", "-C", str(stack.ROOT), "status", "--porcelain"], text=True)),
                  "host": {"system": platform.system(), "release": platform.release(), "machine": platform.machine(), "python": platform.python_version()},
                  "stack": local.manifest, "linux_sdk": "not_run", "physical_power_loss": "not_run",
                  "sdk_evolution_pokedex_retail_save": "not_run", "substitution": "POSIX I/O and persisted synthetic inventory"}
        try:
            runner.build()
            report.update(native_sha256=sha(runner.probe), tls=json.loads((stack.ROOT / "client/tls-dependency.json").read_text()),
                          fixtures=runner.fixture_manifest, fixture_image=runner.fixture_image)
            for case in CASES if args.case == "all" else (args.case,):
                print("RUN " + case, flush=True)
                runner.case_directory = directory / case
                runner.case_directory.mkdir(mode=0o700)
                begin = len(runner.evidence)
                getattr(runner, case.replace("-", "_"))()
                local.compose("exec", "-T", "backend", "/build/platinumwt-ledger", "verify", capture=True, timeout=60)
                report["cases"].append({"case": case, "status": "pass", "evidence": runner.evidence[begin:], "ledger_replay": "pass"})
                save_json(directory / "report.json", report)
                print("PASS " + case, flush=True)
            report["status"] = "pass"
        except Exception as error:
            report.update(status="fail", failure=type(error).__name__ + ": " + str(error), partial_evidence=runner.evidence)
            raise
        finally:
            save_json(directory / "report.json", report)
            print("Native evidence: " + str(directory / "report.json"), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
