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
import re
import shutil
import subprocess
import sys
import time
import uuid

import acceptance
import stack


APPLICATIONS = ("gateway", "backend", "validator")
SETTLED = "idle"
# The gateway owns the namespace shared by the validator, collectors and clients; restarting it
# would strand them, so load faults restart only the services that rejoin it. The matcher exists
# only in the production profile.
RESTARTABLE = ("backend", "validator", "database", "matcher")
# Local CPU/memory limits for the measured services; the validator is sized per Submit worker.
CONSTRAINTS = {"gateway": ["--cpus", "2", "--memory", "512m", "--memory-swap", "512m"],
               "backend": ["--cpus", "2", "--memory", "1g", "--memory-swap", "1g"],
               "matcher": ["--cpus", "1", "--memory", "256m", "--memory-swap", "256m"]}
# The production VPS (Hetzner cpx11: 2 vCPU, 1.9 GB) runs every service on two cores. Memory leaves
# room for Prometheus, Grafana, Alertmanager and node-exporter, which use the rest at idle.
VPS_SERVICES = ("database", "gateway", "backend", "validator", "matcher")
VPS_MEMORY = {"database": "448m", "gateway": "160m", "backend": "256m", "validator": "512m", "matcher": "96m"}
FAKE_ENROLL_ATTEMPTS = 5
# LUNE-540: fake traffic may add at most about 5 GB of WAL a day, at the production enrollment cap.
WAL_BUDGET_BYTES_PER_DAY = 5 * 10 ** 9
PRODUCTION_ENROLLMENTS_PER_HOUR = 200
# Fake clients run beside the real ones with their own process budget, from their own networks.
FLOOD_CONCURRENCY = 32
# Samples this long after an overload ends still count as overload, while queues drain.
OVERLOAD_GRACE = 60
GATEWAY_PORT = 8443
# The LUNE-409 targets; shorter or smaller runs are reported but never qualify.
SPEC = {"duration": 3600, "clients": 1000, "pairs": 10, "p95_ms": 500}
PROBE_TIMEOUT = 90
LOOPBACK_RADIX = 250
MAX_CLIENTS = LOOPBACK_RADIX ** 2
DRAIN_TIMEOUT = 600
CHECKPOINT_SECONDS = 300
NOTES = [
    "Clients connect to the gateway's 127.0.0.1:8443 inside its namespace, bypassing Docker's published-port forwarding.",
    "The backend is the disposable device fixture with its local-test recovery barrier, not the production backend binary.",
    "The native client is the sanitizer-instrumented test build; client latency is conservative versus service latency.",
    "Offered load is paced at a fixed multiple of the target; this is not a search for the local ceiling.",
    "The database and driver container are not CPU/memory limited; gateway restarts are excluded (shared namespace owner).",
    "Latency targets count real clients outside the overload window; fake traffic and overload are reported separately.",
]
PRODUCTION_NOTES = [
    "Clients connect to the gateway's 127.0.0.1:8443 inside its namespace, bypassing Docker's published-port forwarding.",
    "The services are the final production image and entrypoints, with the WAL archive barrier and dswt-matcher.",
    "The gateway keeps the local stack's raised enrollment limits so 1,000 saves can enroll at once; fake enrollment "
    "cost is measured and scaled to the production cap of 200 an hour.",
    "The native client is the sanitizer-instrumented test build; client latency is conservative versus service latency.",
    "The database and driver container are not CPU/memory limited; gateway restarts are excluded (shared namespace owner).",
    "Latency targets count real clients outside the overload window; fake traffic and overload are reported separately.",
]


def cpu_list(text):
    """CPU numbers from a cpuset such as 2,3 or 4-5."""
    cpus = []
    for part in text.split(","):
        first, _, last = part.partition("-")
        cpus.extend(range(int(first), int(last or first) + 1))
    return cpus


def flood_source(index):
    """Fake clients use their own loopback networks, apart from every real client."""
    return f"127.20.{(index // LOOPBACK_RADIX) % LOOPBACK_RADIX}.{1 + index % LOOPBACK_RADIX}"


def _number(text):
    match = re.match(r"\s*([0-9.]+)\s*([KMGT]?i?B|kB)?", text)
    if not match:
        return None
    scale = {None: 1, "B": 1, "KiB": 2 ** 10, "MiB": 2 ** 20, "GiB": 2 ** 30, "TiB": 2 ** 40,
             "kB": 10 ** 3, "KB": 10 ** 3, "MB": 10 ** 6, "GB": 10 ** 9, "TB": 10 ** 12}
    return float(match.group(1)) * scale.get(match.group(2), 1)


def peaks(samples):
    """Highest CPU percentage and memory use per container across docker stats samples."""
    result = {}
    for sample in samples:
        for row in sample["containers"]:
            item = result.setdefault(row["Name"], {"cpu_percent": 0.0, "memory_bytes": 0, "pids": 0})
            item["cpu_percent"] = max(item["cpu_percent"], _number(row["CPUPerc"].rstrip("%")) or 0.0)
            item["memory_bytes"] = max(item["memory_bytes"], int(_number(row["MemUsage"].split("/")[0]) or 0))
            item["pids"] = max(item["pids"], int(row["PIDs"]) if str(row["PIDs"]).isdigit() else 0)
    return result


def growth(first, last, seconds):
    """Storage growth between two storage samples, also scaled to a day."""
    if not first or not last or seconds <= 0:
        return None
    result = {}
    for key in ("wal_bytes", "archive_bytes", "database_bytes"):
        if first.get(key) is None or last.get(key) is None:
            continue
        added = last[key] - first[key]
        result[key] = {"added": added, "per_hour": round(added * 3600 / seconds),
                       "gb_per_day": round(added * 86400 / seconds / 1e9, 3)}
    return result


