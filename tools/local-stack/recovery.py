#!/usr/bin/env python3
"""Rehearse isolated PostgreSQL 16 base-backup and WAL recovery for a local stack.

Restores go only into new volumes owned by this run. A restored cluster is
evidence only: it is never served, swapped in, or used to open the closed
production recovery gate.
"""

import argparse
import fcntl
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
import uuid

import acceptance
import observability
import policy
import stack


RECOVERY_LABEL = "studio.nouvellelune.platinumwt.recovery-run"
PSQL_CONNECTION = ["-h", "/var/run/postgresql", "-U", "platinumwt_local"]
# Tables whose exact rows a restore must reproduce, including the replay journal.
TABLES = ("save_identities", "payloads", "offers", "ownership", "matches", "receipts", "operations",
          "trade_events", "acknowledgements", "matcher_attempts", "audit_journal")
# Root only prepares owned volumes, then drops to the image's postgres user.
CAPABILITIES = ["--cap-drop", "ALL"] + [item for cap in ("CHOWN", "DAC_OVERRIDE", "FOWNER", "SETUID", "SETGID")
                                         for item in ("--cap-add", cap)]
ISOLATED = ["--network", "none", *CAPABILITIES, "--security-opt", "no-new-privileges:true"]
SEGMENT_NAME = re.compile(r"[0-9A-F]{24}")
LSN = re.compile(r"[0-9A-F]{1,8}/[0-9A-F]{1,8}")
# The reason each controlled restore must produce; any other outcome fails the rehearsal.
EXPECTED = {"accepted": "accepted", "missing-wal": "rejected: recovery did not reach its target",
            "truncated-wal": "rejected: recovery did not reach its target",
            "stale-base-only": "rejected: recovery did not reach its target",
            "behind-highwater": "rejected: behind acknowledged client highwater"}


def postgres_image():
    return policy.yaml.safe_load(stack.COMPOSE.read_text())["services"]["database"]["image"]


def digest_sql(table):
    return (f"SELECT count(*)::text || ' ' || encode(sha256(convert_to(coalesce(string_agg(row_text, E'\\n' "
            f"ORDER BY row_text), ''), 'UTF8')), 'hex') FROM (SELECT to_jsonb(t)::text AS row_text "
            f"FROM {table} t) rows")


def capture(sql):
    """Digest every retained row, the applied migrations and each identity checkpoint."""
    tables = {}
    for table in TABLES + ("platinumwt_meta.schema_migrations",):
        name = table if "." in table else "platinumwt." + table
        count, digest = sql("SET bytea_output = 'hex'; SET timezone = 'UTC'; " + digest_sql(name)).split()[-2:]
        tables[table] = {"rows": int(count), "sha256": digest}
    identities = json.loads(sql("SELECT coalesce(json_object_agg(save_id::text, json_build_array("
                                "next_counter::text, event_highwater::text)), '{}') FROM platinumwt.save_identities"))
    applied = json.loads(sql("SELECT coalesce(json_object_agg(save_id, digests), '{}') FROM (SELECT o.save_id::text, "
                             "json_agg(DISTINCT encode(p.sha256, 'hex')) AS digests FROM platinumwt.ownership o "
                             "JOIN platinumwt.payloads p USING (asset_id) WHERE o.location = 3 GROUP BY o.save_id) held"))
    return {"tables": tables, "identities": {key: [int(value) for value in pair] for key, pair in identities.items()},
            "applied": {key: sorted(value) for key, value in applied.items()}}


def client_highwater(state):
    """The client's own retained checkpoint, recorded independently of the backup lineage."""
    return {"save_id": str(uuid.UUID(hex=state["save_id"])), "next_counter": state["next_counter"],
            "event_cursor": state["event_cursor"], "phase": state["phase"], "pending": bool(state["pending_sha256"]),
            "holding": sorted(set(state["inventory"][0]))}


