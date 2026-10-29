#!/usr/bin/env python3
"""Drive real native C clients through the strict local stack and measure trading load.

Every client is the production native engine over pinned HTTPS with a persisted
synthetic inventory. The clients run in a short-lived container inside the
gateway's network namespace so each can use its own loopback source network;
the gateway's per-network admission limits stay in force. Results are local
measurements only, never a hosted capacity claim.
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
# The gateway owns the namespace shared by the validator, collectors and clients; restarting it
# would strand them, so load faults restart only the services that rejoin it.
RESTARTABLE = ("backend", "validator", "database")
# Local CPU/memory limits for the measured services; the validator is sized per Submit worker.
CONSTRAINTS = {"gateway": ["--cpus", "2", "--memory", "512m", "--memory-swap", "512m"], "backend": ["--cpus", "2", "--memory", "1g", "--memory-swap", "1g"]}
GATEWAY_PORT = 8443


def percentile(values, fraction):
    if not values:
        return None
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, max(0, math.ceil(fraction * len(ordered)) - 1))]


def _ms(value):
    return None if value is None else round(value / 1000, 3)


def summarize(values):
    return {"count": len(values), "p50_ms": _ms(percentile(values, 0.5)), "p95_ms": _ms(percentile(values, 0.95)),
            "p99_ms": _ms(percentile(values, 0.99)), "max_ms": _ms(max(values) if values else None)}


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


def source(index):
    """A distinct loopback /32 per client, so per-network admission sees separate players."""
    return f"127.10.{index // 250}.{1 + index % 250}"


def verdict(report, targets):
    measured = report["measured"]
    checks = {
        "duration": measured["window_seconds"] >= targets["duration"],
        "active_clients": measured["active_clients"] >= targets["clients"],
        "completed_pairs_per_second": (measured["completed_pairs"]["per_second"] or 0) >= targets["pairs"],
        # Client-observed, including the native client's own TLS handshake; stricter than gateway time.
        "api_p95_below_500ms": (measured["latency"]["all"]["p95_ms"] or math.inf) < 500,
        "conservation": report["conservation"]["status"] == "pass",
    }
    return {"checks": checks, "status": "pass" if all(checks.values()) else "fail"}


class Driver:
    """Native client scheduler; runs inside the gateway namespace without Docker access."""

    def __init__(self, spec):
        self.spec = spec
        self.slots = asyncio.Semaphore(spec["concurrency"])
        self.tokens = asyncio.Queue(maxsize=max(1, int(2 * spec["offer_rate"])))
        self.latency, self.results, self.failures = {}, Counter(), Counter()
        self.measuring = self.stopping = False
        self.submitted = self.completed = 0

    async def probe(self, client, action):
        spec = self.spec
        arguments = [spec["probe"], "run", client["path"], str(GATEWAY_PORT), "localhost",
                     spec["pins"][0], spec["pins"][1], action]
        async with self.slots:
            process = await asyncio.create_subprocess_exec(
                *arguments, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.STDOUT,
                env=dict(os.environ, WT_PROBE_SOURCE=client["source"]))
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
                    self.latency.setdefault(str(event["wire_operation"]), []).append(event["elapsed_us"])
                    self.results[f"{event['wire_operation']}:{event['wire_result']}"] += 1
            elif "transport_stage" in event and self.measuring:
                self.failures["transport:" + event["transport_stage"]] += 1
            elif event.get("summary"):
                summary = event
        acceptance.require(process.returncode in (0, 1) and summary,
                           f"native client crashed ({process.returncode}): {client['path']}")
        if process.returncode and self.measuring:
            self.failures["retryable_run"] += 1
        return summary

    async def poll(self, client, state):
        await asyncio.sleep(self.spec["poll"] * random.uniform(0.75, 1.25))
        return await self.probe(client, "collect" if state and state["phase"] != "submit_ready" else "reconcile")

    async def settle(self, client, action):
        """Run an action, then exact retries/polls, until the client's journal is idle again."""
        state = await self.probe(client, action)
        while not state or state["phase"] != SETTLED or state["pending_sha256"]:
            state = await self.poll(client, state)
        return state

    async def trader(self, client):
        spec = self.spec
        while not self.stopping:
            wait = spec["idle_poll"] * random.uniform(0.5, 1.5)
            if client["legal"]:
                try:
                    await asyncio.wait_for(self.tokens.get(), wait)
                except TimeoutError:
                    await self.settle(client, "reconcile")  # Active presence between trades.
                    continue
            else:
                await asyncio.sleep(wait)
            if self.stopping:
                break
            self.submitted += self.measuring
            state = await self.probe(client, "submit")
            deadline = time.monotonic() + spec["match_timeout"]
            while not state or state["phase"] != SETTLED or state["pending_sha256"]:
                if self.stopping and time.monotonic() > deadline and state and state["phase"] == "queued":
                    await self.settle(client, "cancel")  # Unmatched when the run drains.
                    break
                state = await self.poll(client, state)
            self.completed += self.measuring and client["legal"]

    async def pace(self):
        interval = 1 / (2 * self.spec["offer_rate"])
        upcoming = time.monotonic()
        while not self.stopping:
            upcoming += interval
            await asyncio.sleep(max(0, upcoming - time.monotonic()))
            if self.tokens.full():
                self.failures["pacing_backlog"] += 1
            else:
                self.tokens.put_nowait(True)

    async def inspect(self, client):
        async with self.slots:
            process = await asyncio.create_subprocess_exec(self.spec["probe"], "inspect", client["path"],
                                                           stdout=asyncio.subprocess.PIPE)
            output, _ = await process.communicate()
        acceptance.require(process.returncode == 0, "cannot inspect " + client["path"])
        return json.loads(output)

    async def run(self, started):
        clients = self.spec["clients"]
        for client in clients:
            subprocess.run([self.spec["probe"], "init", client["path"], client["fixture"]], check=True, timeout=10)
        initial = await asyncio.gather(*(self.settle(client, "reconcile") for client in clients))
        acceptance.require(all(state["save_id"] != "0" * 32 for state in initial), "a client did not enroll")
        start, wall = time.monotonic(), [time.time(), None]
        acceptance.save_json(started, {"started": wall[0]})
        self.measuring = True
        tasks = [asyncio.create_task(self.trader(client)) for client in clients]
        pacer = asyncio.create_task(self.pace())
        while time.monotonic() - start < self.spec["duration"]:
            await asyncio.sleep(min(5, self.spec["duration"] - (time.monotonic() - start)))
            for task in tasks + [pacer]:
                if task.done() and task.exception():
                    raise task.exception()
        wall[1] = time.time()
        self.measuring = False
        self.stopping = True
        pacer.cancel()
        await asyncio.gather(*tasks)
        final = await asyncio.gather(*(self.inspect(client) for client in clients))
        return {"window": wall, "latency": self.latency, "results": dict(self.results), "failures": dict(self.failures),
                "client_submissions": self.submitted, "client_cycles_completed": self.completed,
                "initial": initial, "final": final}


