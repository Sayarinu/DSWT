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

    def test_publisher_may_only_write_packages_by_hand_from_main(self):
        real = policy.yaml.load((ROOT / ".github/workflows/publish-images.yml").read_text(), Loader=policy.WorkflowLoader)
        policy.publisher(real)
        job = lambda d: d["jobs"]["publish"]
        changes = [
            lambda d: d.update(on="pull_request"),
            lambda d: d.update(on=["push", "workflow_dispatch"]),
            lambda d: d.update(on="workflow_dispatch"),
            lambda d: d["on"].update(push=""),
            lambda d: d["on"].update(workflow_call={"secrets": {"TOKEN": {"required": "true"}}}),
            lambda d: d.update(permissions={"contents": "write"}),
            lambda d: job(d).pop("if"),
            lambda d: job(d).update({"if": "always()"}),
            lambda d: job(d).update(permissions={"contents": "write", "packages": "write"}),
            lambda d: job(d).update(permissions={"contents": "read", "packages": "write", "id-token": "write"}),
            lambda d: job(d).update({"runs-on": "self-hosted"}),
            lambda d: job(d).update(environment="production"),
            lambda d: job(d)["steps"][0].update(uses="actions/checkout@v4"),
            lambda d: job(d)["steps"].append({"run": "true", "env": {"KEY": "${{ secrets.PRODUCTION_TOKEN }}"}}),
            lambda d: job(d)["steps"].append({"run": "true", "env": {"OTHER": "${{ github.token }}"}}),
            lambda d: job(d)["steps"].append({"run": "true", "env": {"GH_TOKEN": "${{ secrets.PERSONAL_TOKEN }}"}}),
            lambda d: d["jobs"].update(second=copy.deepcopy(job(d))),
            lambda d: job(d)["steps"].insert(1, {"name": "Join the tailnet", "uses": policy.TAILSCALE,
                                                 "with": {"tags": "tag:ci"}}),
        ]
        for index, change in enumerate(changes):
            with self.subTest(case=index):
                data = copy.deepcopy(real)
                change(data)
                with self.assertRaises(ValueError):
                    policy.publisher(data)
        data = policy.yaml.load(WORKFLOW, Loader=policy.WorkflowLoader)
        data["jobs"]["test"]["steps"].append({"uses": policy.TAILSCALE, "with": {"tags": "tag:ci"}})
        with self.assertRaisesRegex(ValueError, "tailnet"):
            policy.workflow(data)

    def test_releaser_may_only_write_releases_by_hand_from_main(self):
        real = policy.yaml.load((ROOT / ".github/workflows/release.yml").read_text(), Loader=policy.WorkflowLoader)
        policy.releaser(real)
        jobs = lambda d: d["jobs"]
        changes = [
            lambda d: d.update(on="push"),
            lambda d: d.update(on={"workflow_dispatch": "", "push": ""}),
            lambda d: d["on"]["workflow_dispatch"]["inputs"].update(token={"required": "true"}),
            lambda d: d.update(permissions={"contents": "write"}),
            lambda d: jobs(d)["fetch"].pop("if"),
            lambda d: jobs(d)["publish"].update({"if": "always()"}),
            lambda d: jobs(d)["publish"].update(permissions={"contents": "write", "packages": "write"}),
            lambda d: jobs(d)["fetch"].update(permissions={"contents": "write", "id-token": "write"}),
            lambda d: jobs(d)["linux"].update(permissions={"contents": "write"}),
            lambda d: jobs(d)["windows"].pop("needs"),
            lambda d: jobs(d)["publish"].update(needs=["linux", "macos"]),
            lambda d: jobs(d).update(extra=copy.deepcopy(jobs(d)["linux"])),
            lambda d: jobs(d).pop("macos"),
            lambda d: jobs(d)["macos"].update({"runs-on": "self-hosted"}),
            lambda d: jobs(d)["linux"]["steps"][-2].setdefault("env", {}).update(GH_TOKEN="${{ github.token }}"),
            lambda d: jobs(d)["publish"]["steps"].append({"uses": "actions/github-script@" + "1" * 40,
                                                          "env": {"GH_TOKEN": "${{ github.token }}"}}),
            lambda d: jobs(d)["publish"]["steps"].append({"run": "true", "env": {"KEY": "${{ secrets.PERSONAL_TOKEN }}"}}),
            lambda d: jobs(d)["fetch"]["steps"][0].update(uses="actions/checkout@v4"),
        ]
        for index, change in enumerate(changes):
            with self.subTest(case=index):
                data = copy.deepcopy(real)
                change(data)
                with self.assertRaises(ValueError):
                    policy.releaser(data)

    def test_deployer_may_only_read_listed_secrets_in_production_from_main(self):
        real = policy.yaml.load((ROOT / ".github/workflows/deploy.yml").read_text(), Loader=policy.WorkflowLoader)
        policy.deployer(real)
        job = lambda d: d["jobs"]["deploy"]
        step = lambda d, name: next(item for item in job(d)["steps"] if item.get("name") == name)
        changes = [
            lambda d: d["on"].update(pull_request=""),
            lambda d: d["on"].update(pull_request={"paths": ["server/**"]}),
            lambda d: d["on"].update(pull_request_target=""),
            lambda d: d["on"]["push"].pop("paths"),
            lambda d: d["on"]["push"].update(paths=["**"]),
            lambda d: d["on"]["push"]["paths"].append("client/**"),
            lambda d: d["on"]["push"]["paths"].remove("server/**"),
            lambda d: d["on"]["push"].update({"paths-ignore": ["docs/**"]}),
            lambda d: d.update(on=["push", "workflow_dispatch"]),
            lambda d: d["on"].update(push={"branches": ["main", "release"]}),
            lambda d: d["on"].update(push=""),
            lambda d: d["on"].update(workflow_run={"workflows": ["Validate source inputs"]}),
            lambda d: d.pop("concurrency"),
            lambda d: d.update(permissions={"contents": "write"}),
            lambda d: job(d).pop("environment"),
            lambda d: job(d).update(environment="staging"),
            lambda d: job(d).update(environment={"name": "production"}),
            lambda d: job(d).pop("needs"),
            lambda d: job(d).pop("concurrency"),
            lambda d: job(d)["concurrency"].update({"cancel-in-progress": "true"}),
            lambda d: job(d).pop("if"),
            lambda d: job(d).update({"if": "github.ref == 'refs/heads/main'"}),
            lambda d: job(d).update(permissions={"contents": "read", "packages": "write"}),
            lambda d: job(d).update(permissions={"contents": "write", "packages": "read"}),
            lambda d: job(d).update(permissions={"contents": "read", "packages": "read", "id-token": "write"}),
            lambda d: job(d).pop("permissions"),
            lambda d: job(d).update({"runs-on": "self-hosted"}),
            lambda d: job(d).update(secrets="inherit"),
            lambda d: job(d).update(env={"KEY": "${{ secrets.DSWT_SSH_KEY }}"}),
            lambda d: step(d, "Write deploy credentials")["env"].update(EXTRA="${{ secrets.HETZNER_API_TOKEN }}"),
            lambda d: step(d, "Write deploy credentials")["env"].update(HETZNER_API_TOKEN="${{ secrets.HETZNER_API_TOKEN }}"),
            lambda d: step(d, "Write deploy credentials")["env"].update(DSWT_SSH_KEY="${{ secrets.DSWT_BACKUPS_KEY }}"),
            lambda d: step(d, "Write deploy credentials")["env"].update(DSWT_SSH_KEY="${{ toJSON(secrets) }}"),
            lambda d: step(d, "Write deploy credentials").update(run="echo '${{ secrets.DSWT_SSH_KEY }}'"),
            lambda d: step(d, "Deploy")["env"].update(OTHER="${{ github.token }}"),
            lambda d: job(d)["steps"].insert(1, {"uses": "someone/action@" + "1" * 40, "env": {"DSWT_SSH_KEY": "${{ secrets.DSWT_SSH_KEY }}"}}),
            lambda d: job(d)["steps"].insert(1, {"uses": "someone/action@" + "1" * 40}),
            lambda d: job(d)["steps"][0].update(uses="actions/checkout@v4"),
            lambda d: job(d)["steps"][0]["with"].update({"persist-credentials": "true"}),
            lambda d: step(d, "Join the tailnet").update(uses="tailscale/github-action@v4"),
            lambda d: step(d, "Join the tailnet").update(uses="tailscale/github-action@" + "2" * 40),
            lambda d: step(d, "Join the tailnet").update(uses="someone/tailscale-action@" + "2" * 40),
            lambda d: step(d, "Join the tailnet")["with"].update(tags="tag:ci,tag:admin"),
            lambda d: step(d, "Join the tailnet")["with"].update(args="--accept-routes"),
            lambda d: step(d, "Join the tailnet")["with"].update(authkey="${{ secrets.TS_AUTHKEY }}"),
            lambda d: step(d, "Join the tailnet")["with"].update({"oauth-secret": "${{ secrets.DSWT_SSH_KEY }}"}),
            lambda d: step(d, "Join the tailnet").update(env={"TS_API_KEY": "${{ secrets.TS_API_KEY }}"}),
            lambda d: step(d, "Join the tailnet").update({"if": "always()"}),
            lambda d: job(d)["steps"].remove(step(d, "Join the tailnet")),
            lambda d: job(d)["steps"].append(copy.deepcopy(step(d, "Join the tailnet"))),
            lambda d: job(d)["steps"].insert(0, job(d)["steps"].pop(job(d)["steps"].index(step(d, "Write deploy credentials")))),
            lambda d: step(d, "Deploy")["env"].update(DSWT_SSH_HOST="${{ secrets.DSWT_TAILNET_HOST }}"),
            lambda d: step(d, "Deploy")["env"].update(TS_OAUTH_SECRET="${{ secrets.TS_OAUTH_SECRET }}"),
            lambda d: d["jobs"]["publish"].update(secrets="inherit"),
            lambda d: d["jobs"]["publish"].update(uses="./.github/workflows/other.yml"),
            lambda d: d["jobs"]["publish"].update(permissions={"contents": "write", "packages": "write"}),
            lambda d: d["jobs"]["publish"].pop("if"),
            lambda d: d["jobs"].update(second=copy.deepcopy(job(d))),
        ]
        for index, change in enumerate(changes):
            with self.subTest(case=index):
                data = copy.deepcopy(real)
                change(data)
                with self.assertRaises(ValueError):
                    policy.deployer(data)

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
            suffix = arguments[4].removeprefix("repos/Sayarinu/DSWT")
            if errors and suffix in errors:
                return SimpleNamespace(returncode=1, stdout="", stderr="gh: access (HTTP " + str(errors[suffix]) + ")")
            return SimpleNamespace(returncode=0, stdout=json.dumps(responses[suffix]), stderr="")
        return policy.github(SETTINGS, "Sayarinu/DSWT", run)

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
            evidence = policy.github(SETTINGS, "Sayarinu/DSWT", fail)
            self.assertTrue(all(value["status"] == "unavailable" for value in evidence.values()))


