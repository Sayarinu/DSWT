import copy
import io
import json
from pathlib import Path
import subprocess
import sys
import tarfile
from types import SimpleNamespace
import unittest


DIRECTORY = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(DIRECTORY))
import isolation
import policy


ROOT = DIRECTORY.parents[1]
SETTINGS = json.loads((ROOT / ".github/repository-settings.json").read_text())
WORKFLOW = """on: [pull_request, push]
permissions: {contents: read}
jobs:
  test:
    runs-on: ubuntu-24.04
    steps:
      - uses: actions/checkout@1111111111111111111111111111111111111111
        with: {persist-credentials: false}
      - run: python3 -m unittest
"""


class WorkflowTests(unittest.TestCase):
    def test_github_on_and_false_remain_strings_and_real_policy_matches(self):
        data = policy.yaml.load(WORKFLOW, Loader=policy.WorkflowLoader)
        self.assertIn("on", data)
        self.assertEqual(policy.workflow(data), ["test"])
        self.assertEqual(policy.audit(ROOT)["status"], "pass")

    def test_unsafe_permissions_triggers_runners_actions_and_secrets_fail(self):
        changes = [
            lambda d: d.update(permissions="write-all"),
            lambda d: d.update(permissions={"contents": "write"}),
            lambda d: d.update(permissions={"id-token": "write"}),
            lambda d: d.pop("permissions"),
            lambda d: d["jobs"]["test"].update(permissions={"packages": "write"}),
            lambda d: d["jobs"]["test"].update(permissions={"id-token": "write"}),
            lambda d: d.update(on=["pull_request_target"]),
            lambda d: d.update(on=["workflow_run"]),
            lambda d: d["jobs"]["test"].update({"runs-on": "self-hosted"}),
            lambda d: d["jobs"]["test"].update({"runs-on": ["self-hosted", "linux"]}),
            lambda d: d["jobs"]["test"].update({"runs-on": "${{ matrix.runner }}"}),
            lambda d: d["jobs"]["test"].update(secrets="inherit"),
            lambda d: d["jobs"]["test"].update(uses="./.github/workflows/privileged.yml"),
            lambda d: d["jobs"]["test"].update(container={"options": "--privileged"}),
            lambda d: d.update(env={"TOKEN": "${{ secrets.PRODUCTION_TOKEN }}"}),
            lambda d: d.update(env={"TOKEN": "${{ format('{0}', secrets['PRODUCTION_TOKEN']) }}"}),
            lambda d: d.update(env={"TOKEN": "${{ (github['token']) }}"}),
            lambda d: d.update(env={"TOKEN": "${{ toJSON(secrets) }}"}),
            lambda d: d.update(env={"TOKEN": "${{ toJSON(github) }}"}),
            lambda d: d.update(env={"TOKEN": "${{ toJSON(github.*) }}"}),
            lambda d: d.update(env={"TOKEN": "${{ github[format('{0}{1}', 'to', 'ken')] }}"}),
            lambda d: d.update(env={"TOKEN": "${{ format('}}{0}', secrets.PRODUCTION_TOKEN) }}"}),
            lambda d: d.update(env={"TOKEN": "${{ format('}}{0}', github.token) }}"}),
            lambda d: d.update(env={"TOKEN": "${{ format('''}}{0}', github.token) }}"}),
            lambda d: d["jobs"]["test"]["steps"][0].update(uses="actions/checkout@v4"),
            lambda d: d["jobs"]["test"]["steps"][0]["with"].update({"persist-credentials": "true"}),
            lambda d: d["jobs"]["test"]["steps"][0].pop("with"),
            lambda d: d["jobs"]["test"]["steps"][0].update({"with": "invalid"}),
        ]
        for index, change in enumerate(changes):
            with self.subTest(case=index):
                data = policy.yaml.load(WORKFLOW, Loader=policy.WorkflowLoader)
                change(data)
                with self.assertRaises(ValueError):
                    policy.workflow(data)

    def test_duplicate_merged_and_recursive_yaml_are_refused(self):
        for text in (WORKFLOW + "permissions: {contents: write}\n", "jobs: {<<: {test: unsafe}}", "jobs: {test: &job {copy: *job}}"):
            with self.subTest(text=text), self.assertRaises(ValueError):
                policy.yaml.load(text, Loader=policy.WorkflowLoader)
        data = policy.yaml.load(WORKFLOW, Loader=policy.WorkflowLoader)
        data["loop"] = data
        with self.assertRaisesRegex(ValueError, "Recursive"):
            policy.workflow(data)