class Load:
    """Host side: owned container setup, faults, monitoring and authoritative verification."""

    def __init__(self, local, directory, args):
        self.local, self.directory, self.args = local, directory, args
        self.runner = acceptance.Acceptance(local, directory)
        self.events, self.samples, self.checkpoints = [], [], []
        self.image = local.manifest["project"] + "-server"

    def container(self, *arguments, network="none", timeout=None):
        """Run project tooling as this user with only the repository (read-only) and this run mounted."""
        root, run = str(stack.ROOT), str(self.directory)
        return self.local.run(["run", "--rm", "--network", network, "--read-only", "--tmpfs", "/tmp",
                               "--cap-drop", "ALL", "--security-opt", "no-new-privileges:true",
                               "--user", f"{os.getuid()}:{os.getgid()}",
                               "--mount", f"type=bind,source={root},target={root},readonly",
                               "--mount", f"type=bind,source={run},target={run}", self.image, *arguments],
                              capture=True, timeout=timeout)

    def build_probe(self):
        """Build the same sanitizer-instrumented native client inside the service toolchain image."""
        tls = subprocess.run([sys.executable, str(stack.ROOT / "scripts/prepare-tls.py")], check=True, text=True,
                             capture_output=True, timeout=120).stdout.strip()
        build = self.directory / "native-build"
        result = self.container("sh", "-euc", 'cmake -G Ninja -S "$1/client/tests" -B "$2" -DMBEDTLS_SOURCE="$3" >/dev/null '
                                '&& cmake --build "$2" --target trade_probe --parallel 4 >/dev/null', "build",
                                str(stack.ROOT), str(build), tls, timeout=900)
        acceptance.require(result.returncode == 0, "native client build failed")
        return build / "trade_probe"

    def configure(self, args):
        """Recreate the stack with the run's sizing: one isolated validator worker and CPU per Submit handler."""
        workers = str(args.submission_workers)
        self.local.env.update(PWT_LOCAL_MATCHERS=str(args.matchers), PWT_LOCAL_SUBMISSION_WORKERS=workers,
                              PWT_LOCAL_VALIDATOR_WORKERS=workers)
        self.local.up()
        self.local.run(["update", "--cpus", workers, self.container_id("validator")], capture=True)
        if args.constrained:
            for service, limits in CONSTRAINTS.items():
                self.local.run(["update", *limits, self.container_id(service)], capture=True)

    def restore(self):
        """Recreate every application with compose defaults, which also clears run-specific limits."""
        for name in ("PWT_LOCAL_MATCHERS", "PWT_LOCAL_SUBMISSION_WORKERS", "PWT_LOCAL_VALIDATOR_WORKERS"):
            self.local.env.pop(name, None)
        self.local.up()

    def container_id(self, service):
        return self.local.compose("ps", "-q", service, capture=True).stdout.strip()

    def limits(self):
        limits = {}
        for service in ("database", *APPLICATIONS):
            info = json.loads(self.local.run(["inspect", self.container_id(service)], capture=True).stdout)[0]["HostConfig"]
            limits[service] = {"nano_cpus": info["NanoCpus"], "memory": info["Memory"], "pids": info["PidsLimit"]}
        return limits

    async def faults(self, start):
        for offset, service in zip(self.args.restart_at, self.args.restart):
            await asyncio.sleep(max(0, start + offset - time.time()))
            began = time.time()
            await asyncio.to_thread(self.local.compose, "restart", service, capture=True, timeout=120)
            self.events.append({"restart": service, "at": began, "seconds": round(time.time() - began, 3)})

    async def monitor(self):
        project = self.local.manifest["project"]
        while True:
            await asyncio.sleep(60)
            stats = await asyncio.to_thread(self.local.run, ["stats", "--no-stream", "--format", "{{json .}}"],
                                            capture=True, timeout=60)
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

    async def drive(self, spec):
        """Run the driver container while restarts and sampling proceed on the host."""
        started = self.directory / "started.json"
        spec_path, output = self.directory / "spec.json", self.directory / "driver.json"
        acceptance.save_json(spec_path, dict(spec, started=str(started)))
        root, run = str(stack.ROOT), str(self.directory)
        process = await asyncio.create_subprocess_exec(
            *self.local.docker, "run", "--rm", "--name", f"{self.local.manifest['project']}-load-{self.directory.name[-8:]}",
            "--network", "container:" + self.container_id("gateway"), "--read-only", "--tmpfs", "/tmp",
            "--cap-drop", "ALL", "--security-opt", "no-new-privileges:true", "--user", f"{os.getuid()}:{os.getgid()}",
            "--mount", f"type=bind,source={root},target={root},readonly", "--mount", f"type=bind,source={run},target={run}",
            self.image, "python3", str(Path(__file__).resolve()), "drive", str(spec_path), str(output),
            env=self.local.env, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.STDOUT)
        logging = asyncio.create_task(process.stdout.read())
        while not started.exists():
            acceptance.require(process.returncode is None, "load driver exited before measuring")
            await asyncio.sleep(1)
        helpers = [asyncio.create_task(self.faults(json.loads(started.read_text())["started"])),
                   asyncio.create_task(self.monitor())]
        await process.wait()
        for helper in helpers:
            if helper.done() and helper.exception():
                raise helper.exception()
            helper.cancel()
        (self.directory / "driver.log").write_bytes(await logging)
        acceptance.require(process.returncode == 0, "load driver failed; see driver.log")
        return json.loads(output.read_text())

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

    def server_latency(self, service, start, end):
        """Service-measured handling time per operation/result from structured logs, excluding client TLS work."""
        logs = self.local.compose("logs", "--no-color", "--no-log-prefix", "--since", str(int(start)),
                                  "--until", str(int(end) + 1), service, capture=True, timeout=600).stdout
        durations = {}
        for line in logs.splitlines():
            if line.startswith('{"event":"trade_request"'):
                record = json.loads(line)
                if start * 1000 <= record["timestamp_ms"] < end * 1000:
                    durations.setdefault(f"{record['operation']}:{record['result']}", []).append(record["duration_us"])
        summary = {name: summarize(values) for name, values in sorted(durations.items())}
        summary["all"] = summarize([value for values in durations.values() for value in values])
        return summary

    def verify(self, results, legal, fixtures):
        initial, final = results["initial"], results["final"]
        holders = [state["inventory"] for state in final]
        identities = {row["save_id"]: row for row in self.runner.rows(
            "SELECT replace(save_id::text, '-', '') AS save_id, next_counter::text, event_highwater::text "
            "FROM platinumwt.save_identities")}
        checks = {
            "clients_settled": all(state["phase"] == SETTLED and not state["pending_sha256"] for state in final),
            "one_exact_payload_each": all(banks[0] == banks[1] and len(banks[0]) == 1 for banks in holders),
            "legal_multiset_conserved": conserved([item for state in initial[:legal] for item in state["inventory"][0]],
                                                  [item for banks in holders[:legal] for item in banks[0]]),
            "illegal_payloads_retained": all(banks[0] == [fixtures["illegal-rng.pk4"]] for banks in holders[legal:]),
            # Mutation counters are exact; a client may not yet have read its newest event.
            "client_server_checkpoints": all(
                identities.get(state["save_id"], {}).get("next_counter") == str(state["next_counter"])
                and int(identities[state["save_id"]]["event_highwater"]) >= state["event_cursor"] for state in final),
            "no_pending_offers": self.runner.sql("SELECT count(*) FROM platinumwt.offers WHERE state <> 4") == "0",
            "receipts_applied_exactly": self.runner.sql(
                "SELECT count(*) FROM platinumwt.receipts r JOIN platinumwt.ownership o USING (asset_id, save_id, trade_id) "
                "JOIN platinumwt.payloads p USING (asset_id) LEFT JOIN platinumwt.acknowledgements a USING (receipt_id, save_id) "
                "WHERE o.location <> 3 OR a.applied_sha256 IS DISTINCT FROM p.sha256") == "0",
            "receipts_immutable": all(self.retained(item["sequence"]) == item for item in self.checkpoints),
        }
        self.local.compose("exec", "-T", "backend", "/build/platinumwt-ledger", "verify", capture=True, timeout=1200)
        checks["ledger_replay"] = True
        return {"status": "pass" if all(checks.values()) else "fail", "checks": checks,
                "immutability_checkpoints": len(self.checkpoints)}