def behind(highwater, restored):
    """List clients whose acknowledged operations, events or held payloads the restore does not cover."""
    reasons = []
    for client in highwater:
        identity = restored["identities"].get(client["save_id"])
        if identity is None:
            reasons.append(client["save_id"] + ": identity missing")
        elif identity[0] < client["next_counter"] or identity[1] < client["event_cursor"]:
            reasons.append(f"{client['save_id']}: restored counter/events {identity} behind client "
                           f"{[client['next_counter'], client['event_cursor']]}")
        # Every client in the rehearsal has applied a receipt, so what it holds must be an applied delivery.
        missing = set(client["holding"]) - set(restored["applied"].get(client["save_id"], []))
        if missing:
            reasons.append(f"{client['save_id']}: held payloads {sorted(missing)} have no applied receipt")
    return reasons


def matches_clients(highwater, reference):
    """Once serving pauses, the cluster must equal, not exceed, every settled client's checkpoint."""
    return all(reference["identities"].get(client["save_id"]) == [client["next_counter"], client["event_cursor"]]
               for client in highwater)


def differences(reference, restored):
    tables = sorted(name for name in reference["tables"] if reference["tables"][name] != restored["tables"].get(name))
    for name in ("identities", "applied"):
        if reference[name] != restored[name]:
            tables.append(name)
    return tables


def decide(case):
    """Accept only a promoted, replay-verified restore that exactly matches the paused cluster."""
    checks = case["checks"]
    if not checks.get("promoted"):
        return "rejected: recovery did not reach its target"
    for name in ("ledger_replay", "migrations"):
        if checks.get(name) != "pass":
            return f"rejected: {name} failed"
    if checks["behind_highwater"]:
        return "rejected: behind acknowledged client highwater"
    if checks["differences"]:
        return "rejected: restored rows differ from the paused cluster"
    return "accepted"


def segments(names):
    return sorted(name for name in names if SEGMENT_NAME.fullmatch(name))


