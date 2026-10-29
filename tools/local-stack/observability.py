#!/usr/bin/env python3
"""Verify the private local observability profile and capture real alert firing and resolution."""

import argparse
import base64
import fcntl
import json
import os
from pathlib import Path
import secrets
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid

import isolation
import policy
import stack

require = policy.require
CONFIGS = ("prometheus.yml", "rules.yml", "alertmanager.yml", "loki.yaml", "config.alloy", "capture.py",
           "grafana/datasources/datasources.yaml", "grafana/dashboards/provider.yaml", "grafana/dashboards/platinumwt.json")
LOG_FIELDS = {"event", "service", "timestamp_ms", "operation", "result", "duration_us",
              "request_id", "operation_id", "trade_id"}
CAPTURE = "http://127.0.0.1:9099/alerts"
BACKUP_METRIC = "platinumwt_backup_last_success_timestamp_seconds"
SCRAPE_JOBS = {"gateway", "backend", "host", "collectors"}


def write_backup_timestamp(workspace, timestamp):
    """Atomically publish the textfile metric that local backup tooling maintains."""
    directory = workspace / "observability/textfile"
    temporary = directory / f".backup.prom.{uuid.uuid4().hex}"
    with temporary.open("x") as output:
        output.write(f"# TYPE {BACKUP_METRIC} gauge\n{BACKUP_METRIC} {timestamp:.0f}\n")
        output.flush()
        os.fsync(output.fileno())
    os.chmod(temporary, 0o644)
    os.replace(temporary, directory / "backup.prom")


def notifications(path, offset):
    """Return (alertname, labels, status, receiver, end offset) for each complete captured delivery after offset."""
    events = []
    if not path.exists():
        return events
    with path.open("rb") as source:
        source.seek(offset)
        for line in source:
            if not line.endswith(b"\n"):
                break  # still being appended; read it on the next poll
            offset += len(line)
            payload = json.loads(line)
            for alert in payload.get("alerts", []):
                events.append((alert["labels"].get("alertname"), alert["labels"], alert["status"], payload.get("receiver"), offset))
    return events


