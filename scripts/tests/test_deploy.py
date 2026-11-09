"""Check how deploy/dswt.py treats credentials, host keys and the pinned certificate, without a server."""

import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("dswt_deploy", ROOT / "deploy/dswt.py")
dswt = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(dswt)
REAL_RUN = subprocess.run


class Stop(Exception):
    pass


def certificate(directory, name):
    key, pem = directory / f"{name}.key", directory / f"{name}.pem"
    REAL_RUN(["openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256", "-nodes",
              "-keyout", str(key), "-out", str(pem), "-days", "1", "-subj", "/CN=test"],
             check=True, capture_output=True)
    return dswt.pin(key), pem.read_text()


class DeployTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.keys = tempfile.TemporaryDirectory()
        directory = Path(cls.keys.name)
        cls.pin, cls.pem = certificate(directory, "current")
        cls.other_pin, cls.other_pem = certificate(directory, "other")

    @classmethod
    def tearDownClass(cls):
        cls.keys.cleanup()

    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.local = Path(temporary.name) / "game/dswt-production"
        (self.local / "tls").mkdir(parents=True)
        profile = {"hostname": "example.test", "ipv4": "192.0.2.1", "port": 443, "current_spki_sha256": self.pin}
        (self.local / "tls/connection.json").write_text(json.dumps(profile))
        patcher = mock.patch.object(dswt, "LOCAL", self.local)
        patcher.start()
        self.addCleanup(patcher.stop)

    def target(self, served=None, **options):
        target = dswt.Target(Path("/keys/deploy"), **options)
        def run(script, capture=False, check=True):
            if script.startswith("cat "):
                return SimpleNamespace(returncode=0 if served else 1, stdout=served or "")
            return SimpleNamespace(returncode=0, stdout="")
        target.run = mock.Mock(side_effect=run)
        return target

    def test_known_hosts_makes_host_keys_strict_and_default_is_unchanged(self):
        default = dswt.Target(Path("/keys/deploy"))
        self.assertEqual(default.ssh, ["ssh", "-i", "/keys/deploy", "-o", "IdentitiesOnly=yes", "-o", "BatchMode=yes"])
        strict = dswt.Target(Path("/keys/deploy"), Path("/run/known_hosts")).ssh
        self.assertEqual(strict[:len(default.ssh)], default.ssh)
        self.assertIn("StrictHostKeyChecking=yes", strict)
        self.assertIn("UserKnownHostsFile=/run/known_hosts", strict)
        self.assertIn("GlobalKnownHostsFile=/dev/null", strict)

    def test_ssh_host_changes_only_the_destination_and_keeps_the_host_key(self):
        default = dswt.Target(Path("/keys/deploy"), Path("/run/known_hosts"))
        self.assertEqual(default.address, "dswt@192.0.2.1")
        self.assertFalse([option for option in default.ssh if option.startswith("HostKeyAlias")])
        routed = dswt.Target(Path("/keys/deploy"), Path("/run/known_hosts"), "dswt-server.example.ts.net")
        self.assertEqual(routed.address, "dswt@dswt-server.example.ts.net")
        options = routed.ssh[:]
        alias = options.index("HostKeyAlias=192.0.2.1")
        self.assertEqual(options.pop(alias - 1), "-o")
        options.remove("HostKeyAlias=192.0.2.1")
        self.assertEqual(options, default.ssh)
        self.assertEqual(routed.profile["ipv4"], "192.0.2.1")
        self.assertEqual(dswt.Target(Path("/keys/deploy"), None, "100.64.0.7").address, "dswt@100.64.0.7")
        for host in ("-oProxyCommand=true", "host name", "host;true", "user@host", "fd7a:115c:a1e0::1"):
            with self.subTest(host=host), self.assertRaises(ValueError):
                dswt.Target(Path("/keys/deploy"), None, host)

    def test_ssh_host_comes_from_the_flag_or_the_environment(self):
        for arguments, environment, expected in ((["--ssh-host", "100.64.0.7"], {}, "100.64.0.7"),
                                                 ([], {"DSWT_SSH_HOST": "dswt-server"}, "dswt-server"),
                                                 ([], {}, None)):
            with self.subTest(arguments=arguments), mock.patch.dict(os.environ, environment, clear=True), \
                    mock.patch("sys.argv", ["dswt.py", "verify", *arguments]), \
                    mock.patch.object(dswt, "Target", side_effect=Stop) as target:
                with self.assertRaises(Stop):
                    dswt.main()
                self.assertEqual(target.call_args.args[2], expected)

    def test_served_certificate_must_exist_and_match_the_profile(self):
        self.assertEqual(self.target(self.pem).served_certificate(), self.pem)
        with self.assertRaisesRegex(RuntimeError, "no TLS certificate"):
            self.target(None).served_certificate()
        with self.assertRaisesRegex(RuntimeError, "does not match"):
            self.target(self.other_pem).served_certificate()

    def sync_calls(self, target):
        """The commands sync runs up to and including its certificate check."""
        calls = []
        def run(arguments, **options):
            calls.append(arguments)
            return SimpleNamespace(returncode=0, stdout="")
        with mock.patch.object(dswt.subprocess, "run", side_effect=run), \
                mock.patch.object(target, "served_certificate", side_effect=Stop):
            with self.assertRaises(Stop):
                target.sync("0123456789ab")
        return calls

    def test_sync_uploads_local_tls_files_without_overwriting(self):
        (self.local / "tls/current.key").write_text("key")
        (self.local / "tls/current.pem").write_text(self.pem)
        tls = [call for call in self.sync_calls(self.target(self.pem)) if str(self.local / "tls/current.key") in call]
        self.assertEqual(len(tls), 1)
        self.assertIn("--ignore-existing", tls[0])
        self.assertIn(str(self.local / "tls/current.pem"), tls[0])

    def test_sync_without_local_tls_files_uploads_none_and_still_checks_the_pin(self):
        calls = self.sync_calls(self.target(self.pem))
        self.assertFalse([call for call in calls if any(str(self.local / "tls") in str(part) for part in call)])
        self.assertTrue(calls)

    def test_sync_refuses_half_of_the_local_tls_pair(self):
        (self.local / "tls/current.pem").write_text(self.pem)
        with self.assertRaisesRegex(ValueError, "together"), mock.patch.object(dswt.subprocess, "run"):
            self.target(self.pem).sync("0123456789ab")

    def test_ready_trusts_the_local_certificate_or_else_the_verified_served_one(self):
        served = self.target(self.pem)
        self.assertEqual(served.pinned_certificate(), self.pem)
        self.assertEqual(served.pinned_certificate(), self.pem)
        self.assertEqual(served.run.call_count, 1)
        with self.assertRaisesRegex(RuntimeError, "does not match"):
            self.target(self.other_pem).pinned_certificate()
        (self.local / "tls/current.pem").write_text(self.other_pem)
        local = self.target(None)
        self.assertEqual(local.pinned_certificate(), self.other_pem)
        local.run.assert_not_called()

    def pull(self, environment):
        calls = []
        def check_output(arguments, **options):
            calls.append((arguments, options))
            if arguments[:3] == ["gh", "auth", "token"]:
                return "gh-token\n"
            return "unix:///var/run/docker.sock\n" if arguments[1] == "context" else "digest\n"
        def run(arguments, **options):
            calls.append((arguments, options))
            return SimpleNamespace(returncode=0)
        with mock.patch.dict(os.environ, environment, clear=True), \
                mock.patch.object(dswt.subprocess, "check_output", side_effect=check_output), \
                mock.patch.object(dswt.subprocess, "run", side_effect=run), mock.patch("builtins.print"):
            dswt.pull_images("0123456789ab")
        return calls

    def test_registry_token_comes_from_the_environment_without_gh(self):
        calls = self.pull({"DSWT_REGISTRY_TOKEN": "ci-token", "GITHUB_ACTOR": "owner", "PATH": os.environ.get("PATH", "")})
        self.assertFalse([arguments for arguments, _ in calls if arguments[0] == "gh"])
        login = [(arguments, options) for arguments, options in calls if "login" in arguments]
        self.assertEqual(len(login), 1)
        arguments, options = login[0]
        self.assertEqual(options["input"], "ci-token")
        self.assertEqual(arguments[arguments.index("--username") + 1], "owner")
        for arguments, options in calls:
            self.assertNotIn("ci-token", " ".join(arguments))
            self.assertNotIn("DSWT_REGISTRY_TOKEN", options.get("env", {}))

    def test_registry_token_falls_back_to_gh(self):
        calls = self.pull({"PATH": os.environ.get("PATH", "")})
        self.assertIn(["gh", "auth", "token"], [arguments for arguments, _ in calls])
        login = [(arguments, options) for arguments, options in calls if "login" in arguments][0]
        self.assertEqual(login[1]["input"], "gh-token")
        self.assertEqual(login[0][login[0].index("--username") + 1], "token")


if __name__ == "__main__":
    unittest.main()