class Rehearsal:
    def __init__(self, local, directory):
        self.local, self.directory = local, directory
        self.run_id = directory.name.rsplit("-", 1)[-1]
        self.prefix = f"{local.manifest['project']}-recovery-{self.run_id}"
        # PostgreSQL limits restore point names to 63 bytes.
        self.slot, self.restore_point = "platinumwt_rehearsal_" + self.run_id, "pwt-" + self.run_id
        self.image = postgres_image()
        self.socket = local.manifest["project"] + "-socket"
        self.created, self.containers = [], []
        self.highwater = self.reference = None

    def docker(self, *arguments, **kwargs):
        return self.local.run(list(arguments), capture=True, **kwargs)

    def volume(self, suffix):
        name = f"{self.prefix}-{suffix}"
        existing = self.docker("volume", "ls", "-q", "--filter", "name=^" + name + "$").stdout.split()
        acceptance.require(not existing, "refusing to reuse an existing volume: " + name)
        self.docker("volume", "create", "--label", stack.LABEL + "=" + self.local.manifest["project"],
                    "--label", RECOVERY_LABEL + "=" + self.run_id, name)
        self.created.append(name)
        return name

    def tool(self, script, *mounts, timeout=300):
        """Run one PostgreSQL client step with only the named mounts and no network."""
        arguments = ["run", "--rm", *ISOLATED, "--entrypoint", "sh"]
        for mount in mounts:
            arguments += ["--mount", mount]
        return self.docker(*arguments, self.image, "-euc", script, timeout=timeout).stdout

    def live(self, query):
        return self.local.compose("exec", "-T", "database", "psql", "-XAt", "-v", "ON_ERROR_STOP=1", *PSQL_CONNECTION,
                                  "-d", "platinumwt_e2e", "-c", query, capture=True, timeout=60).stdout.strip()

    def backup(self, base, archive):
        socket = f"type=volume,source={self.socket},target=/var/run/postgresql,readonly"
        self.tool(f"chown postgres:postgres /backup /archive && gosu postgres pg_basebackup {' '.join(PSQL_CONNECTION)} "
                  f"-D /backup/base -X stream -C -S {self.slot} -c fast --manifest-checksums=SHA256 "
                  "&& gosu postgres pg_verifybackup /backup/base",
                  socket, f"type=volume,source={base},target=/backup", f"type=volume,source={archive},target=/archive")
        return self.tool("cat /backup/base/backup_manifest | sha256sum | cut -d' ' -f1",
                         f"type=volume,source={base},target=/backup,readonly").strip()

    def fetch_wal(self, archive, endpos):
        socket = f"type=volume,source={self.socket},target=/var/run/postgresql,readonly"
        self.tool(f"gosu postgres pg_receivewal {' '.join(PSQL_CONNECTION)} -D /archive -S {self.slot} --endpos={endpos} --no-loop",
                  socket, f"type=volume,source={archive},target=/archive")
        self.drop_slot()
        return segments(self.tool("ls /archive", f"type=volume,source={archive},target=/archive,readonly").split())

    def drop_slot(self):
        """A retained slot would hold WAL on the active cluster indefinitely."""
        self.live("SELECT pg_drop_replication_slot(slot_name) FROM pg_replication_slots "
                  f"WHERE slot_name = '{self.slot}'")

    def restore(self, name, base, archive, target, mutate=""):
        """Recover into new owned volumes. mutate: "" complete WAL, a shell edit of a WAL copy, or None for no WAL."""
        data, socket = self.volume(name + "-data"), self.volume(name + "-socket")
        source = archive
        if mutate:
            source = self.volume(name + "-wal")
            self.tool(f"cp -a /archive/. /copy/ && cd /copy && {mutate}",
                      f"type=volume,source={archive},target=/archive,readonly", f"type=volume,source={source},target=/copy")
        settings = "\n".join(("restore_command = 'cp /archive/%f %p'", f"recovery_target_name = '{target}'",
                              "recovery_target_action = 'promote'", ""))
        self.docker("run", "--rm", *ISOLATED, "--env", "SETTINGS=" + settings, "--mount", f"type=volume,source={base},target=/backup,readonly",
                    "--mount", f"type=volume,source={data},target=/data", "--entrypoint", "sh", self.image, "-euc",
                    "cp -a /backup/base/. /data/ && printf %s \"$SETTINGS\" >> /data/postgresql.auto.conf "
                    "&& touch /data/recovery.signal && chown -R postgres:postgres /data && chmod 0700 /data", timeout=300)
        container = f"{self.prefix}-{name}"
        mounts = ["--mount", f"type=volume,source={data},target=/var/lib/postgresql/data",
                  "--mount", f"type=volume,source={socket},target=/var/run/postgresql"]
        if mutate is not None:
            mounts += ["--mount", f"type=volume,source={source},target=/archive,readonly"]
        self.docker("run", "-d", "--name", container, *ISOLATED, "--env", "POSTGRES_HOST_AUTH_METHOD=trust",
                    "--label", RECOVERY_LABEL + "=" + self.run_id, *mounts, self.image,
                    "postgres", "-c", "listen_addresses=", "-c", "fsync=on", "-c", "full_page_writes=on")
        self.containers.append(container)
        case = {"case": name, "target": target, "archive": "damaged copy" if mutate else ("none" if mutate is None else "complete"),
                "checks": {"promoted": False}}
        try:
            case["checks"]["promoted"] = self.promoted(container)
            if not case["checks"]["promoted"]:
                log = self.docker("logs", container, check=False).stderr
                case["postgres"] = [line.split("FATAL:", 1)[1].strip() for line in log.splitlines()
                                    if "FATAL:" in line and "is starting up" not in line][-3:]
            else:
                self.verify(case, container, socket)
        finally:
            self.docker("rm", "-f", container, check=False)
        case["decision"] = decide(case)
        return case

    def promoted(self, container, timeout=120):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            state = self.docker("inspect", "--format", "{{.State.Running}}", container).stdout.strip()
            if state != "true":
                return False
            ready = self.docker("exec", container, "psql", "-XAt", *PSQL_CONNECTION, "-d", "platinumwt_e2e", "-c",
                                "SELECT NOT pg_is_in_recovery()", check=False)
            if ready.returncode == 0 and ready.stdout.strip() == "t":
                return True
            time.sleep(0.5)
        raise TimeoutError("restored cluster neither promoted nor failed: " + container)

    def verify(self, case, container, socket):
        def sql(query):
            return self.docker("exec", container, "psql", "-XAt", "-v", "ON_ERROR_STOP=1", *PSQL_CONNECTION,
                               "-d", "platinumwt_e2e", "-c", query, timeout=60).stdout.strip()
        before = capture(sql)
        checks = case["checks"]
        for name, arguments in (("ledger_replay", ["/build/platinumwt-ledger", "verify"]),
                                ("migrations", ["/build/platinumwt-ledger", "migrate", "/src/migrations"])):
            result = self.docker("run", "--rm", "--network", "none", "--read-only", "--cap-drop", "ALL",
                                 "--security-opt", "no-new-privileges:true",
                                 "--user", f"{os.getuid()}:{os.getgid()}", "--env", "PGHOST=/test/run",
                                 "--env", "PGDATABASE=platinumwt_e2e", "--env", "PGUSER=platinumwt_local",
                                 "--mount", f"type=volume,source={socket},target=/test/run",
                                 self.local.manifest["project"] + "-server", *arguments, check=False, timeout=120)
            checks[name] = "pass" if result.returncode == 0 else "fail"
            case[name + "_output"] = (result.stdout + result.stderr).strip().splitlines()[-1:]
        restored = capture(sql)
        if restored != before:
            checks["migrations"] = "fail: the restored schema was behind and migrate changed it"
        checks["behind_highwater"] = behind(self.highwater, restored)
        checks["differences"] = differences(self.reference, restored)
        case["restored"] = restored["tables"]

    def cleanup(self, verified):
        for container in self.containers:
            self.docker("rm", "-f", container, check=False)
        # Remove only volumes this run created; keep a verified backup and its WAL archive.
        for name in self.created:
            if not verified or name.endswith(("-data", "-socket", "-wal")):
                self.docker("volume", "rm", name, check=False)