class GithubTests(unittest.TestCase):
    def responses(self):
        result = {"": dict(SETTINGS["repository"], private=True),
                  "/actions/permissions/workflow": SETTINGS["actions"],
                  "/actions/permissions/fork-pr-workflows-private-repos": SETTINGS["private_fork_workflows"],
                  "/branches/main/protection": copy.deepcopy(SETTINGS["main_protection"])}
        for name in ("enforce_admins", "required_conversation_resolution", "allow_force_pushes", "allow_deletions"):
            result["/branches/main/protection"][name] = {"enabled": result["/branches/main/protection"][name]}
        return result

    def run_with(self, responses, errors=None):
        def run(arguments, **kwargs):
            self.assertEqual(arguments[:2], ["gh", "api"])
            self.assertEqual(arguments[2:4], ["--hostname", "github.com"])
            self.assertEqual(len(arguments), 5)
            suffix = arguments[4].removeprefix("repos/Sayarinu/PlatinumWonderTrade")
            if errors and suffix in errors:
                return SimpleNamespace(returncode=1, stdout="", stderr="gh: access (HTTP " + str(errors[suffix]) + ")")
            return SimpleNamespace(returncode=0, stdout=json.dumps(responses[suffix]), stderr="")
        return policy.github(SETTINGS, "Sayarinu/PlatinumWonderTrade", run)

    def test_private_public_fork_policy_is_explicitly_not_applicable(self):
        evidence = self.run_with(self.responses())
        self.assertEqual(isolation.github_status(evidence), "pass")
        self.assertEqual(evidence["public_fork_approval"]["status"], "not_applicable")

    def test_denied_unavailable_and_mismatch_are_distinct_and_incomplete(self):
        evidence = self.run_with(self.responses(), {"/actions/permissions/workflow": 403, "/branches/main/protection": 404})
        self.assertEqual(evidence["actions"]["status"], "denied")
        self.assertEqual(evidence["main_protection"]["status"], "unavailable")
        responses = self.responses()
        responses["/actions/permissions/workflow"] = {"default_workflow_permissions": "write"}
        evidence = self.run_with(responses)
        self.assertEqual(evidence["actions"]["status"], "mismatch")
        self.assertEqual(isolation.github_status(evidence), "incomplete")

    def test_missing_client_and_timeout_are_unavailable(self):
        for error in (FileNotFoundError(), subprocess.TimeoutExpired("gh", 30)):
            def fail(*args, **kwargs):
                raise error
            evidence = policy.github(SETTINGS, "Sayarinu/PlatinumWonderTrade", fail)
            self.assertTrue(all(value["status"] == "unavailable" for value in evidence.values()))