class Observer:
    def __init__(self, local):
        self.local = local
        require(local.observability, "Workspace was not initialized with --observability")
        self.capture = local.workspace / "observability/alerts/received.jsonl"
        self.password = (local.workspace / "observability/grafana-admin-password").read_text().strip()

    def internal(self, url, timeout=15, attempts=3):
        """GET an endpoint that exists only inside the private gateway namespace, retrying briefly while collectors start."""
        for attempt in range(attempts):
            try:
                return self.local.compose("exec", "-T", "prometheus", "wget", "-T", str(timeout - 5), "-qO-", url,
                                          capture=True, timeout=timeout).stdout
            except subprocess.SubprocessError:
                if attempt == attempts - 1:
                    raise
                time.sleep(3)

    def grafana(self, path, authenticated=True):
        request = urllib.request.Request(f"http://127.0.0.1:{self.local.observability['grafana_port']}{path}")
        if authenticated:
            request.add_header("Authorization", "Basic " + base64.b64encode(f"admin:{self.password}".encode()).decode())
        try:
            with urllib.request.urlopen(request, timeout=10) as response:
                return response.status, json.loads(response.read())
        except urllib.error.HTTPError as error:
            return error.code, None

    def configuration(self):
        flags = json.loads(self.internal("http://127.0.0.1:9090/api/v1/status/flags"))["data"]
        require(flags["storage.tsdb.retention.time"] == "15d", "Prometheus must retain metrics for 15 days")
        deadline = time.monotonic() + 120  # freshly started collectors (notably Loki) need time to become ready
        while True:
            targets = json.loads(self.internal("http://127.0.0.1:9090/api/v1/targets"))["data"]["activeTargets"]
            health = {(t["labels"]["job"], t["labels"]["instance"]): t["health"] for t in targets}
            jobs = {job for job, _ in health}
            # An empty target list right after a restart must not count as healthy.
            if (jobs == SCRAPE_JOBS and all(value == "up" for value in health.values())) or time.monotonic() >= deadline:
                break
            time.sleep(2)
        unhealthy = sorted(f"{job} {instance} {value}" for (job, instance), value in health.items() if value != "up")
        require(jobs == SCRAPE_JOBS and not unhealthy,
                "Every scrape target must be up: " + ", ".join(unhealthy))
        loki = policy.yaml.safe_load(self.internal("http://127.0.0.1:3100/config"))
        loki_retention = loki["limits_config"]["retention_period"]
        require(loki_retention in ("1w", "7d", "168h"), "Loki must retain logs for 7 days")
        require(loki["compactor"]["retention_enabled"] is True, "Loki compactor retention must be enabled")
        # The API masks webhook URLs, so bind the loaded receiver shape to the reviewed source file.
        loaded = policy.yaml.safe_load(json.loads(self.internal("http://127.0.0.1:9093/api/v2/status"))["config"]["original"])
        source = policy.yaml.safe_load((stack.OBSERVABILITY_CONFIG / "alertmanager.yml").read_text())
        shapes = [{key for key, value in receiver.items() if key.endswith("_configs") and value} for receiver in loaded["receivers"]]
        urls = [hook["url"] for receiver in source["receivers"] for hook in receiver.get("webhook_configs", [])]
        require(shapes == [{"webhook_configs"}] and len(loaded["receivers"][0]["webhook_configs"]) == 1
                and urls == [CAPTURE] and loaded["route"]["receiver"] == "local-capture" and not loaded["route"].get("routes"),
                "Alertmanager may deliver only to the local capture receiver")
        code, _ = self.grafana("/api/datasources", authenticated=False)
        require(code == 401, "Grafana must refuse unauthenticated access")
        code, sources = self.grafana("/api/datasources")
        require(code == 200 and sorted(item["uid"] for item in sources) == ["alertmanager", "loki", "prometheus"],
                "Grafana must provision only the local data sources")
        code, dashboard = self.grafana("/api/dashboards/uid/platinumwt-local")
        require(code == 200, "Provisioned dashboard is missing")
        gateway = self.local.compose("ps", "-q", "gateway", capture=True, timeout=10).stdout.strip()
        bindings = json.loads(self.local.run(["inspect", "--format", "{{json .HostConfig.PortBindings}}", gateway],
                                             capture=True, timeout=10).stdout)
        require(all(item["HostIp"] == "127.0.0.1" for values in bindings.values() for item in values),
                "Administrative and ingest ports must be loopback-only")
        return {"prometheus_retention": flags["storage.tsdb.retention.time"], "loki_retention": loki_retention,
                "targets": sorted(f"{job} {instance}" for job, instance in health), "alert_receivers": urls,
                "grafana_unauthenticated": 401, "grafana_datasources": sorted(item["uid"] for item in sources),
                "published": {key: [item["HostIp"] + ":" + item["HostPort"] for item in value] for key, value in bindings.items()}}

    def logs(self, timeout=90):
        """Send one uniquely identified request and find its redacted structured line in Loki."""
        request_id = secrets.token_bytes(16)
        self.local.ready(request_id)
        query = urllib.parse.urlencode({"query": f'{{service="gateway"}} |= "{request_id.hex()}"', "since": "15m", "limit": "10"})
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            result = json.loads(self.internal("http://127.0.0.1:3100/loki/api/v1/query_range?" + query))["data"]["result"]
            lines = [value[1] for stream in result for value in stream["values"]]
            if lines:
                record = json.loads(lines[0])
                require(set(record) <= LOG_FIELDS and record["request_id"] == request_id.hex()
                        and record["event"] == "trade_request", "Indexed log line has unexpected fields")
                return {"request_id": request_id.hex(), "seconds_to_index": round(timeout - (deadline - time.monotonic()), 1),
                        "labels": result[0]["stream"], "fields": sorted(record)}
            time.sleep(3)
        raise RuntimeError("Gateway request log was not searchable in Loki")

    def await_alert(self, offset, name, status, match=None, timeout=180):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for alertname, labels, state, receiver, end in notifications(self.capture, offset):
                if alertname == name and state == status and all(labels.get(k) == v for k, v in (match or {}).items()):
                    return {"alert": name, "status": status, "labels": labels, "receiver": receiver,
                            "seconds": round(timeout - (deadline - time.monotonic()), 1)}, end
            time.sleep(2)
        raise RuntimeError(f"Did not capture {name} {status} within {timeout}s")

    def inactive(self, name, timeout=180):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            alerts = json.loads(self.internal("http://127.0.0.1:9090/api/v1/alerts"))["data"]["alerts"]
            if not any(alert["labels"]["alertname"] == name for alert in alerts):
                return
            time.sleep(2)
        raise RuntimeError(f"{name} did not clear before its scenario")

    def scenario(self, name, induce, restore, match=None):
        self.inactive(name)
        offset = self.capture.stat().st_size if self.capture.exists() else 0
        induced = time.time()
        induce()
        firing, position = self.await_alert(offset, name, "firing", match)
        restore()
        # Resolution must be a later delivery than the captured firing for this scenario.
        resolved, _ = self.await_alert(position, name, "resolved", match)
        return {"alert": name, "induced_at": induced, "firing": firing, "resolved": resolved}

    def alerts(self):
        workspace = self.local.workspace
        compose = self.local.compose
        results = []
        results.append(self.scenario("ServiceDown", lambda: compose("stop", "backend", capture=True, timeout=60),
                                     lambda: compose("start", "backend", capture=True, timeout=60), {"job": "backend"}))
        results.append(self.scenario("LedgerDatabaseUnavailable",
                                     lambda: compose("stop", "database", capture=True, timeout=60),
                                     lambda: compose("start", "database", capture=True, timeout=60)))
        # The real backup tool owns backup.prom; preserve whatever it recorded (or its absence) afterwards.
        textfile = workspace / "observability/textfile/backup.prom"
        prior = textfile.read_bytes() if textfile.exists() else None
        try:
            write_backup_timestamp(workspace, time.time())
            results.append(self.scenario("BackupStale", lambda: write_backup_timestamp(workspace, time.time() - 2 * 86400),
                                         lambda: write_backup_timestamp(workspace, time.time())))
        finally:
            failing = sys.exc_info()[1] is not None
            try:
                if prior is None:
                    textfile.unlink(missing_ok=True)
                else:
                    temporary = textfile.with_name(".backup.prom.restore")
                    temporary.write_bytes(prior)
                    os.chmod(temporary, 0o644)
                    os.replace(temporary, textfile)
            except OSError:
                if not failing:  # never mask the scenario's own failure
                    raise
        fill = "import os; f=os.statvfs('/monitored/pressure'); open('/monitored/pressure/fill','wb').write(b'\\0'*int(f.f_bavail*f.f_frsize*0.95))"
        results.append(self.scenario(
            "FilesystemPressure",
            lambda: compose("exec", "-T", "capture", "python3", "-c", fill, capture=True, timeout=60),
            lambda: compose("exec", "-T", "capture", "rm", "-f", "/monitored/pressure/fill", capture=True, timeout=60),
            {"mountpoint": "/monitored/pressure"}))
        return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace", type=Path, required=True)
    parser.add_argument("action", choices=("verify",))
    args = parser.parse_args()
    os.umask(0o077)
    report = {"version": 1, "evidence_layer": "local-observability", "status": "running", "hosted": "not_run",
              "external_notifications": "none configured"}
    directory = None
    try:
        workspace = stack.workspace_path(args.workspace)
        stack.read_manifest(workspace)
        descriptor = os.open(workspace / ".lock", os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600)
        with os.fdopen(descriptor, "a+") as lock:
            stack.private_file(workspace / ".lock")
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            local = stack.Stack(workspace, stack.read_manifest(workspace))
            observer = Observer(local)
            local.volumes()
            local.database_identity()
            local.ready()
            parent = workspace / "observability/evidence"
            parent.mkdir(mode=0o700, exist_ok=True)
            require(not parent.is_symlink(), "Linked evidence directory")
            directory = parent / (time.strftime("%Y%m%dT%H%M%S") + "-" + uuid.uuid4().hex[:8])
            directory.mkdir(mode=0o700)
            report.update(source_commit=local.manifest["source_commit"], source_dirty=local.manifest["source_dirty"],
                          images=local.manifest["images"], database_system_id=local.manifest["database_system_id"],
                          configs={name: isolation.digest(stack.OBSERVABILITY_CONFIG / name) for name in CONFIGS},
                          compose={path.name: isolation.digest(path) for path in (stack.COMPOSE, stack.OBSERVABILITY)})
            report["configuration"] = observer.configuration()
            report["logs"] = observer.logs()
            report["alerts"] = observer.alerts()
            local.database_identity()
            local.ready()
            local.compose("exec", "-T", "backend", "/build/platinumwt-ledger", "verify", capture=True, timeout=60)
            report["ledger_replay_after"] = "pass"
        report["status"] = "pass"
    except (OSError, ValueError, KeyError, TypeError, RuntimeError, subprocess.SubprocessError, policy.yaml.YAMLError) as error:
        report.update(status="fail", failure=type(error).__name__ + ": " + str(error))
    finally:
        if directory:
            with (directory / "report.json").open("x") as output:
                json.dump(report, output, indent=2, sort_keys=True)
                output.write("\n")
            print("Private observability evidence: " + str(directory / "report.json"), file=sys.stderr)
        print(json.dumps(report, indent=2, sort_keys=True))
    return 0 if report["status"] == "pass" else 1


if __name__ == "__main__":
    sys.exit(main())