class RuntimeTests(unittest.TestCase):
    observability = None

    def setUp(self):
        manifest = {"project": "dswt-local-case", "images": {}}
        if self.observability:
            manifest.update(https_port=18443, observability=self.observability)
        services = isolation.stack.SERVICES + (isolation.stack.OBSERVABILITY_SERVICES if self.observability else ())
        self.local = SimpleNamespace(workspace=Path("/private/run"), manifest=manifest, services=services,
                                     observability=self.observability)
        bindings = {f"{target}/tcp": [{"HostIp": address, "HostPort": port}] for address, port, target in isolation.published(self.local)}
        self.containers = {}
        gateway = "a" * 64
        for index, name in enumerate(services):
            image = "sha256:" + str(index) * 64
            self.local.manifest["images"][name] = image
            mounts = []
            for target, (kind, source, rw) in isolation.volume_contract(self.local, name).items():
                mounts.append({"Destination": target, "Type": kind, "Name": source, "Source": source, "RW": rw})
            self.containers[name] = {
                "Id": gateway if name == "gateway" else str(index) * 64, "Image": image, "Mounts": mounts,
                "State": {"Running": True},
                "Config": {"User": "1000:1000", "Labels": {"com.docker.compose.project": "dswt-local-case", "com.docker.compose.service": name}},
                "HostConfig": {"Privileged": False, "CapAdd": [], "Devices": [], "PidMode": "", "IpcMode": "private",
                               "PortBindings": copy.deepcopy(bindings) if name == "gateway" else {},
                               "ReadonlyRootfs": name != "database", "CapDrop": ["ALL"], "SecurityOpt": ["no-new-privileges:true"],
                               "Tmpfs": {"/tmp": "rw,noexec,nosuid,nodev,size=64m" if name == "validator" else "rw"},
                               "NetworkMode": "none" if name == "database" else "dswt-local-case_default" if name == "gateway" else "container:" + gateway,
                               "Memory": 512 * 1024 * 1024, "NanoCpus": 2_000_000_000, "PidsLimit": 128}}

    def test_runtime_is_bound_to_owned_images_network_mounts_and_limits(self):
        self.assertEqual(set(isolation.runtime_evidence(self.local, self.containers)), set(self.local.services))
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

    def compose(self):
        volumes = ("data", "socket") + (isolation.stack.OBSERVABILITY_VOLUMES if self.observability else ())
        config = {"services": {}, "volumes": {key: {"name": "dswt-local-case-" + key, "external": True} for key in volumes}}
        ports = [{"host_ip": address, "published": port, "target": target} for address, port, target in sorted(isolation.published(self.local))]
        for name in (*self.local.services, "migrate"):
            volumes = []
            for target, (kind, source, rw) in isolation.volume_contract(self.local, name).items():
                volumes.append({"target": target, "type": kind, "source": source.rsplit("-", 1)[-1] if kind == "volume" else source, "read_only": not rw})
            config["services"][name] = {
                "volumes": volumes, "ports": copy.deepcopy(ports) if name == "gateway" else [],
                "read_only": name != "database", "cap_drop": ["ALL"], "security_opt": ["no-new-privileges:true"],
                "tmpfs": ["/tmp:rw,noexec,nosuid,nodev,size=64m"] if name == "validator" else ["/tmp"],
                "mem_limit": 512 * 1024 * 1024, "cpus": 2, "pids_limit": 128}
            if name in ("database", "migrate"):
                config["services"][name]["network_mode"] = "none"
            elif name != "gateway":
                config["services"][name]["network_mode"] = "service:gateway"
        config["services"]["database"].update(image="postgres:16.15@sha256:" + "1" * 64,
            command=["postgres", "-c", "listen_addresses=", "-c", "fsync=on", "-c", "full_page_writes=on", "-c", "synchronous_commit=on"])
        config["services"]["gateway"]["environment"] = {"WT_METRICS_PORT": "9091"}
        config["services"]["backend"]["environment"] = {"WT_METRICS_PORT": "9092"}
        return config

    def test_rendered_compose_rejects_public_ports_networking_and_foreign_mounts(self):
        config = self.compose()
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

    def test_logs_may_only_reach_the_owned_local_collector(self):
        config = self.compose()
        config["services"]["backend"]["logging"] = {"driver": "fluentd", "options": {"fluentd-address": "tcp://127.0.0.1:24224"}}
        mutated = copy.deepcopy(self.containers)
        mutated["backend"]["HostConfig"]["LogConfig"] = {"Type": "fluentd", "Config": {"fluentd-address": "tcp://127.0.0.1:24224"}}
        if self.observability:
            self.assertEqual(isolation.compose_evidence(self.local, config)["status"], "pass")
            self.assertIn("backend", isolation.runtime_evidence(self.local, mutated))
        for address in ("tcp://192.0.2.1:24224", "tcp://127.0.0.1:1"):
            config["services"]["backend"]["logging"]["options"]["fluentd-address"] = address
            mutated["backend"]["HostConfig"]["LogConfig"]["Config"]["fluentd-address"] = address
            with self.subTest(address=address), self.assertRaises(ValueError):
                isolation.compose_evidence(self.local, config)
            with self.subTest(address=address), self.assertRaises(ValueError):
                isolation.runtime_evidence(self.local, mutated)
        mutated["backend"]["HostConfig"]["LogConfig"] = {"Type": "syslog", "Config": {}}
        with self.assertRaises(ValueError):
            isolation.runtime_evidence(self.local, mutated)

    def test_grpc_must_be_loopback_and_database_must_not_listen_on_tcp(self):
        def row(address, port):
            return "0: " + address + ":" + format(port, "04X") + " 00000000:0000 0A 0 0\n"
        gateway = "".join(row("0100007F", port) for port in (50051, 50052, 50053, 50054, 9091, 9092)) + row("00000000", 8443)
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


