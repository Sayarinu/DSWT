import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

DIRECTORY = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(DIRECTORY))
import observability
import policy


def delivery(name, status, **labels):
    return json.dumps({"receiver": "local-capture", "status": status,
                       "alerts": [{"status": status, "labels": dict(labels, alertname=name)}]}) + "\n"


class CaptureTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.workspace = Path(temporary.name)
        (self.workspace / "observability/alerts").mkdir(parents=True)
        (self.workspace / "observability/textfile").mkdir()
        (self.workspace / "observability/grafana-admin-password").write_text("secret")
        self.local = SimpleNamespace(workspace=self.workspace, observability={"grafana_port": 13000, "log_port": 24224})
        self.observer = observability.Observer(self.local)

    def test_resolution_must_follow_the_captured_firing(self):
        capture = self.observer.capture
        capture.write_text(delivery("BackupStale", "resolved") + delivery("BackupStale", "firing"))
        with patch.object(observability.time, "sleep"):
            firing, position = self.observer.await_alert(0, "BackupStale", "firing", timeout=1)
            self.assertEqual(position, capture.stat().st_size)
            # The earlier resolution (from a previous condition) must not satisfy this scenario.
            with self.assertRaises(RuntimeError):
                self.observer.await_alert(position, "BackupStale", "resolved", timeout=0.01)
            with capture.open("a") as output:
                output.write(delivery("ServiceDown", "resolved", job="gateway") + delivery("BackupStale", "resolved"))
            resolved, _ = self.observer.await_alert(position, "BackupStale", "resolved", timeout=1)
            self.assertEqual(resolved["receiver"], "local-capture")
            with self.assertRaises(RuntimeError):
                self.observer.await_alert(0, "ServiceDown", "resolved", {"job": "backend"}, timeout=0.01)

    def test_backup_metric_is_published_atomically(self):
        observability.write_backup_timestamp(self.workspace, 1234.4)
        directory = self.workspace / "observability/textfile"
        self.assertEqual([path.name for path in directory.iterdir()], ["backup.prom"])
        self.assertIn(f"{observability.BACKUP_METRIC} 1234\n", (directory / "backup.prom").read_text())

    def test_only_the_local_capture_receiver_is_accepted(self):
        source = policy.yaml.safe_load((DIRECTORY / "observability/alertmanager.yml").read_text())
        self.assertEqual([hook["url"] for r in source["receivers"] for hook in r["webhook_configs"]], [observability.CAPTURE])
        loaded = {"route": {"receiver": "local-capture"},
                  "receivers": [{"name": "local-capture", "webhook_configs": [{"url": "<secret>"}]}]}
        flags = {"data": {"storage.tsdb.retention.time": "15d"}}
        targets = {"data": {"activeTargets": [{"labels": {"job": job, "instance": "127.0.0.1:1"}, "health": "up"}
                                              for job in ("gateway", "backend", "host", "collectors")]}}

        def internal(config):
            def fetch(url, timeout=20):
                if "flags" in url:
                    return json.dumps(flags)
                if "targets" in url:
                    return json.dumps(targets)
                if ":3100" in url:
                    return "retention_period: 1w\nretention_enabled: true\n"
                return json.dumps({"config": {"original": policy.yaml.safe_dump(config)}})
            return fetch

        external = dict(loaded, receivers=[dict(loaded["receivers"][0], email_configs=[{"to": "owner@example.invalid"}])])
        extra_route = dict(loaded, route={"receiver": "local-capture", "routes": [{"receiver": "pager"}]})
        for config in (external, extra_route):
            with self.subTest(config=config), patch.object(self.observer, "internal", internal(config)), \
                    self.assertRaisesRegex(ValueError, "local capture"):
                self.observer.configuration()
        responses = iter([(200, [])])
        with patch.object(self.observer, "internal", internal(loaded)), \
                patch.object(self.observer, "grafana", side_effect=lambda *a, **k: next(responses)), \
                self.assertRaisesRegex(ValueError, "unauthenticated"):
            self.observer.configuration()


if __name__ == "__main__":
    unittest.main()