def error_rate(latency, failures):
    """Failed attempts (transport failures and timeouts) over every attempt the clients made, and apart
    from them the delivered replies that asked the client to wait or retry, such as PROCESSING."""
    attempts = sum(len(values) for values in latency.values())
    failed = sum(count for name, count in failures.items() if name.startswith("transport:") or name == "probe_timeout")
    waits = sum(count for name, count in failures.items() if name.startswith("reply:"))
    return {"attempts": attempts, "failed": failed, "rate": round(failed / attempts, 6) if attempts else None,
            "wait_or_retry_replies": waits, "wait_or_retry_rate": round(waits / attempts, 6) if attempts else None}


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
    return f"127.10.{index // LOOPBACK_RADIX}.{1 + index % LOOPBACK_RADIX}"


def settled(state):
    """An idle journal is settled only after its pending packet is retired."""
    return bool(state and state["phase"] == SETTLED and not state["pending_sha256"])


def verdict(report):
    """Gate on the fixed LUNE-409 targets, whatever arguments the run used."""
    measured = report["measured"]
    latency = measured["latency"]
    checks = {
        "duration": measured["window_seconds"] >= SPEC["duration"],
        "active_clients": measured["active_clients"]["trading"] >= SPEC["clients"],
        "completed_pairs_per_second": (measured["completed_pairs"]["per_second"] or 0) >= SPEC["pairs"],
        # Client-observed per attempt, failures and timeouts included, with the native client's own TLS
        # handshake; every operation and the aggregate must be below the limit.
        "api_p95_below_500ms": bool(latency) and all((item["p95_ms"] or math.inf) < SPEC["p95_ms"] for item in latency.values()),
        "conservation": report["conservation"]["status"] == "pass",
    }
    flood = report.get("fake_traffic")
    if flood:
        # Real clients met the targets above while fake traffic ran beside them.
        checks["fake_traffic_ran"] = flood["enrollments"] > 0 or flood["submissions"] > 0
        cost = flood.get("calibration")
        if cost:
            checks["fake_enrollment_wal_within_budget"] = cost["production_cap_bytes_per_day"] <= WAL_BUDGET_BYTES_PER_DAY
    return {"checks": checks, "status": "pass" if all(checks.values()) else "fail"}


