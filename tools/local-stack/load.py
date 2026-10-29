#!/usr/bin/env python3
"""Drive real native C clients through the strict local stack and measure trading load.

Every client is the production native engine over pinned HTTPS with a persisted
synthetic inventory. Results are local measurements only, never a hosted capacity claim.
"""

import argparse
import asyncio
from collections import Counter
import fcntl
import json
import math
import os
from pathlib import Path
import platform
import random
import subprocess
import sys
import time
import uuid

import acceptance
import stack


APPLICATIONS = ("gateway", "backend", "validator")
SETTLED = "idle"
# Lightweight local constraints for the measured services; the validator keeps its compose limits.
CONSTRAINTS = {"gateway": ("2", "512m"), "backend": ("2", "1g")}


def percentile(values, fraction):
    if not values:
        return None
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, max(0, math.ceil(fraction * len(ordered)) - 1))]


def summarize(values):
    return {"count": len(values), "p50_ms": _ms(percentile(values, 0.5)), "p95_ms": _ms(percentile(values, 0.95)),
            "p99_ms": _ms(percentile(values, 0.99)), "max_ms": _ms(max(values) if values else None)}


def _ms(value):
    return None if value is None else round(value / 1000, 3)


def rates(times, start, end, bucket=60):
    """Completed events per second over the window, plus the slowest and median full bucket."""
    inside = [value for value in times if start <= value < end]
    buckets = Counter(int((value - start) // bucket) for value in inside)
    full = [buckets.get(index, 0) / bucket for index in range(int((end - start) // bucket))]
    return {"count": len(inside), "per_second": round(len(inside) / (end - start), 3) if end > start else None,
            "slowest_minute_per_second": round(min(full), 3) if full else None,
            "median_minute_per_second": round(percentile(full, 0.5), 3) if full else None}


def conserved(initial, final):
    """Trades move exact payloads between holders; the global multiset must not change."""
    return Counter(initial) == Counter(final)


def verdict(report, targets):
    measured = report["measured"]
    checks = {
        "duration": measured["window_seconds"] >= targets["duration"],
        "active_clients": measured["active_clients"] >= targets["clients"],
        "completed_pairs_per_second": (measured["completed_pairs"]["per_second"] or 0) >= targets["pairs"],
        "api_p95_below_500ms": (measured["latency"]["all"]["p95_ms"] or math.inf) < 500,
        "conservation": report["conservation"]["status"] == "pass",
    }
    return {"checks": checks, "status": "pass" if all(checks.values()) else "fail"}


class Load:
    def __init__(self, local, directory, args):
        self.local, self.directory, self.args = local, directory, args
        self.runner = acceptance.Acceptance(local, directory)
        self.slots = asyncio.Semaphore(args.concurrency)
        self.tokens = asyncio.Queue(maxsize=max(1, int(2 * args.pairs)))
        self.latency, self.results, self.failures = {}, Counter(), Counter()
        self.measuring = self.stopping = False
        self.submitted = self.completed = 0
        self.events, self.samples, self.checkpoints = [], [], []

    async def probe(self, client, action):
        profile = self.runner.profile
        arguments = [str(self.runner.probe), "run", str(client), str(profile["port"]), profile["hostname"],
                     profile["current_spki_sha256"], profile["next_spki_sha256"], action]
        async with self.slots:
            process = await asyncio.create_subprocess_exec(*arguments, stdout=asyncio.subprocess.PIPE,
                                                           stderr=asyncio.subprocess.STDOUT)
            try:
                output, _ = await asyncio.wait_for(process.communicate(), 90)
            except TimeoutError:
                process.kill()
                await process.wait()
                self.failures["probe_timeout"] += 1
                return None
        summary = None
        for line in output.splitlines():
            event = json.loads(line)
            if "wire_operation" in event:
                if self.measuring:
                    self.latency.setdefault(event["wire_operation"], []).append(event["elapsed_us"])
                    self.results[f"{event['wire_operation']}:{event['wire_result']}"] += 1
            elif "transport_stage" in event and self.measuring:
                self.failures["transport:" + event["transport_stage"]] += 1
            elif event.get("summary"):
                summary = event
        acceptance.require(process.returncode in (0, 1) and summary, f"native client crashed ({process.returncode}): {client}")
        if process.returncode and self.measuring:
            self.failures["retryable_run"] += 1
        return summary

    async def settle(self, client, action, poll):
        """Repeat an action, then exact retries/polls, until the client's journal is idle again."""
        state = await self.probe(client, action)
        while not state or state["phase"] != SETTLED or state["pending_sha256"]:
            await asyncio.sleep(poll * random.uniform(0.75, 1.25))
            state = await self.probe(client, "collect" if state and state["phase"] != "submit_ready" else "reconcile")
        return state

    async def trader(self, client, legal):
        while not self.stopping:
            if legal:
                try:
                    await asyncio.wait_for(self.tokens.get(), self.args.idle_poll * random.uniform(0.5, 1.5))
                except TimeoutError:
                    await self.settle(client, "reconcile", self.args.poll)  # Active presence between trades.
                    continue
            else:
                await asyncio.sleep(self.args.idle_poll * random.uniform(0.5, 1.5))
            if self.stopping:
                break
            self.submitted += self.measuring
            submitted = await self.probe(client, "submit")
            if not legal:
                await self.settle(client, "reconcile", self.args.poll)
                continue
            deadline = time.monotonic() + self.args.match_timeout
            state = submitted
            while not state or state["phase"] != SETTLED or state["pending_sha256"]:
                await asyncio.sleep(self.args.poll * random.uniform(0.75, 1.25))
                if self.stopping and time.monotonic() > deadline and state and state["phase"] == "queued":
                    state = await self.settle(client, "cancel", self.args.poll)  # Unmatched at drain.
                    break
                state = await self.probe(client, "collect" if state and state["phase"] != "submit_ready" else "reconcile")
            self.completed += self.measuring

    async def pace(self):
        interval = 1 / (2 * self.args.pairs)
        upcoming = time.monotonic()
        while not self.stopping:
            upcoming += interval
            await asyncio.sleep(max(0, upcoming - time.monotonic()))
            if self.tokens.full():
                self.failures["pacing_backlog"] += 1
                continue
            self.tokens.put_nowait(True)

    async def faults(self, start):
        for offset, service in zip(self.args.restart_at, self.args.restart):
            await asyncio.sleep(max(0, start + offset - time.monotonic()))
            if self.stopping:
                return
            began = time.time()
            await asyncio.to_thread(self.local.compose, "restart", service, capture=True, timeout=120)
            self.events.append({"restart": service, "at": began, "seconds": round(time.time() - began, 3)})

    async def monitor(self):
        while not self.stopping:
            await asyncio.sleep(60)
            stats = await asyncio.to_thread(self.local.run, ["stats", "--no-stream", "--format", "{{json .}}"],
                                            capture=True, timeout=60)
            project = self.local.manifest["project"]
            self.samples.append({"at": time.time(), "containers": [
                {key: row[key] for key in ("Name", "CPUPerc", "MemUsage", "PIDs")}
                for row in map(json.loads, stats.stdout.splitlines()) if row["Name"].startswith(project)]})
            if len(self.samples) % 10 == 0:
                self.checkpoints.append(await asyncio.to_thread(self.retained))

    def retained(self, sequence=None):
        """Digest receipts/acknowledgements visible at a journal position; later reads must match."""
        if sequence is None:
            sequence = int(self.runner.sql("SELECT coalesce(max(sequence), 0) FROM platinumwt.audit_journal"))
        digest = self.runner.sql(
            "SET bytea_output = 'hex'; SELECT count(*) || ' ' || encode(sha256(convert_to(coalesce(string_agg(row, E'\\n' "
            "ORDER BY row), ''), 'UTF8')), 'hex') FROM (SELECT to_jsonb(r)::text || coalesce(to_jsonb(a)::text, '') AS row "
            "FROM platinumwt.receipts r LEFT JOIN platinumwt.acknowledgements a USING (receipt_id) "
            "WHERE r.receipt_id IN (SELECT (row_key ->> 'receipt_id')::uuid FROM platinumwt.audit_journal "
            f"WHERE table_name = 'acknowledgements' AND sequence <= {sequence})) item").splitlines()[-1]
        return {"sequence": sequence, "digest": digest}

    def pair_times(self):
        rows = self.runner.rows(
            "SELECT extract(epoch FROM max(j.recorded_at))::float8 AS completed FROM platinumwt.matches m "
            "JOIN platinumwt.receipts r ON r.match_id = m.match_id JOIN platinumwt.audit_journal j "
            "ON j.table_name = 'acknowledgements' AND j.row_key ->> 'receipt_id' = r.receipt_id::text "
            "GROUP BY m.match_id HAVING count(*) = 2")
        offers = self.runner.rows(
            "SELECT extract(epoch FROM j.recorded_at)::float8 AS accepted FROM platinumwt.operations o "
            "JOIN platinumwt.audit_journal j ON j.table_name = 'operations' AND j.row_key ->> 'operation_id' = o.operation_id::text "
            "AND j.row_key ->> 'save_id' = o.save_id::text WHERE o.opcode = 2 AND o.result = 1")
        return [row["completed"] for row in rows], [row["accepted"] for row in offers]

    def configure(self, matchers, constrained):
        self.local.env["PWT_LOCAL_MATCHERS"] = str(matchers)
        self.local.compose("up", "-d", "--no-deps", "--force-recreate", "backend", "gateway", capture=True, timeout=120)
        if constrained:
            for service, (cpus, memory) in CONSTRAINTS.items():
                container = self.local.compose("ps", "-q", service, capture=True).stdout.strip()
                self.local.run(["update", "--cpus", cpus, "--memory", memory, "--memory-swap", memory, container], capture=True)
        self.runner.restart_service("gateway")

    def limits(self):
        limits = {}
        for service in ("database", *APPLICATIONS):
            container = self.local.compose("ps", "-q", service, capture=True).stdout.strip()
            info = json.loads(self.local.run(["inspect", container], capture=True).stdout)[0]["HostConfig"]
            limits[service] = {"nano_cpus": info["NanoCpus"], "memory": info["Memory"], "pids": info["PidsLimit"]}
        return limits

    async def drive(self, clients, illegal):
        initial = await asyncio.gather(*(self.settle(client, "reconcile", self.args.poll) for client in clients + illegal))
        acceptance.require(all(state["save_id"] != "0" * 32 for state in initial), "a client did not enroll")
        start = time.monotonic()
        self.wall = [time.time(), None]
        self.measuring = True
        tasks = [asyncio.create_task(self.trader(client, True)) for client in clients]
        tasks += [asyncio.create_task(self.trader(client, False)) for client in illegal]
        helpers = [asyncio.create_task(job) for job in (self.pace(), self.faults(start), self.monitor())]
        while time.monotonic() - start < self.args.duration:
            await asyncio.sleep(min(5, self.args.duration - (time.monotonic() - start)))
            for task in tasks + helpers:
                if task.done() and task.exception():
                    raise task.exception()
        self.wall[1] = time.time()
        self.measuring = False
        self.stopping = True
        for helper in helpers:
            helper.cancel()
        await asyncio.gather(*tasks)
        return initial

    def verify(self, clients, illegal, initial, fixtures):
        final = [self.runner.inspect(client) for client in clients + illegal]
        holders = [state["inventory"] for state in final]
        identities = {row["save_id"]: row for row in self.runner.rows(
            "SELECT replace(save_id::text, '-', '') AS save_id, next_counter::text, event_highwater::text "
            "FROM platinumwt.save_identities")}
        checks = {
            "clients_settled": all(state["phase"] == SETTLED and not state["pending_sha256"] for state in final),
            "one_exact_payload_each": all(banks[0] == banks[1] and len(banks[0]) == 1 for banks in holders),
            "legal_multiset_conserved": conserved([item for state in initial[:len(clients)] for item in state["inventory"][0]],
                                                  [item for banks in holders[:len(clients)] for item in banks[0]]),
            "illegal_payloads_retained": all(banks[0] == [fixtures["illegal-rng.pk4"]] for banks in holders[len(clients):]),
            "client_server_checkpoints": all(
                identities.get(state["save_id"], {}).get("next_counter") == str(state["next_counter"])
                and identities[state["save_id"]]["event_highwater"] == str(state["event_cursor"]) for state in final),
            "no_pending_offers": self.runner.sql("SELECT count(*) FROM platinumwt.offers WHERE state <> 4") == "0",
            "receipts_applied_exactly": self.runner.sql(
                "SELECT count(*) FROM platinumwt.receipts r JOIN platinumwt.ownership o USING (asset_id, save_id, trade_id) "
                "JOIN platinumwt.payloads p USING (asset_id) LEFT JOIN platinumwt.acknowledgements a USING (receipt_id, save_id) "
                "WHERE o.location <> 3 OR a.applied_sha256 IS DISTINCT FROM p.sha256") == "0",
            "receipts_immutable": all(self.retained(item["sequence"]) == item for item in self.checkpoints),
        }
        self.local.compose("exec", "-T", "backend", "/build/platinumwt-ledger", "verify", capture=True, timeout=600)
        checks["ledger_replay"] = True
        return {"status": "pass" if all(checks.values()) else "fail", "checks": checks,
                "immutability_checkpoints": len(self.checkpoints)}


def run(local, directory, args):
    report = {"version": 1, "evidence_layer": "local-native-load", "status": "running",
              "claim": "local measurement only; no hosted capacity claim",
              "source_commit": stack.command(["git", "-C", str(stack.ROOT), "rev-parse", "HEAD"], capture=True).stdout.strip(),
              "source_dirty": bool(stack.command(["git", "-C", str(stack.ROOT), "status", "--porcelain"], capture=True).stdout.strip()),
              "host": {"system": platform.system(), "release": platform.release(), "machine": platform.machine(),
                       "cpus": os.cpu_count(), "python": platform.python_version()},
              "stack": local.manifest, "excluded": ["matchmaking wait between polls", "WiFi/DS radio", "hosted network"],
              "targets": {"duration_seconds": args.duration, "active_clients": args.clients,
                          "completed_pairs_per_second": args.pairs, "api_p95_ms_below": 500},
              "workload": {key: getattr(args, key) for key in ("clients", "illegal", "concurrency", "poll", "idle_poll",
                                                                "matchers", "restart", "restart_at", "constrained")}}
    load = Load(local, directory, args)
    try:
        local.volumes()
        local.database_identity()
        load.runner.build()
        report["native_sha256"] = acceptance.sha(load.runner.probe)
        report["fixtures"], report["fixture_image"] = load.runner.fixture_manifest, load.runner.fixture_image
        load.configure(args.matchers, args.constrained)
        report["images"] = {service: local.run(["inspect", "--format", "{{.Image}}",
                                                local.compose("ps", "-q", service, capture=True).stdout.strip()],
                                               capture=True).stdout.strip() for service in ("database", *APPLICATIONS)}
        report["limits"] = load.limits()
        (directory / "clients").mkdir(mode=0o700)
        load.runner.case_directory = directory / "clients"
        clients = [load.runner.client(f"c{index:04d}", "starter.pk4" if index % 2 else "wild.pk4")
                   for index in range(args.clients)]
        illegal = [load.runner.client(f"x{index:02d}", "illegal-rng.pk4") for index in range(args.illegal)]
        initial = asyncio.run(load.drive(clients, illegal))
        pairs, offers = load.pair_times()
        start, end = load.wall
        latency = {str(op): summarize(values) for op, values in sorted(load.latency.items())}
        latency["all"] = summarize([value for values in load.latency.values() for value in values])
        report["measured"] = {
            "window": [start, end], "window_seconds": round(end - start, 3), "active_clients": args.clients,
            "completed_pairs": rates(pairs, start, end), "acknowledged_offers": rates(offers, start, end),
            "client_cycles_completed": load.completed, "client_submissions": load.submitted,
            "latency": latency, "results": dict(load.results), "failures": dict(load.failures),
            "restarts": load.events, "resource_samples": load.samples,
        }
        report["conservation"] = load.verify(clients, illegal, initial, load.runner.fixture_manifest["sha256"])
        report["verdict"] = verdict(report, {"duration": args.duration, "clients": args.clients, "pairs": args.pairs})
        report["status"] = report["verdict"]["status"]
    except Exception as error:
        report.update(status="fail", failure=type(error).__name__ + ": " + str(error))
        raise
    finally:
        local.env.pop("PWT_LOCAL_MATCHERS", None)
        # Recreate the measured applications with their default single worker and no extra limits.
        local.compose("up", "-d", "--no-deps", "--force-recreate", "backend", "gateway", capture=True, timeout=120, check=False)
        acceptance.save_json(directory / "report.json", report)
        print("Load evidence: " + str(directory / "report.json"), flush=True)
    return 0 if report["status"] == "pass" else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace", type=Path, default=stack.ROOT / "game/local-e2e")
    parser.add_argument("--duration", type=float, default=3600, help="measured seconds")
    parser.add_argument("--clients", type=int, default=1000)
    parser.add_argument("--illegal", type=int, default=10, help="extra clients that keep submitting a strict-illegal fixture")
    parser.add_argument("--pairs", type=float, default=10, help="target completed reciprocal pairs per second")
    parser.add_argument("--concurrency", type=int, default=96, help="concurrent native client processes")
    parser.add_argument("--poll", type=float, default=1.0, help="seconds between a queued client's polls")
    parser.add_argument("--idle-poll", type=float, default=45, help="seconds between an idle client's presence checks")
    parser.add_argument("--match-timeout", type=float, default=30, help="drain seconds before cancelling an unmatched offer")
    parser.add_argument("--matchers", type=int, default=4, choices=range(1, 9), help="concurrent matcher workers")
    parser.add_argument("--restart", nargs="*", default=["backend", "validator", "gateway", "database"])
    parser.add_argument("--restart-at", nargs="*", type=float, default=[900, 1800, 2700, 3300])
    parser.add_argument("--unconstrained", dest="constrained", action="store_false",
                        help="skip the local CPU/memory limits on the gateway and backend")
    args = parser.parse_args()
    if len(args.restart) != len(args.restart_at) or not set(args.restart) <= {"database", *APPLICATIONS}:
        parser.error("give one --restart-at offset per known --restart service")
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
        runs = workspace / "load"
        runs.mkdir(mode=0o700, exist_ok=True)
        acceptance.require(not runs.is_symlink(), "load directory must not be linked")
        directory = runs / (time.strftime("%Y%m%dT%H%M%S") + "-" + uuid.uuid4().hex[:8])
        directory.mkdir(mode=0o700)
        return run(local, directory, args)


if __name__ == "__main__":
    sys.exit(main())
