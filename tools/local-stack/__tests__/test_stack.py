import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch


SCRIPT = Path(__file__).resolve().parents[1] / "stack.py"
spec = importlib.util.spec_from_file_location("local_stack", SCRIPT)
stack = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stack)


class StackTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="local stack tests ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        root = patch.object(stack, "ROOT", self.root)
        root.start()
        self.addCleanup(root.stop)
        self.workspace = self.root / "game/run one"
        self.calls = []
        self.volumes = {}

        def prepare(directory, ipv4, hostname, port):
            directory.mkdir(mode=0o700)
            for name in ("current", "next", "untrusted"):
                key = directory / (name + ".key")
                key.write_text("private synthetic key")
                key.chmod(0o600)
        with patch.object(stack, "load_module", return_value=SimpleNamespace(prepare=prepare)):
            stack.initialize(self.workspace)
        self.manifest = stack.read_manifest(self.workspace)

    def fake_command(self, arguments, **kwargs):
        self.calls.append((arguments, kwargs))
        if arguments == ["docker", "context", "inspect"]:
            output = json.dumps([{"Endpoints": {"docker": {"Host": "unix:///local/docker.sock"}}}])
        elif "info" in arguments:
            output = "linux/aarch64\n"
        elif "volume" in arguments:
            action = arguments[arguments.index("volume") + 1]
            if action == "ls":
                output = "\n".join(json.dumps({"Name": name}) for name in self.volumes)
            elif action == "create":
                self.volumes[arguments[-1]] = {stack.LABEL: self.manifest["project"]}
                output = arguments[-1]
            else:
                output = json.dumps([{"Labels": self.volumes[arguments[-1]]}])
        elif "pg_control_system()" in arguments[-1]:
            output = "123456789\n"
        elif "ps" in arguments:
            output = "fake-container\n"
        elif "inspect" in arguments:
            output = "sha256:" + "1" * 64
        elif arguments[:1] == ["git"]:
            output = "1" * 40 if "rev-parse" in arguments else ""
        else:
            output = ""
        return SimpleNamespace(stdout=output, returncode=0)

    def make_stack(self):
        return stack.Stack(self.workspace, self.manifest)

    def test_init_is_exclusive_and_paths_reject_aliases(self):
        original = (self.workspace / "manifest.json").read_bytes()
        with self.assertRaises(FileExistsError):
            stack.initialize(self.workspace)
        alias = self.root / "game/alias"
        alias.symlink_to(self.workspace, target_is_directory=True)
        for path in (alias, alias / "child", self.root / "outside", self.root / "game"):
            with self.subTest(path=path), self.assertRaises(ValueError):
                stack.workspace_path(path)
        self.assertEqual(stack.workspace_path(self.workspace), self.workspace)
        self.assertEqual((self.workspace / "manifest.json").read_bytes(), original)

    def test_manifest_and_private_keys_reject_links_and_public_permissions(self):
        key = self.workspace / "tls/current.key"
        key.chmod(0o644)
        with self.assertRaises(ValueError):
            stack.read_manifest(self.workspace)
        key.chmod(0o600)
        os.link(key, self.workspace / "another.key")
        with self.assertRaises(ValueError):
            stack.read_manifest(self.workspace)
        (self.workspace / "another.key").unlink()
        control = self.workspace / "control"
        (control / "run").rmdir()
        control.rmdir()
        control.symlink_to(self.workspace / "tls", target_is_directory=True)
        with self.assertRaises(ValueError):
            stack.read_manifest(self.workspace)

    def test_manifest_cannot_be_rebound_to_another_directory_or_project(self):
        for key, value in (("workspace", "/another/workspace"), ("project", "another-project"),
                           ("profile", "../elsewhere"), ("version", 2)):
            with self.subTest(key=key):
                stack.save_manifest(self.workspace, dict(self.manifest, **{key: value}))
                with self.assertRaises(ValueError):
                    stack.read_manifest(self.workspace)

    def test_remote_docker_context_and_host_are_refused(self):
        for endpoint in ("tcp://192.0.2.1:2375", "ssh://remote.invalid", "http://remote.invalid"):
            with self.subTest(endpoint=endpoint), patch.dict(os.environ, {"DOCKER_HOST": endpoint}, clear=True), \
                    patch.object(stack, "command") as command:
                with self.assertRaisesRegex(ValueError, "remote"):
                    self.make_stack()
                command.assert_not_called()
        remote = [{"Endpoints": {"docker": {"Host": "ssh://remote.invalid"}}}]
        with patch.dict(os.environ, {}, clear=True), \
                patch.object(stack, "command", return_value=SimpleNamespace(stdout=json.dumps(remote))):
            with self.assertRaisesRegex(ValueError, "remote"):
                self.make_stack()

    def test_explicit_context_compose_platform_and_environment(self):
        inherited = {"COMPOSE_FILE": "/wrong.yaml", "COMPOSE_PROJECT_NAME": "production", "COMPOSE_PROFILES": "all",
                     "PGHOST": "remote.invalid", "PGSERVICE": "production", "DATABASE_URL": "private",
                     "PLATINUMWT_DATABASE_URL": "private", "DOCKER_DEFAULT_PLATFORM": "linux/amd64"}
        with patch.dict(os.environ, inherited, clear=True), patch.object(stack, "command", side_effect=self.fake_command):
            local = self.make_stack()
            local.compose("down")
        arguments, options = self.calls[-1]
        self.assertEqual(arguments[:4], ["docker", "--host", "unix:///local/docker.sock", "compose"])
        self.assertEqual(arguments[arguments.index("--project-name") + 1], self.manifest["project"])
        self.assertEqual(arguments[arguments.index("--file") + 1], str(stack.COMPOSE))
        self.assertEqual(arguments[-1], "down")
        self.assertNotIn("--volumes", arguments)
        self.assertEqual(options["env"]["PWT_LOCAL_PLATFORM"], "linux/arm64")
        for key in inherited:
            self.assertNotIn(key, options["env"])

    def test_volume_claim_is_recorded_before_start_and_missing_state_is_refused(self):
        with patch.dict(os.environ, {}, clear=True), patch.object(stack, "command", side_effect=self.fake_command):
            local = self.make_stack()
            local.volumes(create=True)
            self.assertTrue(stack.read_manifest(self.workspace)["volumes_created"])
            self.assertEqual(len(self.volumes), 2)
            self.assertFalse(any("up" in args for args, _ in self.calls))
            self.volumes.pop(self.manifest["project"] + "-data")
            self.calls.clear()
            with self.assertRaisesRegex(ValueError, "refusing"):
                local.up()
            self.assertFalse(any("create" in args or "build" in args for args, _ in self.calls))

    def test_foreign_volume_and_replaced_database_are_refused(self):
        name = self.manifest["project"] + "-data"
        self.volumes[name] = {stack.LABEL: "foreign"}
        with patch.dict(os.environ, {}, clear=True), patch.object(stack, "command", side_effect=self.fake_command):
            local = self.make_stack()
            with self.assertRaisesRegex(ValueError, "ownership"):
                local.volumes(create=True)
            self.manifest["database_system_id"] = "98765"
            with self.assertRaisesRegex(ValueError, "identity changed"):
                local.database_identity()

    def test_up_preserves_manifest_identity_and_waits_for_real_readiness(self):
        with patch.dict(os.environ, {}, clear=True), patch.object(stack, "command", side_effect=self.fake_command):
            local = self.make_stack()
            with patch.object(local, "ready", side_effect=[OSError("not ready"), None]) as ready, \
                    patch.object(stack.time, "sleep"):
                local.up()
                self.assertEqual(ready.call_count, 2)
            first = stack.read_manifest(self.workspace)
            self.assertEqual(first["database_system_id"], "123456789")
            self.assertEqual(set(first["images"]), set(stack.SERVICES))
            with patch.object(local, "ready"):
                local.up()
            self.assertEqual(stack.read_manifest(self.workspace)["project"], first["project"])
            arguments = [args for args, _ in self.calls]
            migration = next(i for i, args in enumerate(arguments) if "migrate" in args)
            start = next(i for i, args in enumerate(arguments) if "--force-recreate" in args)
            self.assertLess(migration, start)
            self.assertFalse(any("--volumes" in args or "rm" in args for args in arguments))


if __name__ == "__main__":
    unittest.main()