class Driver:
    """Native client scheduler; runs inside the gateway namespace without Docker access."""

    def __init__(self, spec):
        self.spec = spec
        self.slots = asyncio.Semaphore(spec["concurrency"])
        self.flood_slots = asyncio.Semaphore(FLOOD_CONCURRENCY)
        self.rate = spec["offer_rate"]
        peak = max(spec["offer_rate"], spec.get("overload_rate") or 0)
        self.tokens = asyncio.Queue(maxsize=max(1, int(2 * peak)))
        self.latency, self.results, self.failures = {}, Counter(), Counter()
        self.overload_latency, self.flood_latency, self.flood_results = {}, {}, Counter()
        self.measuring = self.stopping = self.overloaded = False
        self.submitted = self.completed = self.enrolled = 0
        self.active = set()

    async def probe(self, client, action):
        spec = self.spec
        arguments = [spec["probe"], "run", client["path"], str(GATEWAY_PORT), "localhost",
                     spec["pins"][0], spec["pins"][1], action]
        fake = not client.get("legal", True)
        # Fake traffic and overload are reported apart from the real clients' latency targets.
        if fake:
            latency, results, prefix = self.flood_latency, self.flood_results, "fake:"
        else:
            latency, results, prefix = self.overload_latency if self.overloaded else self.latency, self.results, ""
        async with self.flood_slots if fake else self.slots:
            process = await asyncio.create_subprocess_exec(
                *arguments, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.STDOUT,
                env=dict(os.environ, WT_PROBE_SOURCE=client["source"]))
            try:
                output, _ = await asyncio.wait_for(process.communicate(), PROBE_TIMEOUT)
            except TimeoutError:
                process.kill()
                await process.wait()
                if self.measuring:
                    self.failures[prefix + "probe_timeout"] += 1
                    latency.setdefault("timeout", []).append(PROBE_TIMEOUT * 1_000_000)
                return None
        summary = None
        for line in output.splitlines():
            event = json.loads(line)
            if "transport_stage" in event:
                if self.measuring and event["transport_stage"] == "engine-reply":
                    # A delivered reply asking the client to wait or retry; its wire line already holds the sample.
                    self.failures[prefix + "reply:" + str(event["result"])] += 1
                elif self.measuring:
                    # A failed attempt still costs the client its elapsed time; keep it in the latency sample.
                    self.failures[prefix + "transport:" + event["transport_stage"]] += 1
                    latency.setdefault(str(event["failed_operation"]), []).append(event["elapsed_us"])
            elif "wire_operation" in event:
                if self.measuring:
                    if not fake:
                        self.active.add(client["path"])
                    latency.setdefault(str(event["wire_operation"]), []).append(event["elapsed_us"])
                    results[f"{event['wire_operation']}:{event['wire_result']}"] += 1
            elif event.get("summary"):
                summary = event
        acceptance.require(process.returncode in (0, 1) and summary,
                           f"native client crashed ({process.returncode}): {client['path']}")
        if process.returncode and self.measuring:
            self.failures[prefix + "retryable_run"] += 1
        return summary

    async def poll(self, client, state, action=None):
        await asyncio.sleep(self.spec["poll"] * random.uniform(0.75, 1.25))
        return await self.probe(client, action or ("collect" if state and state["phase"] != "submit_ready" else "reconcile"))

    async def settle(self, client, action):
        """Run an action, then exact retries/polls, until the client's journal is idle again."""
        state = await self.probe(client, action)
        while not settled(state):
            again = None
            if action == "cancel" and state and state["phase"] == "queued":
                # The offer is still queued, so the cancel did not take; a player presses it again.
                self.failures["cancel_repeated"] += 1
                again = "cancel"
            state = await self.poll(client, state, again)
        return state

    async def trader(self, client):
        spec = self.spec
        while not self.stopping:
            interval = spec["idle_poll"] if client["legal"] else spec.get("illegal_interval") or spec["idle_poll"]
            wait = interval * random.uniform(0.5, 1.5)
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
            if self.measuring:
                if client["legal"]:
                    self.submitted += 1
                else:
                    self.flood_results["fake_submissions"] += 1
            state = await self.probe(client, "submit")
            deadline = time.monotonic() + spec["match_timeout"]
            while not settled(state):
                if self.stopping and time.monotonic() > deadline and state and state["phase"] == "queued":
                    await self.settle(client, "cancel")  # Unmatched when the run drains.
                    break
                if self.stopping and time.monotonic() > deadline:
                    # Draining but not cancellable yet: record where clients wait, for the report.
                    self.failures["drain_wait:" + (state["phase"] if state else "no_reply")] += 1
                state = await self.poll(client, state)
            if self.measuring and client["legal"]:
                self.completed += 1

    async def pace(self):
        upcoming = time.monotonic()
        while not self.stopping:
            upcoming += 1 / (2 * self.rate)
            await asyncio.sleep(max(0, upcoming - time.monotonic()))
            if self.tokens.full():
                self.failures["pacing_backlog"] += 1
            else:
                self.tokens.put_nowait(True)

    async def overload(self, start):
        """Offer more trades than the target for a while, then return to the normal rate."""
        spec = self.spec
        await asyncio.sleep(max(0, start + spec["overload_at"] - time.monotonic()))
        self.overloaded, self.rate = True, spec["overload_rate"]
        await asyncio.sleep(spec["overload_seconds"])
        self.rate = spec["offer_rate"]
        await asyncio.sleep(OVERLOAD_GRACE)
        self.overloaded = False

    async def enroll_once(self, index):
        path = Path(self.spec["flood_directory"]) / f"f{index:06d}"
        client = {"path": str(path), "legal": False, "source": flood_source(index)}
        submit = self.spec.get("flood_submit")
        fixture = self.spec["flood_illegal_fixture" if submit else "flood_fixture"]
        try:
            await asyncio.to_thread(subprocess.run, [self.spec["probe"], "init", str(path), fixture], check=True, timeout=10)
            # Retries like a real client, so a busy gateway or a held archive does not hide what it writes.
            for _ in range(FAKE_ENROLL_ATTEMPTS):
                state = await self.probe(client, "reconcile")
                if settled(state):
                    break
                await asyncio.sleep(self.spec["poll"] * random.uniform(0.75, 1.25))
            if self.measuring and state and state["save_id"] != "0" * 32:
                self.enrolled += 1
            if submit and settled(state):
                # One strict-illegal submission, then the save is thrown away: the server's
                # rejection is final whatever the disposable client does next.
                if self.measuring:
                    self.flood_results["fake_submissions"] += 1
                await self.probe(client, "submit")
        except (OSError, ValueError, subprocess.SubprocessError):
            self.failures["fake:enroll_error"] += 1
        finally:
            await asyncio.to_thread(shutil.rmtree, path, True)

    async def flood(self, count=None):
        """New saves that enroll and never trade, each from its own network: paced, or a fixed count."""
        index, upcoming, running = 0, time.monotonic(), set()
        while not self.stopping and (count is None or index < count):
            if count is None:
                upcoming += 1 / self.spec["flood_enroll_rate"]
                await asyncio.sleep(max(0, upcoming - time.monotonic()))
            elif len(running) >= FLOOD_CONCURRENCY:
                await asyncio.wait(running, return_when=asyncio.FIRST_COMPLETED)
            task = asyncio.create_task(self.enroll_once(self.spec.get("flood_offset", 0) + index))
            running.add(task)
            task.add_done_callback(running.discard)
            index += 1
        if running:
            await asyncio.gather(*running, return_exceptions=count is None)

    async def calibrate(self):
        """Run a fixed amount of fake traffic alone, so the host can measure what it writes."""
        spec = self.spec
        started = Path(spec["started"])
        acceptance.save_json(started, {"started": time.time()})
        # The host records the database position first.
        while not started.with_suffix(".go").exists():
            await asyncio.sleep(0.2)
        self.measuring = True
        await self.flood(spec["count"])
        return {"enrolled": self.enrolled, "results": dict(self.flood_results), "failures": dict(self.failures)}

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
        helpers = [asyncio.create_task(self.pace())]
        if self.spec.get("overload_rate"):
            helpers.append(asyncio.create_task(self.overload(start)))
        if self.spec.get("flood_enroll_rate"):
            helpers.append(asyncio.create_task(self.flood()))
        while time.monotonic() - start < self.spec["duration"]:
            await asyncio.sleep(min(5, self.spec["duration"] - (time.monotonic() - start)))
            for task in tasks + helpers:
                if task.done() and not task.cancelled() and task.exception():
                    raise task.exception()
        wall[1] = time.time()
        self.measuring = False
        self.stopping = True
        for helper in helpers:
            helper.cancel()
        await asyncio.gather(*helpers, return_exceptions=True)
        # Bound the drain; a client that cannot settle fails conservation instead of hanging the run.
        done, pending = await asyncio.wait(tasks, timeout=DRAIN_TIMEOUT)
        for task in pending:
            task.cancel()
        await asyncio.gather(*pending, return_exceptions=True)
        for task in done:
            if task.exception():
                raise task.exception()
        final = await asyncio.gather(*(self.inspect(client) for client in clients))
        return {"window": wall, "latency": self.latency, "results": dict(self.results), "failures": dict(self.failures),
                "overload_latency": self.overload_latency, "fake_latency": self.flood_latency,
                "fake_results": dict(self.flood_results), "fake_enrollments": self.enrolled,
                "legal_submissions": self.submitted, "legal_cycles_completed": self.completed,
                "clients_exchanging": len(self.active), "drain_unsettled": len(pending),
                "initial": initial, "final": final}