def run(local, directory, args):
    git = ["git", "-C", str(stack.ROOT)]
    report = {"version": 1, "evidence_layer": "local-native-load", "status": "running",
              "claim": "local measurement only; no hosted capacity claim",
              "source_commit": stack.command(git + ["rev-parse", "HEAD"], capture=True).stdout.strip(),
              "source_dirty": bool(stack.command(git + ["status", "--porcelain"], capture=True).stdout.strip()),
              "host": {"system": platform.system(), "release": platform.release(), "machine": platform.machine(),
                       "cpus": os.cpu_count(), "python": platform.python_version()},
              "stack": local.manifest, "excluded": ["matchmaking wait between polls", "WiFi/DS radio", "hosted network"],
              "client_networks": "one loopback /32 per client inside the gateway namespace",
              "targets": {"duration_seconds": args.duration, "active_clients": args.clients,
                          "completed_pairs_per_second": args.pairs, "api_p95_ms_below": 500},
              "workload": {key: getattr(args, key) for key in ("clients", "illegal", "offer_rate", "concurrency", "poll",
                                                                "idle_poll", "matchers", "submission_workers", "restart", "restart_at", "constrained")}}
    load = Load(local, directory, args)
    try:
        local.volumes()
        local.database_identity()
        load.runner.build()  # Generates the strict legal/illegal fixtures with the pinned validator image.
        probe = load.build_probe()
        report["native_sha256"] = acceptance.sha(probe)
        report["fixtures"], report["fixture_image"] = load.runner.fixture_manifest, load.runner.fixture_image
        load.configure(args)
        report["images"] = {service: local.run(["inspect", "--format", "{{.Image}}", load.container_id(service)],
                                               capture=True).stdout.strip() for service in ("database", *APPLICATIONS)}
        report["limits"] = load.limits()
        clients_directory = directory / "clients"
        clients_directory.mkdir(mode=0o700)
        fixtures = load.runner.fixtures
        clients = [{"path": str(clients_directory / f"c{index:04d}"), "legal": True, "source": source(index),
                    "fixture": str(fixtures / ("starter.pk4" if index % 2 else "wild.pk4"))} for index in range(args.clients)]
        clients += [{"path": str(clients_directory / f"x{index:02d}"), "legal": False, "source": source(args.clients + index),
                     "fixture": str(fixtures / "illegal-rng.pk4")} for index in range(args.illegal)]
        profile = load.runner.profile
        spec = {"probe": str(probe), "pins": [profile["current_spki_sha256"], profile["next_spki_sha256"]],
                "clients": clients, **{key: getattr(args, key) for key in
                                       ("duration", "offer_rate", "concurrency", "poll", "idle_poll", "match_timeout")}}
        results = asyncio.run(load.drive(spec))
        start, end = results["window"]
        pairs, offers = load.pair_times()
        latency = {op: summarize(values) for op, values in sorted(results["latency"].items())}
        latency["all"] = summarize([value for values in results["latency"].values() for value in values])
        report["measured"] = {
            "window": [start, end], "window_seconds": round(end - start, 3), "active_clients": args.clients,
            "completed_pairs": rates(pairs, start, end), "acknowledged_offers": rates(offers, start, end),
            "latency": latency, "gateway_latency": load.server_latency("gateway", start, end),
            "backend_latency": load.server_latency("backend", start, end),
            **{key: results[key] for key in ("results", "failures", "client_submissions", "client_cycles_completed")},
            "restarts": load.events, "resource_samples": load.samples,
        }
        report["conservation"] = load.verify(results, args.clients, load.runner.fixture_manifest["sha256"])
        report["verdict"] = verdict(report, {"duration": args.duration, "clients": args.clients, "pairs": args.pairs})
        report["status"] = report["verdict"]["status"]
    except Exception as error:
        report.update(status="fail", failure=type(error).__name__ + ": " + str(error))
        raise
    finally:
        try:
            load.restore()
        except (OSError, subprocess.SubprocessError, ValueError) as error:
            report.setdefault("cleanup_failures", []).append(f"restore default backend/gateway settings manually ({error})")
        acceptance.save_json(directory / "report.json", report)
        print("Load evidence: " + str(directory / "report.json"), flush=True)
    return 0 if report["status"] == "pass" else 1