def pair(runner, first, second):
    runner.run(first, "submit"); runner.run(second, "submit")
    runner.wait_matched(first, second)
    runner.run(first, "collect"); runner.run(second, "collect")
    runner.paired(first, second)


def rehearse(local, directory):
    report = {"version": 1, "evidence_layer": "local-postgresql-recovery", "status": "running",
              "source_commit": stack.command(["git", "-C", str(stack.ROOT), "rev-parse", "HEAD"], capture=True).stdout.strip(),
              "source_dirty": bool(stack.command(["git", "-C", str(stack.ROOT), "status", "--porcelain"], capture=True).stdout.strip()),
              "stack": local.manifest, "postgres_image": postgres_image(), "production_recovery_gate": "closed (unchanged)",
              "reconciliation": "not performed; the restored cluster is evidence only and was never served",
              "off_site_encryption": "not_run (remains on the AWS backup parent)"}
    runner = acceptance.Acceptance(local, directory)
    rehearsal = Rehearsal(local, directory)
    applications = ("gateway", "backend", "validator")
    paused = False
    try:
        local.volumes()
        local.database_identity()
        runner.build()
        runner.case_directory = directory / "clients"
        runner.case_directory.mkdir(mode=0o700)
        a, b = runner.client("a"), runner.client("b", "wild.pk4")
        pair(runner, a, b)  # Retained before the base backup.
        base, archive = rehearsal.volume("backup"), rehearsal.volume("archive")
        report["base_backup"] = {"volume": base, "manifest_sha256": rehearsal.backup(base, archive), "pg_verifybackup": "pass"}
        rehearsal.live("SELECT pg_switch_wal()")
        c, d = runner.client("c"), runner.client("d", "wild.pk4")
        pair(runner, c, d)
        behind_lsn = rehearsal.live(f"SELECT pg_create_restore_point('{rehearsal.restore_point}-behind')")
        rehearsal.live("SELECT pg_switch_wal()")
        pair(runner, a, b)  # Retained identities advance only in WAL after the backup.
        e = runner.client("e")
        with runner.hold():
            runner.run(e, "submit"); runner.run(e, "cancel")
        clients = (a, b, c, d, e)
        # Independent highwater: each client's own journal, before any interruption.
        rehearsal.highwater = [client_highwater(runner.inspect(client)) for client in clients]
        acceptance.require(all(item["phase"] == "idle" and not item["pending"] for item in rehearsal.highwater),
                           "workload clients did not settle")
        report["client_highwater"] = rehearsal.highwater
        paused = True  # Set first: a partially completed stop must still be undone.
        local.compose("stop", *applications, capture=True, timeout=60)
        rehearsal.reference = capture(rehearsal.live)
        acceptance.require(matches_clients(rehearsal.highwater, rehearsal.reference)
                           and not behind(rehearsal.highwater, rehearsal.reference),
                           "paused cluster does not exactly match its settled clients")
        final_lsn = rehearsal.live(f"SELECT pg_create_restore_point('{rehearsal.restore_point}-final')")
        rehearsal.live("SELECT pg_switch_wal()")
        endpos = rehearsal.live("SELECT pg_current_wal_lsn()")
        acceptance.require(all(LSN.fullmatch(lsn) for lsn in (behind_lsn, final_lsn, endpos)), "unexpected WAL position")
        wal = rehearsal.fetch_wal(archive, endpos)
        names = {lsn: rehearsal.live(f"SELECT pg_walfile_name('{lsn}')") for lsn in (behind_lsn, final_lsn)}
        acceptance.require(all(SEGMENT_NAME.fullmatch(name) for name in names.values()) and names[behind_lsn] in wal and names[final_lsn] in wal and names[behind_lsn] != names[final_lsn],
                           "archived WAL does not span both restore points")
        report["wal"] = {"segments": wal, "behind_lsn": behind_lsn, "final_lsn": final_lsn, "end_lsn": endpos}
        report["reference"] = rehearsal.reference["tables"]
        final, lagging = rehearsal.restore_point + "-final", rehearsal.restore_point + "-behind"
        cases = [rehearsal.restore("accepted", base, archive, final),
                 rehearsal.restore("missing-wal", base, archive, final, f"rm {names[behind_lsn]}"),
                 rehearsal.restore("truncated-wal", base, archive, final, f"truncate -s 8192 {names[final_lsn]}"),
                 rehearsal.restore("stale-base-only", base, archive, final, None),
                 rehearsal.restore("behind-highwater", base, archive, lagging)]
        report["restores"] = cases
        for case in cases:
            acceptance.require(case["decision"] == EXPECTED[case["case"]], f"{case['case']} restore decision was {case['decision']}")
        acceptance.require(capture(rehearsal.live) == rehearsal.reference, "active cluster changed during the rehearsal")
        report["active_unchanged"] = True
        local.compose("start", *reversed(applications), capture=True, timeout=60)
        runner.restart_service("gateway")  # Waits until the HTTPS trading path is ready again.
        paused = False
        local.compose("exec", "-T", "backend", "/build/platinumwt-ledger", "verify", capture=True, timeout=60)
        report["active_ledger_replay"] = "pass"
        if local.observability:
            observability.write_backup_timestamp(local.workspace, time.time())
            report["backup_metric"] = observability.BACKUP_METRIC
        report["status"] = "pass"
    except Exception as error:
        report.update(status="fail", failure=type(error).__name__ + ": " + str(error))
        raise
    finally:
        # Resume serving first; each later step reports its own failure without masking the original one.
        for step, message in ((lambda: paused and local.compose("start", *reversed(applications), capture=True, timeout=60),
                               "Restart the paused applications manually"),
                              (rehearsal.drop_slot, "Drop the retained replication slot manually: " + rehearsal.slot),
                              (lambda: rehearsal.cleanup(report["status"] == "pass"),
                               "Remove this run's labelled recovery containers/volumes manually")):
            try:
                step()
            except (OSError, subprocess.SubprocessError) as error:
                report.setdefault("cleanup_failures", []).append(message)
                print(f"{message} ({error})", file=sys.stderr)
        acceptance.save_json(directory / "report.json", report)
        print("Recovery evidence: " + str(directory / "report.json"), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace", type=Path, default=stack.ROOT / "game/local-e2e")
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
        runs = workspace / "recovery"
        runs.mkdir(mode=0o700, exist_ok=True)
        acceptance.require(not runs.is_symlink(), "recovery directory must not be linked")
        directory = runs / (time.strftime("%Y%m%dT%H%M%S") + "-" + uuid.uuid4().hex[:8])
        directory.mkdir(mode=0o700)
        rehearse(local, directory)
    return 0


if __name__ == "__main__":
    sys.exit(main())