class ObservabilityRuntimeTests(RuntimeTests):
    """The same contract with the opt-in collectors, a non-default HTTPS port and loopback-only admin ports."""

    observability = {"grafana_port": 13000, "log_port": 24224}

    def test_collectors_publish_only_loopback_admin_and_ingest_ports(self):
        config = self.compose()
        for change in (lambda d: d["services"]["gateway"]["ports"].append({"host_ip": "127.0.0.1", "published": "9090", "target": 9090}),
                       lambda d: d["services"]["gateway"]["ports"][0].update(host_ip="0.0.0.0"),
                       lambda d: d["services"]["grafana"].update(ports=[{"host_ip": "127.0.0.1", "published": "3000", "target": 3000}]),
                       lambda d: d["services"]["alloy"].update(network_mode="bridge"),
                       lambda d: d["services"]["capture"]["volumes"].append({"target": "/tls", "type": "bind", "source": "/private/run/tls", "read_only": True}),
                       lambda d: d["services"]["prometheus"].update(read_only=False),
                       lambda d: d["volumes"].pop("loki")):
            mutated = copy.deepcopy(config)
            change(mutated)
            with self.subTest(change=change), self.assertRaises(ValueError):
                isolation.compose_evidence(self.local, mutated)
        for change in (lambda d: d["gateway"]["HostConfig"]["PortBindings"]["3000/tcp"][0].update(HostIp="0.0.0.0"),
                       lambda d: d["grafana"]["HostConfig"].update(NetworkMode="host"),
                       lambda d: d.pop("capture")):
            mutated = copy.deepcopy(self.containers)
            change(mutated)
            with self.subTest(change=change), self.assertRaises(ValueError):
                isolation.runtime_evidence(self.local, mutated)

    def test_grpc_must_be_loopback_and_database_must_not_listen_on_tcp(self):
        def row(address, port):
            return "0: " + address + ":" + format(port, "04X") + " 00000000:0000 0A 0 0\n"
        loopback = (50051, 50052, 50053, 50054, 9091, 9092) + isolation.COLLECTOR_LOOPBACK
        gateway = "".join(row("0100007F", port) for port in loopback) + "".join(row("00000000", port) for port in (8443, 3000, 24224))
        for shared, passes in ((gateway, True), (gateway.replace(row("0100007F", 9093), row("00000000", 9093)), False),
                               (gateway.replace(row("00000000", 3000), ""), False), (gateway + row("00000000", 9094), False)):
            self.local.compose = lambda *args, **kwargs: SimpleNamespace(stdout=shared if "gateway" in args else "")
            if passes:
                self.assertEqual(isolation.listener_evidence(self.local)["status"], "pass")
            else:
                with self.assertRaises(ValueError):
                    isolation.listener_evidence(self.local)