def drive_main(spec_path, output):
    spec = json.loads(Path(spec_path).read_text())
    results = asyncio.run(Driver(spec).run(Path(spec["started"])))
    acceptance.save_json(Path(output), results)
    return 0


def main():
    if sys.argv[1:2] == ["drive"]:
        return drive_main(*sys.argv[2:4])
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace", type=Path, default=stack.ROOT / "game/local-e2e")
    parser.add_argument("--duration", type=float, default=3600, help="measured seconds")
    parser.add_argument("--clients", type=int, default=1000)
    parser.add_argument("--illegal", type=int, default=10, help="extra clients that keep submitting a strict-illegal fixture")
    parser.add_argument("--pairs", type=float, default=10, help="target completed reciprocal pairs per second")
    parser.add_argument("--offer-rate", type=float, help="paced pairs/s of offered load (default 1.1 x --pairs); "
                        "the completed-pairs target is unchanged")
    parser.add_argument("--concurrency", type=int, default=192, help="concurrent native client processes")
    parser.add_argument("--poll", type=float, default=1.0, help="seconds between a queued client's polls")
    parser.add_argument("--idle-poll", type=float, default=45, help="seconds between an idle client's presence checks")
    parser.add_argument("--match-timeout", type=float, default=30, help="drain seconds before cancelling an unmatched offer")
    parser.add_argument("--matchers", type=int, default=4, choices=range(1, 9), help="concurrent matcher workers")
    parser.add_argument("--submission-workers", type=int, default=6, choices=range(1, 9),
                        help="concurrent Submit handlers, each with one isolated validator worker and CPU")
    parser.add_argument("--restart", nargs="*", default=["backend", "validator", "database"])
    parser.add_argument("--restart-at", nargs="*", type=float, default=[900, 1800, 2700])
    parser.add_argument("--unconstrained", dest="constrained", action="store_false",
                        help="skip the local CPU/memory limits on the gateway and backend")
    args = parser.parse_args()
    if len(args.restart) != len(args.restart_at) or not set(args.restart) <= set(RESTARTABLE):
        parser.error("give one --restart-at offset per --restart service: " + ", ".join(RESTARTABLE))
    if not 0 < args.clients + args.illegal <= 62500:
        parser.error("use 1-62500 clients")
    args.offer_rate = args.offer_rate or 1.1 * args.pairs
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