class RuntimeTests(unittest.TestCase):
    def setUp(self):
        self.local = SimpleNamespace(workspace=Path("/private/run"), manifest={"project": "platinumwt-local-case", "images": {}})
        self.containers = {}
        gateway = "a" * 64
        for index, name in enumerate(isolation.stack.SERVICES):
            image = "sha256:" + str(index) * 64
            self.local.manifest["images"][name] = image
            mounts = []
            for target, (kind, source, rw) in isolation.volume_contract(self.local, name).items():
                mounts.append({"Destination": target, "Type": kind, "Name": source, "Source": source, "RW": rw})
            self.containers[name] = {
                "Id": gateway if name == "gateway" else str(index) * 64, "Image": image, "Mounts": mounts,
                "State": {"Running": True},
                "Config": {"User": "1000:1000", "Labels": {"com.docker.compose.project": "platinumwt-local-case", "com.docker.compose.service": name}},
                "HostConfig": {"Privileged": False, "CapAdd": [], "Devices": [], "PidMode": "", "IpcMode": "private",
                               "PortBindings": {"8443/tcp": [{"HostIp": "127.0.0.1", "HostPort": "8443"}]} if name == "gateway" else {},
                               "ReadonlyRootfs": name != "database", "CapDrop": ["ALL"], "SecurityOpt": ["no-new-privileges:true"],
                               "Tmpfs": {"/tmp": "rw,noexec,nosuid,nodev,size=64m" if name == "validator" else "rw"},
                               "NetworkMode": "none" if name == "database" else "platinumwt-local-case_default" if name == "gateway" else "container:" + gateway,
                               "Memory": 512 * 1024 * 1024, "NanoCpus": 2_000_000_000, "PidsLimit": 128}}

    def test_runtime_is_bound_to_owned_images_network_mounts_and_limits(self):
        self.assertEqual(set(isolation.runtime_evidence(self.local, self.containers)), set(isolation.stack.SERVICES))
        changes = [
            lambda d: d["gateway"]["HostConfig"].update(Privileged=True),
            lambda d: d["gateway"]["HostConfig"].update(CapAdd=["SYS_ADMIN"]),
            lambda d: d["gateway"]["HostConfig"].update(ReadonlyRootfs=False),
            lambda d: d["gateway"]["HostConfig"].update(NetworkMode="host"),
            lambda d: d["backend"]["HostConfig"].update(NetworkMode="bridge"),
            lambda d: d["gateway"]["HostConfig"]["PortBindings"]["8443/tcp"][0].update(HostIp="0.0.0.0"),
            lambda d: d["gateway"]["Mounts"][0].update(RW=True),
            lambda d: d["gateway"]["Mounts"].append({"Destination": "/var/run/docker.sock", "Type": "bind", "Source": "/var/run/docker.sock", "RW": False}),
            lambda d: d["database"]["Mounts"][0].update(Name="foreign-data"),
            lambda d: d["validator"]["HostConfig"].update(PidsLimit=0),
            lambda d: d["validator"]["HostConfig"].update(NanoCpus=0),
            lambda d: d["validator"]["HostConfig"].update(Memory=0),
            lambda d: d["validator"]["HostConfig"].update(Tmpfs={"/tmp": "rw,exec,suid,dev,size=4g"}),
            lambda d: d["validator"]["HostConfig"].update(Tmpfs={"/tmp": "rw,noexec,nosuid,nodev,size=4g"}),
            lambda d: d["backend"]["Config"].update(User="root"),
            lambda d: d["backend"]["Config"].update(User="root:1000"),
            lambda d: d["backend"]["Config"].update(User="0000:1000"),
            lambda d: d["backend"].update(Image="different-image"),
            lambda d: d["backend"]["Config"]["Labels"].update({"com.docker.compose.project": "foreign"}),
        ]
        for index, change in enumerate(changes):
            with self.subTest(case=index):
                mutated = copy.deepcopy(self.containers)
                change(mutated)
                with self.assertRaises(ValueError):
                    isolation.runtime_evidence(self.local, mutated)

    def test_rendered_compose_rejects_public_ports_networking_and_foreign_mounts(self):
        config = {"services": {}, "volumes": {key: {"name": "platinumwt-local-case-" + key, "external": True} for key in ("data", "socket")}}
        for name in (*isolation.stack.SERVICES, "migrate"):
            volumes = []
            for target, (kind, source, rw) in isolation.volume_contract(self.local, name).items():
                volumes.append({"target": target, "type": kind, "source": source.rsplit("-", 1)[-1] if kind == "volume" else source, "read_only": not rw})
            config["services"][name] = {
                "volumes": volumes, "ports": [{"host_ip": "127.0.0.1", "published": "8443", "target": 8443}] if name == "gateway" else [],
                "read_only": name != "database", "cap_drop": ["ALL"], "security_opt": ["no-new-privileges:true"],
                "tmpfs": ["/tmp:rw,noexec,nosuid,nodev,size=64m"] if name == "validator" else ["/tmp"],
                "mem_limit": 512 * 1024 * 1024, "cpus": 2, "pids_limit": 128}
            if name in ("database", "migrate"):
                config["services"][name]["network_mode"] = "none"
            elif name in ("backend", "validator"):
                config["services"][name]["network_mode"] = "service:gateway"
        config["services"]["database"].update(image="postgres:16.15@sha256:" + "1" * 64,
            command=["postgres", "-c", "listen_addresses=", "-c", "fsync=on", "-c", "full_page_writes=on", "-c", "synchronous_commit=on"])
        self.assertEqual(isolation.compose_evidence(self.local, config)["status"], "pass")
        for change in (lambda d: d["services"]["gateway"]["ports"][0].update(host_ip="0.0.0.0"),
                       lambda d: d["services"]["database"].update(network_mode="host"),
                       lambda d: d["services"]["backend"].update(network_mode="bridge"),
                       lambda d: d["services"]["gateway"]["volumes"][0].update(source="/var/run/docker.sock"),
                       lambda d: d["volumes"]["data"].update(name="foreign-data"),
                       lambda d: d["services"]["database"]["command"].append("listen_addresses=*"),
                       lambda d: d["services"]["validator"].update(mem_limit=0),
                       lambda d: d["services"]["validator"].update(tmpfs=["/tmp:rw,exec,suid,dev,size=4g"])):
            mutated = copy.deepcopy(config)
            change(mutated)
            with self.subTest(change=change), self.assertRaises(ValueError):
                isolation.compose_evidence(self.local, mutated)

    def test_grpc_must_be_loopback_and_database_must_not_listen_on_tcp(self):
        def row(address, port):
            return "0: " + address + ":" + format(port, "04X") + " 00000000:0000 0A 0 0\n"
        gateway = "".join(row("0100007F", port) for port in (50051, 50052, 50053, 50054)) + row("00000000", 8443)
        for shared, database, passes in ((gateway, "", True),
                                         (gateway.replace("0100007F", "0000000000000000FFFF00000100007F"), "", True),
                                         (gateway.replace("0100007F", "00000000"), "", False),
                                         (gateway.replace("0100007F", "0000000000000000FFFF00000100000A"), "", False),
                                         (gateway, row("00000000", 5432), False),
                                         (gateway + row("00000000", 12345), "", False)):
            self.local.compose = lambda *args, **kwargs: SimpleNamespace(stdout=shared if "gateway" in args else database)
            if passes:
                self.assertEqual(isolation.listener_evidence(self.local)["status"], "pass")
            else:
                with self.assertRaises(ValueError):
                    isolation.listener_evidence(self.local)