class ProductionTests(unittest.TestCase):
    def archive(self, extra=None, omit=None):
        stream = io.BytesIO()
        with tarfile.open(fileobj=stream, mode="w") as archive:
            for name in ("usr/local/bin/dswt-server", "usr/local/bin/dswt-backend", "usr/local/bin/dswt-matcher",
                         "usr/local/bin/dswt-ledger",
                         "usr/share/dswt/LICENSE", "usr/share/dswt/NOTICE", extra):
                if name and name != omit:
                    info = tarfile.TarInfo(name)
                    info.size = 1
                    archive.addfile(info, io.BytesIO(b"x"))
        stream.seek(0)
        return tarfile.open(fileobj=stream)

    def test_production_inspection_rejects_build_trees_fixtures_and_missing_binaries(self):
        with self.archive() as archive:
            self.assertEqual(isolation.production_files(archive)["status"], "pass")
        for extra in ("build/test", "src/server.cpp", "test/run", "usr/local/bin/dswt-device-fixture",
                      "usr/local/bin/dswt-gateway-fixture", "usr/local/bin/dswt-unit-tests", "../unsafe"):
            with self.subTest(extra=extra), self.archive(extra) as archive, self.assertRaises(ValueError):
                isolation.production_files(archive)
        with self.archive(omit="usr/local/bin/dswt-backend") as archive, self.assertRaises(ValueError):
            isolation.production_files(archive)


if __name__ == "__main__":
    unittest.main()