class Load:
    """Host side: owned container setup, faults, monitoring and authoritative verification."""

    def __init__(self, local, directory, args):
        self.local, self.directory, self.args = local, directory, args
        self.runner = acceptance.Acceptance(local, directory)
        self.events, self.samples, self.checkpoints, self.storage_samples = [], [], [], []
        # The production image has no toolchain; build and drive the clients from the build stage instead.
        self.image = local.manifest["project"] + ("-toolchain" if local.production else "-server")
        self.offsite = None

    def container(self, *arguments, network="none", timeout=None):
        """Run project tooling as this user with only the repository (read-only) and this run mounted."""
        root, run = str(stack.ROOT), str(self.directory)
        return self.local.run(["run", "--rm", "--network", network, "--read-only", "--tmpfs", "/tmp",
                               "--cap-drop", "ALL", "--security-opt", "no-new-privileges:true",
                               "--user", f"{os.getuid()}:{os.getgid()}",
                               "--mount", f"type=bind,source={root},target={root},readonly",
                               "--mount", f"type=bind,source={run},target={run}", self.image, *arguments],
                              capture=True, timeout=timeout)

    def build_toolchain(self):
        if self.local.production:
            self.local.run(["build", "--platform", self.local.env["DSWT_LOCAL_PLATFORM"], "--target", "build",
                            "--file", str(stack.ROOT / "server/Dockerfile"), "--tag", self.image, str(stack.ROOT)],
                           capture=True, timeout=3600)

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
        self.local.env.update(DSWT_LOCAL_MATCHERS=str(args.matchers), DSWT_LOCAL_SUBMISSION_WORKERS=workers,
                              DSWT_LOCAL_VALIDATOR_WORKERS=str(args.validator_workers or args.submission_workers))
        acceptance.require((os.cpu_count() or 0) >= args.submission_workers + 8, "too few host CPUs for this sizing")
        self.local.up()
        if self.local.production and args.matcher_replicas > 1:
            # Separate matcher processes, each with its own workers, claim from the same ledger.
            self.local.compose("up", "-d", "--no-deps", "--no-recreate", "--scale", f"matcher={args.matcher_replicas}",
                               "matcher", capture=True, timeout=120)
        if args.vps_cpus:
            # Every service shares the same two cores and a memory budget, like the production VPS.
            for service in VPS_SERVICES:
                if service in self.local.services:
                    for container in self.container_ids(service):
                        memory = VPS_MEMORY[service]
                        self.local.run(["update", "--cpuset-cpus", args.vps_cpus, "--memory", memory, "--memory-swap",
                                        memory, container], capture=True)
            if args.offsite:
                self.start_offsite()
            return
        self.local.run(["update", "--cpus", workers, self.container_id("validator")], capture=True)
        if args.constrained:
            for service, limits in CONSTRAINTS.items():
                if service in self.local.services:
                    for container in self.container_ids(service):
                        self.local.run(["update", *limits, container], capture=True)
        if args.offsite:
            self.start_offsite()

    def start_offsite(self):
        """Run the production uploader against a local directory, so every WAL file waits to be copied off."""
        project = self.local.manifest["project"]
        volume, name = project + "-offsite", project + "-uploader"
        uploader = re.search(r"^  backup-uploader:\n(?:.*\n)*?    image: (\S+)",
                             (stack.ROOT / "deploy/compose.yaml").read_text(), re.M).group(1)
        database = self.local.run(["inspect", "--format", "{{.Image}}", self.container_id("database")],
                                  capture=True).stdout.strip()
        self.local.run(["volume", "create", "--label", stack.LABEL + "=" + project, volume], capture=True)
        self.local.run(["run", "--rm", "--network", "none", "-v", volume + ":/offsite", "--entrypoint", "chown",
                        database, "999:999", "/offsite"], capture=True, timeout=60)
        self.local.run(["run", "-d", "--name", name, "--network", "none", "--read-only", "--tmpfs", "/tmp",
                        # On the VPS it shares the two cores with everything else.
                        *(["--cpuset-cpus", self.args.vps_cpus] if self.args.vps_cpus else []),
                        "--cap-drop", "ALL", "--security-opt", "no-new-privileges:true", "--user", "999:999",
                        "--memory", "128m", "--pids-limit", "64", "-e", "HOME=/tmp", "-e", "RCLONE_CACHE_DIR=/tmp",
                        "-e", "DSWT_METRICS=/tmp", "-e", "RCLONE_CONFIG=/tmp/none.conf",
                        "-e", "RCLONE_CONFIG_BACKUP_TYPE=alias", "-e", "RCLONE_CONFIG_BACKUP_REMOTE=/offsite",
                        "-v", project + "-archive:/archive", "-v", volume + ":/offsite",
                        "--mount", f"type=bind,source={stack.ROOT / 'deploy/backup-uploader.sh'},"
                                   "target=/usr/local/bin/backup-uploader.sh,readonly",
                        "--entrypoint", "/bin/sh", uploader, "/usr/local/bin/backup-uploader.sh"], capture=True, timeout=60)
        self.offsite = {"container": name, "volume": volume, "image": uploader, "database_image": database}
        self.local.compose("exec", "-T", "--user", "postgres", "database", "touch", "/archive/offsite.required",
                           capture=True, timeout=10)
        # Segments from here on can only count as archived once copied off.
        self.offsite["first_segment"] = self.runner.sql("SELECT pg_walfile_name(pg_current_wal_insert_lsn())")

    def check_offsite(self):
        """Every WAL file archived while off-site copies were required has an identical compressed copy there."""
        script = ('missing=0; total=0; for path in /archive/*; do name=${path##*/}; case $name in '
                  '????????????????????????) { [ "$name" \\< "$1" ] || [ "$name" \\> "$2" ]; } && continue; total=$((total+1)); '
                  'gunzip -c "/offsite/wal/$name.gz" 2>/dev/null | cmp -s "$path" - || missing=$((missing+1));; esac; done; '
                  'echo "$total $missing"')
        # Archiving stops at the last segment PostgreSQL finished; later ones are still open.
        last = self.runner.sql("SELECT last_archived_wal FROM pg_stat_archiver")
        output = self.local.run(["run", "--rm", "--network", "none", "--user", "999:999", "-v",
                                 self.local.manifest["project"] + "-archive:/archive:ro", "-v", self.offsite["volume"] + ":/offsite:ro",
                                 "--entrypoint", "bash", self.offsite["database_image"], "-c", script, "check",
                                 self.offsite["first_segment"], last], capture=True, timeout=600).stdout.split()
        return {"from_segment": self.offsite["first_segment"], "last_archived": last,
                "archived_segments": int(output[0]), "missing_or_different": int(output[1])}

    def stop_offsite(self):
        if self.offsite:
            self.local.compose("exec", "-T", "--user", "postgres", "database", "rm", "-f", "/archive/offsite.required",
                               capture=True, timeout=10, check=False)
            self.local.run(["rm", "-f", self.offsite["container"]], capture=True, check=False)

    def restore(self):
        """Recreate every application with compose defaults, which also clears run-specific limits."""
        for name in ("DSWT_LOCAL_MATCHERS", "DSWT_LOCAL_SUBMISSION_WORKERS", "DSWT_LOCAL_VALIDATOR_WORKERS"):
            self.local.env.pop(name, None)
        self.local.up()

    def container_ids(self, service):
        return self.local.compose("ps", "-q", service, capture=True).stdout.split()

    def container_id(self, service):
        return self.container_ids(service)[0]

    def applications(self):
        return APPLICATIONS + (("matcher",) if self.local.production else ())

    def limits(self):
        limits = {}
        for service in ("database", *self.applications()):
            for index, container in enumerate(self.container_ids(service)):
                info = json.loads(self.local.run(["inspect", container], capture=True).stdout)[0]["HostConfig"]
                limits[service + (f"-{index + 1}" if index else "")] = {
                    "nano_cpus": info["NanoCpus"], "memory": info["Memory"], "pids": info["PidsLimit"]}
        return limits

    def storage(self):
        """WAL position, archive size and database size, in bytes."""
        row = self.runner.sql("SELECT (pg_current_wal_lsn() - '0/0'::pg_lsn)::bigint || ' ' || pg_database_size('dswt_e2e') "
                              "|| ' ' || (SELECT count(*) FROM dswt.save_identities)").split()
        sample = {"at": time.time(), "wal_bytes": int(row[0]), "database_bytes": int(row[1]), "save_identities": int(row[2]),
                  "archive_bytes": None}
        if self.local.production:
            # Archiving renames temporary files while du reads; it still prints the total, then fails.
            archive = self.local.compose("exec", "-T", "--user", "postgres", "database", "du", "-sb", "/archive",
                                         capture=True, timeout=120, check=False).stdout.split()
            sample["archive_bytes"] = int(archive[0]) if archive and archive[0].isdigit() else None
        return sample

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
            self.storage_samples.append(await asyncio.to_thread(self.storage))
            if len(self.samples) % (CHECKPOINT_SECONDS // 60) == 0:
                self.checkpoints.append(await asyncio.to_thread(self.retained))

    def retained(self, sequence=None):
        """Digest acknowledged receipts and terminal (applied) ownership at a journal position; later reads must match."""
        if sequence is None:
            sequence = int(self.runner.sql("SELECT coalesce(max(sequence), 0) FROM dswt.audit_journal"))
        digest = ("SET bytea_output = 'hex'; SELECT count(*) || ' ' || encode(sha256(convert_to(coalesce(string_agg(row, "
                  "E'\\n' ORDER BY row), ''), 'UTF8')), 'hex') FROM ({}) item")
        receipts = self.runner.sql(digest.format(
            "SELECT to_jsonb(r)::text || to_jsonb(a)::text AS row FROM dswt.receipts r "
            "JOIN dswt.acknowledgements a USING (receipt_id) WHERE r.receipt_id IN (SELECT (row_key ->> "
            f"'receipt_id')::uuid FROM dswt.audit_journal WHERE table_name = 'acknowledgements' AND sequence <= {sequence})"))
        ownership = self.runner.sql(digest.format(
            "SELECT to_jsonb(o)::text AS row FROM dswt.ownership o WHERE o.asset_id IN (SELECT (row_key ->> "
            "'asset_id')::uuid FROM dswt.audit_journal WHERE table_name = 'ownership' "
            f"AND after_image ->> 'location' = '3' AND sequence <= {sequence})"))
        return {"sequence": sequence, "receipts": receipts.splitlines()[-1], "ownership": ownership.splitlines()[-1]}

    def outside(self):
        """Keep helper containers off the cores that stand in for the VPS."""
        if not self.args.vps_cpus:
            return []
        taken = set(cpu_list(self.args.vps_cpus))
        return ["--cpuset-cpus", ",".join(str(cpu) for cpu in range(os.cpu_count()) if cpu not in taken)]

    def driver(self, name, spec_path, output):
        """The client container, inside the gateway's namespace with only the repository and this run mounted."""
        root, run = str(stack.ROOT), str(self.directory)
        return [*self.local.docker, "run", "--rm", "--name", name, *self.outside(),
                "--network", "container:" + self.container_id("gateway"), "--read-only", "--tmpfs", "/tmp",
                "--cap-drop", "ALL", "--security-opt", "no-new-privileges:true", "--user", f"{os.getuid()}:{os.getgid()}",
                "--mount", f"type=bind,source={root},target={root},readonly", "--mount", f"type=bind,source={run},target={run}",
                self.image, "python3", str(Path(__file__).resolve()), "drive", str(spec_path), str(output)]

    def calibrate(self, spec, kind, count):
        """Measure what a fixed amount of fake traffic alone writes, with nothing else running."""
        label = f"calibrate-{kind}"
        started, spec_path, output = (self.directory / f"{label}-started.json", self.directory / f"{label}.json",
                                      self.directory / f"{label}-result.json")
        acceptance.save_json(spec_path, dict(spec, calibrate=kind, count=count, started=str(started)))
        name = f"{self.local.manifest['project']}-{label}-{self.directory.name[-8:]}"
        with (self.directory / f"{label}.log").open("x") as log:
            process = subprocess.Popen(self.driver(name, spec_path, output), env=self.local.env, stdout=log,
                                       stderr=subprocess.STDOUT)
            try:
                while not started.exists():
                    acceptance.require(process.poll() is None, f"{label} driver exited early")
                    time.sleep(0.5)
                before = self.storage()
                started.with_suffix(".go").touch()
                acceptance.require(process.wait(timeout=1800) == 0, f"{label} driver failed")
            finally:
                if process.poll() is None:
                    self.local.run(["rm", "-f", name], capture=True, check=False)
                    process.wait()
        if self.local.production:
            # Close the open segment, so the archive holds everything written.
            self.runner.sql("SELECT pg_switch_wal()")
            time.sleep(5)
        after = self.storage()
        result = json.loads(output.read_text())
        done = result["enrolled"] if kind == "enroll" else result["results"].get("fake_submissions", 0)
        cost = {key: after[key] - before[key] for key in ("wal_bytes", "archive_bytes", "database_bytes")
                if before[key] is not None and after[key] is not None}
        return {"count": done, "requested": count, "results": result["results"], "failures": result["failures"],
                "bytes": cost, "bytes_each": {key: round(value / done) for key, value in cost.items()} if done else None,
                "seconds": round(after["at"] - before["at"], 3)}

    async def drive(self, spec):
        """Run the driver container while restarts and sampling proceed on the host."""
        started = self.directory / "started.json"
        spec_path, output = self.directory / "spec.json", self.directory / "driver.json"
        acceptance.save_json(spec_path, dict(spec, started=str(started)))
        name = f"{self.local.manifest['project']}-load-{self.directory.name[-8:]}"
        process = await asyncio.create_subprocess_exec(
            *self.driver(name, spec_path, output),
            env=self.local.env, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.STDOUT)
        logging = asyncio.create_task(process.stdout.read())
        helpers = []
        try:
            while not started.exists():
                acceptance.require(process.returncode is None, "load driver exited before measuring")
                await asyncio.sleep(1)
            self.storage_samples.append(await asyncio.to_thread(self.storage))
            helpers = [asyncio.create_task(self.faults(json.loads(started.read_text())["started"])),
                       asyncio.create_task(self.monitor())]
            while process.returncode is None:
                await asyncio.wait([asyncio.create_task(process.wait())], timeout=5)
                for helper in helpers:
                    if helper.done() and not helper.cancelled() and helper.exception():
                        raise helper.exception()
            self.checkpoints.append(await asyncio.to_thread(self.retained))
            self.storage_samples.append(await asyncio.to_thread(self.storage))
        finally:
            for helper in helpers:
                helper.cancel()
            if process.returncode is None:
                # Never leave the client container running in the gateway namespace.
                await asyncio.to_thread(self.local.run, ["rm", "-f", name], capture=True, check=False)
                await process.wait()
            (self.directory / "driver.log").write_bytes(await logging)
        acceptance.require(process.returncode == 0, "load driver failed; see driver.log")
        return json.loads(output.read_text())

    def pair_times(self, start, end):
        """Completed pairs (both ACKs) and accepted offers by commit time, and distinct saves trading in the window."""
        rows = self.runner.rows(
            "SELECT extract(epoch FROM max(j.recorded_at))::float8 AS completed FROM dswt.matches m "
            "JOIN dswt.receipts r ON r.match_id = m.match_id JOIN dswt.audit_journal j "
            "ON j.table_name = 'acknowledgements' AND j.row_key ->> 'receipt_id' = r.receipt_id::text "
            "GROUP BY m.match_id HAVING count(*) = 2")
        offers = self.runner.rows(
            "SELECT extract(epoch FROM j.recorded_at)::float8 AS accepted FROM dswt.operations o "
            "JOIN dswt.audit_journal j ON j.table_name = 'operations' AND j.row_key ->> 'operation_id' = o.operation_id::text "
            "AND j.row_key ->> 'save_id' = o.save_id::text WHERE o.opcode = 2 AND o.result = 1")
        traders = int(self.runner.sql(
            "SELECT count(DISTINCT o.save_id) FROM dswt.operations o JOIN dswt.audit_journal j "
            "ON j.table_name = 'operations' AND j.row_key ->> 'operation_id' = o.operation_id::text "
            "AND j.row_key ->> 'save_id' = o.save_id::text WHERE o.opcode = 2 AND o.result = 1 "
            f"AND j.recorded_at >= to_timestamp({start}) AND j.recorded_at < to_timestamp({end})"))
        return [row["completed"] for row in rows], [row["accepted"] for row in offers], traders

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
            "FROM dswt.save_identities")}
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
            "no_pending_offers": self.runner.sql("SELECT count(*) FROM dswt.offers WHERE state <> 4") == "0",
            "receipts_applied_exactly": self.runner.sql(
                "SELECT count(*) FROM dswt.receipts r JOIN dswt.ownership o USING (asset_id, save_id, trade_id) "
                "JOIN dswt.payloads p USING (asset_id) LEFT JOIN dswt.acknowledgements a USING (receipt_id, save_id) "
                "WHERE o.location <> 3 OR a.applied_sha256 IS DISTINCT FROM p.sha256") == "0",
            # At least one checkpoint must exist; acknowledged receipts and applied ownership never change afterwards.
            "receipts_and_ownership_immutable": bool(self.checkpoints)
                and all(self.retained(item["sequence"]) == item for item in self.checkpoints),
            "drain_settled": results["drain_unsettled"] == 0,
        }
        self.local.compose("exec", "-T", "backend", self.local.ledger, "verify", capture=True, timeout=1200)
        checks["ledger_replay"] = True
        if self.offsite:
            self.offsite["copies"] = self.check_offsite()
            checks["offsite_copies_complete"] = (self.offsite["copies"]["archived_segments"] > 0
                                                 and self.offsite["copies"]["missing_or_different"] == 0)
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
              "stack": None, "excluded": ["matchmaking wait between polls", "WiFi/DS radio", "hosted network"],
              "client_networks": "one loopback /32 per client inside the gateway namespace",
              "targets": {"duration_seconds": SPEC["duration"], "active_clients": SPEC["clients"],
                          "completed_pairs_per_second": SPEC["pairs"], "api_p95_ms_below": SPEC["p95_ms"]},
              "profile": "production" if local.production else "device-fixture",
              "measurement_notes": PRODUCTION_NOTES if local.production else NOTES,
              "workload": {key: getattr(args, key) for key in (
                  "clients", "illegal", "illegal_interval", "offer_rate", "concurrency", "poll", "idle_poll", "matchers",
                  "matcher_replicas", "submission_workers", "restart", "restart_at", "constrained", "overload_at",
                  "overload_seconds", "overload_rate", "flood_enroll_rate", "flood_submit", "calibrate", "offsite", "validator_workers",
                  "vps_cpus")}}
    load = Load(local, directory, args)
    acceptance.require(local.production or not (args.offsite or args.matcher_replicas > 1 or "matcher" in args.restart),
                       "--offsite, --matcher-replicas and matcher restarts need the production profile")
    try:
        local.volumes()
        local.database_identity()
        load.runner.build()  # Generates the strict legal/illegal fixtures with the pinned validator image.
        load.build_toolchain()
        probe = load.build_probe()
        report["native_sha256"] = acceptance.sha(probe)
        report["fixtures"], report["fixture_image"] = load.runner.fixture_manifest, load.runner.fixture_image
        load.configure(args)
        # Snapshot the measured stack; restore() later rewrites the live manifest.
        report["stack"] = json.loads(json.dumps(local.manifest))
        report["images"] = {service: local.run(["inspect", "--format", "{{.Image}}", load.container_id(service)],
                                               capture=True).stdout.strip() for service in ("database", *load.applications())}
        report["matcher_containers"] = len(load.container_ids("matcher")) if local.production else 0
        report["limits"] = load.limits()
        clients_directory = directory / "clients"
        clients_directory.mkdir(mode=0o700)
        fixtures = load.runner.fixtures
        clients = [{"path": str(clients_directory / f"c{index:04d}"), "legal": True, "source": source(index),
                    "fixture": str(fixtures / ("starter.pk4" if index % 2 else "wild.pk4"))} for index in range(args.clients)]
        clients += [{"path": str(clients_directory / f"x{index:02d}"), "legal": False, "source": source(args.clients + index),
                     "fixture": str(fixtures / "illegal-rng.pk4")} for index in range(args.illegal)]
        profile = load.runner.profile
        flood_directory = directory / "fake"
        flood_directory.mkdir(mode=0o700)
        acceptance.require(args.illegal == 0 or not local.production,
                           "use --flood-submit for strict-illegal traffic: retained native clients cannot yet restore a "
                           "rejection (the probe lacks WT_INVENTORY_PREPARE_RESTORE)")
        spec = {"probe": str(probe), "pins": [profile["current_spki_sha256"], profile["next_spki_sha256"]],
                "clients": clients, "flood_directory": str(flood_directory), "flood_fixture": str(fixtures / "starter.pk4"),
                "flood_illegal_fixture": str(fixtures / "illegal-rng.pk4"), "flood_submit": args.flood_submit,
                **{key: getattr(args, key) for key in
                   ("duration", "offer_rate", "concurrency", "poll", "idle_poll", "match_timeout", "illegal_interval",
                    "overload_at", "overload_seconds", "overload_rate", "flood_enroll_rate")}}
        calibration = None
        if args.calibrate:
            # Alone first, so the cost of each fake request is not mixed with trades.
            enroll = load.calibrate(dict(spec, clients=[], flood_offset=50_000, flood_submit=False), "enroll", args.calibrate)
            submit = load.calibrate(dict(spec, clients=[], flood_offset=55_000, flood_submit=True), "submit", args.calibrate)
            each = (enroll["bytes_each"] or {})
            per_enrollment = max(each.get("wal_bytes", 0), each.get("archive_bytes") or 0)
            if submit["count"]:
                # The submitting saves also enrolled; what is left is the rejected submission's own cost.
                submit["rejected_bytes_each"] = {key: round((value - each.get(key, 0) * submit["count"]) / submit["count"])
                                                 for key, value in submit["bytes"].items()}
            calibration = {"enroll": enroll, "enroll_and_rejected_submit": submit, "bytes_per_enrollment": per_enrollment,
                           "production_enrollments_per_day": PRODUCTION_ENROLLMENTS_PER_HOUR * 24,
                           "production_cap_bytes_per_day": per_enrollment * PRODUCTION_ENROLLMENTS_PER_HOUR * 24,
                           "budget_bytes_per_day": WAL_BUDGET_BYTES_PER_DAY}
        results = asyncio.run(load.drive(spec))
        start, end = results["window"]
        pairs, offers, traders = load.pair_times(start, end)
        latency = {op: summarize(values) for op, values in sorted(results["latency"].items())}
        latency["all"] = summarize([value for values in results["latency"].values() for value in values])
        def grouped(values):
            summary = {op: summarize(items) for op, items in sorted(values.items())}
            summary["all"] = summarize([item for items in values.values() for item in items])
            return summary
        report["measured"] = {
            "window": [start, end], "window_seconds": round(end - start, 3),
            "active_clients": {"configured": args.clients, "exchanging": results["clients_exchanging"], "trading": traders},
            "completed_pairs": rates(pairs, start, end), "accepted_offers": rates(offers, start, end),
            "latency": latency, "gateway_latency": load.server_latency("gateway", start, end),
            "backend_latency": load.server_latency("backend", start, end),
            **{key: results[key] for key in ("results", "failures", "legal_submissions", "legal_cycles_completed",
                                             "drain_unsettled")},
            "restarts": load.events, "resource_samples": load.samples, "resource_peaks": peaks(load.samples),
            "error_rate": error_rate(results["latency"], {key: value for key, value in results["failures"].items()
                                                          if not key.startswith("fake:")}),
            "storage_samples": load.storage_samples,
            "storage_growth": growth(load.storage_samples[0] if load.storage_samples else None,
                                     load.storage_samples[-1] if load.storage_samples else None, end - start),
        }
        if args.overload_rate:
            report["measured"]["overload"] = {
                "offered_pairs_per_second": args.overload_rate, "at": args.overload_at, "seconds": args.overload_seconds,
                "grace_seconds": OVERLOAD_GRACE, "latency": grouped(results["overload_latency"]),
                "completed_pairs": rates(pairs, start + args.overload_at, start + args.overload_at + args.overload_seconds)}
        if args.flood_enroll_rate or args.illegal or args.calibrate:
            report["fake_traffic"] = {
                "enroll_rate": args.flood_enroll_rate, "enrollments": results["fake_enrollments"],
                "submissions": results["fake_results"].get("fake_submissions", 0), "results": results["fake_results"],
                "failures": {key: value for key, value in results["failures"].items() if key.startswith("fake:")},
                "latency": grouped(results["fake_latency"]), "calibration": calibration}
        if load.offsite:
            report["offsite"] = load.offsite
        report["conservation"] = load.verify(results, args.clients, load.runner.fixture_manifest["sha256"])
        report["verdict"] = verdict(report)
        report["status"] = report["verdict"]["status"]
    except Exception as error:
        report.update(status="fail", failure=type(error).__name__ + ": " + str(error))
        raise
    finally:
        try:
            load.stop_offsite()
            load.restore()
        except (OSError, subprocess.SubprocessError, ValueError) as error:
            report.setdefault("cleanup_failures", []).append(f"restore default backend/gateway settings manually ({error})")
        acceptance.save_json(directory / "report.json", report)
        print("Load evidence: " + str(directory / "report.json"), flush=True)
    return 0 if report["status"] == "pass" else 1