class ProductionTests(unittest.TestCase):
    def archive(self, extra=None, omit=None):
        stream = io.BytesIO()
        with tarfile.open(fileobj=stream, mode="w") as archive:
            for name in ("usr/local/bin/platinumwt-server", "usr/local/bin/platinumwt-backend", "usr/local/bin/platinumwt-ledger",
                         "usr/share/platinumwt/LICENSE", "usr/share/platinumwt/NOTICE", extra):
                if name and name != omit:
                    info = tarfile.TarInfo(name)
                    info.size = 1
                    archive.addfile(info, io.BytesIO(b"x"))
        stream.seek(0)
        return tarfile.open(fileobj=stream)

    def test_production_inspection_rejects_build_trees_fixtures_and_missing_binaries(self):
        with self.archive() as archive:
            self.assertEqual(isolation.production_files(archive)["status"], "pass")
        for extra in ("build/test", "src/server.cpp", "test/run", "usr/local/bin/platinumwt-device-fixture",
                      "usr/local/bin/platinumwt-gateway-fixture", "usr/local/bin/platinumwt-unit-tests", "../unsafe"):
            with self.subTest(extra=extra), self.archive(extra) as archive, self.assertRaises(ValueError):
                isolation.production_files(archive)
        with self.archive(omit="usr/local/bin/platinumwt-backend") as archive, self.assertRaises(ValueError):
            isolation.production_files(archive)


if __name__ == "__main__":
    unittest.main()