def drive_main(spec_path, output):
    spec = json.loads(Path(spec_path).read_text())
    driver = Driver(spec)
    results = asyncio.run(driver.calibrate() if spec.get("calibrate") else driver.run(Path(spec["started"])))
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
    parser.add_argument("--illegal-interval", type=float, help="mean seconds between an illegal client's submissions "
                        "(default --idle-poll)")
    parser.add_argument("--validator-workers", type=int, choices=range(1, 9),
                        help="isolated validator workers (default --submission-workers)")
    parser.add_argument("--vps-cpus", help="run every service on these host cores (such as 2,3) with the production "
                        "VPS's memory budget, instead of the per-service limits")
    parser.add_argument("--matcher-replicas", type=int, default=1, choices=range(1, 5),
                        help="production: separate matcher containers, each with --matchers workers")
    parser.add_argument("--overload-at", type=float, default=0, help="seconds into the window to start overloading")
    parser.add_argument("--overload-seconds", type=float, default=0)
    parser.add_argument("--overload-rate", type=float, default=0, help="offered pairs/s while overloading (0: none)")
    parser.add_argument("--flood-enroll-rate", type=float, default=0,
                        help="fake saves enrolling per second from their own networks, never trading")
    parser.add_argument("--flood-submit", action="store_true",
                        help="each fake save also submits a strict-illegal Pokémon once before it is thrown away")
    parser.add_argument("--calibrate", type=int, default=0,
                        help="first measure the WAL and disk cost of this many fake enrollments and rejected submissions")
    parser.add_argument("--offsite", action="store_true",
                        help="production: run the backup uploader into a local directory and require each WAL copy")
    parser.add_argument("--restart", nargs="*", default=["backend", "validator", "database"])
    parser.add_argument("--restart-at", nargs="*", type=float, default=[900, 1800, 2700])
    parser.add_argument("--unconstrained", dest="constrained", action="store_false",
                        help="skip the local CPU/memory limits on the gateway and backend")
    args = parser.parse_args()
    if len(args.restart) != len(args.restart_at) or not set(args.restart) <= set(RESTARTABLE):
        parser.error("give one --restart-at offset per --restart service: " + ", ".join(RESTARTABLE))
    if args.vps_cpus and (len(cpu_list(args.vps_cpus)) != 2 or max(cpu_list(args.vps_cpus)) >= (os.cpu_count() or 0)):
        parser.error("--vps-cpus takes two host cores, such as 2,3")
    if bool(args.overload_rate) != bool(args.overload_seconds) or (args.overload_rate and args.overload_rate <= args.pairs):
        parser.error("give --overload-rate above --pairs together with --overload-seconds")
    if not 0 < args.clients + args.illegal <= MAX_CLIENTS:
        parser.error(f"use 1-{MAX_CLIENTS} clients")
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
