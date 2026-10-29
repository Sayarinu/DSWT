#!/usr/bin/env python3
"""Collect private local isolation evidence without changing retained services/data."""

import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tarfile
import time
import uuid

import policy
import stack


require = policy.require


def digest(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def volume_contract(local, service):
    project = local.manifest["project"]
    socket = ("volume", project + "-socket", True)
    return {
        "database": {"/var/lib/postgresql/data": ("volume", project + "-data", True), "/var/run/postgresql": socket},
        "gateway": {"/tls": ("bind", str(local.workspace / "tls"), False)},
        "backend": {"/test": ("bind", str(local.workspace / "control"), False), "/test/run": socket},
        "validator": {},
        "migrate": {"/test/run": socket},
    }[service]


def mounts(actual, expected):
    require(actual == expected, "Unexpected mount, access mode or volume identity")


def validator_tmpfs(options):
    require(isinstance(options, str), "Invalid validator tmpfs options")
    flags = options.lower().split(",")
    require({"noexec", "nosuid", "nodev"} <= set(flags) and not {"exec", "suid", "dev"} & set(flags),
            "Validator tmpfs must prohibit execution, devices and setuid")
    sizes = [flag[5:] for flag in flags if flag.startswith("size=")]
    require(len(sizes) == 1, "Validator tmpfs needs one size bound")
    match = re.fullmatch(r"([0-9]+)([kmg])?", sizes[0])
    require(match is not None and int(match[1]) * {None: 1, "k": 1024, "m": 1024 ** 2, "g": 1024 ** 3}[match[2]] == 64 * 1024 * 1024,
            "Validator tmpfs must be bounded to 64 MiB")


def compose_evidence(local, config):
    services = config["services"]
    require(set(services) == {*stack.SERVICES, "migrate"}, "Unexpected local service set")
    require(all(config["volumes"][key].get("external") is True
                and config["volumes"][key]["name"] == local.manifest["project"] + "-" + key
                for key in ("data", "socket")), "Unexpected external volume binding")
    for name, service in services.items():
        require(not service.get("privileged", False) and not service.get("devices")
                and not service.get("cap_add") and service.get("pid") != "host"
                and service.get("ipc") != "host", "Privileged local service")
        ports = service.get("ports", [])
        actual_ports = {(p.get("host_ip"), str(p["published"]), p["target"], p.get("protocol", "tcp")) for p in ports}
        require(actual_ports == ({("127.0.0.1", "8443", 8443, "tcp")} if name == "gateway" else set()), "Unexpected published endpoint")
        actual_mounts = {}
        for item in service.get("volumes", []):
            source = item["source"]
            if item["type"] == "volume":
                source = config["volumes"][source]["name"]
            require(item["target"] not in actual_mounts, "Duplicate mount destination")
            actual_mounts[item["target"]] = (item["type"], source, not item.get("read_only", False))
        mounts(actual_mounts, volume_contract(local, name))
        if name in ("database", "migrate"):
            require(service.get("network_mode") == "none", "Database/migration networking must be disabled")
        elif name in ("backend", "validator"):
            require(service.get("network_mode") == "service:gateway", "Internal services must share the gateway namespace")
        else:
            require(not service.get("network_mode"), "Unexpected gateway network mode")
        if name != "database":
            require(service.get("read_only") is True and service.get("cap_drop") == ["ALL"]
                    and "no-new-privileges:true" in service.get("security_opt", [])
                    and bool(service.get("tmpfs")), "Missing workload restrictions")
    database = services["database"]
    require(database["command"] == ["postgres", "-c", "listen_addresses=", "-c", "fsync=on", "-c",
                                    "full_page_writes=on", "-c", "synchronous_commit=on"], "Unexpected PostgreSQL durability/listener settings")
    require(re.fullmatch(r"postgres:16\.[0-9]+@sha256:[0-9a-f]{64}", database["image"]), "Pin the PostgreSQL image")
    validator = services["validator"]
    require(int(validator["mem_limit"]) == 512 * 1024 * 1024 and float(validator["cpus"]) == 2
            and validator["pids_limit"] == 128, "Unexpected validator resource bounds")
    require(len(validator["tmpfs"]) == 1 and validator["tmpfs"][0].startswith("/tmp:"), "Unexpected validator tmpfs mount")
    validator_tmpfs(validator["tmpfs"][0].split(":", 1)[1])
    return {"status": "pass", "sha256": hashlib.sha256(json.dumps(config, sort_keys=True).encode()).hexdigest(),
            "services": sorted(services)}


def runtime_evidence(local, containers):
    require(set(containers) == set(stack.SERVICES), "Missing runtime containers")
    gateway = containers["gateway"]["Id"]
    report = {}
    for name, item in containers.items():
        host, config = item["HostConfig"], item["Config"]
        require(item["State"]["Running"] is True, "Service is not running: " + name)
        require(config["Labels"].get("com.docker.compose.project") == local.manifest["project"]
                and config["Labels"].get("com.docker.compose.service") == name, "Foreign container")
        require(item["Image"] == local.manifest["images"][name], "Running image differs from retained manifest")
        require(not host.get("Privileged") and not host.get("CapAdd") and not host.get("Devices")
                and host.get("PidMode") != "host" and host.get("IpcMode") != "host", "Privileged runtime configuration")
        ports = {key: value for key, value in (host.get("PortBindings") or {}).items() if value}
        require(ports == ({"8443/tcp": [{"HostIp": "127.0.0.1", "HostPort": "8443"}]} if name == "gateway" else {}),
                "Unexpected runtime port publication")
        actual_mounts = {}
        for mount in item["Mounts"]:
            require(mount["Destination"] not in actual_mounts, "Duplicate runtime mount")
            actual_mounts[mount["Destination"]] = (mount["Type"], mount.get("Name") if mount["Type"] == "volume" else mount["Source"], mount["RW"])
        mounts(actual_mounts, volume_contract(local, name))
        mode = host["NetworkMode"]
        if name == "database":
            require(mode == "none", "Database must have no container network")
        elif name in ("backend", "validator"):
            require(mode == "container:" + gateway, "Unexpected internal namespace")
        else:
            require(mode == local.manifest["project"] + "_default", "Unexpected gateway network")
        if name != "database":
            require(host["ReadonlyRootfs"] and host["CapDrop"] == ["ALL"]
                    and "no-new-privileges:true" in host["SecurityOpt"] and set(host["Tmpfs"]) == {"/tmp"}, "Missing runtime restrictions")
            require(re.fullmatch(r"[1-9][0-9]*(?::[0-9]+)?", config["User"]), "Application must use a nonzero numeric UID")
        if name == "validator":
            require(host["Memory"] == 512 * 1024 * 1024 and host["NanoCpus"] == 2_000_000_000
                    and host["PidsLimit"] == 128, "Unexpected runtime validator limits")
            validator_tmpfs(host["Tmpfs"]["/tmp"])
        report[name] = {"container": item["Id"], "image": item["Image"], "network_mode": mode, "status": "pass"}
    return report


def listeners(text):
    result = set()
    for line in text.splitlines():
        fields = line.split()
        if len(fields) > 3 and fields[3] == "0A":
            address, port = fields[1].split(":")
            result.add((address, int(port, 16)))
    return result


def listener_evidence(local):
    output = local.compose("exec", "-T", "gateway", "sh", "-c", "cat /proc/net/tcp /proc/net/tcp6", capture=True, timeout=10).stdout
    bound = listeners(output)
    # Linux gRPC may represent 127.0.0.1 in the IPv6-mapped TCP table.
    loopback = {"0100007F", "00000000000000000000000001000000", "0000000000000000FFFF00000100007F"}
    for port in (50051, 50052, 50053, 50054):
        endpoints = {address for address, number in bound if number == port}
        require(bool(endpoints) and endpoints <= loopback, "Internal gRPC listener is not loopback")
    require(any(port == 8443 for _, port in bound), "HTTPS listener is absent")
    require(all(port in (8443, 50051, 50052, 50053, 50054) or address == "0B00007F" for address, port in bound),
            "Unexpected listener in the shared namespace")
    output = local.compose("exec", "-T", "database", "sh", "-c", "cat /proc/net/tcp /proc/net/tcp6", capture=True, timeout=10).stdout
    require(not listeners(output), "Database TCP must be disabled")
    return {"status": "pass", "shared_namespace_listeners": sorted(bound), "database_tcp": []}


def production_files(archive):
    required = {"usr/local/bin/platinumwt-server", "usr/local/bin/platinumwt-backend", "usr/local/bin/platinumwt-ledger",
                "usr/share/platinumwt/LICENSE", "usr/share/platinumwt/NOTICE"}
    found, count = {}, 0
    for member in archive:
        path = member.name.removeprefix("./").rstrip("/")
        require(not path.startswith("/") and ".." not in path.split("/"), "Unsafe image path")
        require(not re.match(r"^(build|src|test)(/|$)", path)
                and not re.search(r"(^|/)(platinumwt-device-fixture|platinumwt-gateway-fixture|platinumwt-.*tests?)(/|$)", path),
                "Production image contains project tests/build inputs")
        require(not path.startswith("usr/local/bin/") or path in required, "Unexpected production executable")
        count += 1
        if path in required:
            require(member.isfile(), "Production executable/notice must be an ordinary file")
            found[path] = hashlib.file_digest(archive.extractfile(member), "sha256").hexdigest()
    require(set(found) == required, "Production executables/notices are missing")
    return {"status": "pass", "entries_checked": count, "files": found}


def production_evidence(local, image, directory):
    info = json.loads(local.run(["image", "inspect", image], capture=True, timeout=30).stdout)[0]
    require(not info["Config"].get("Volumes"), "Unexpected production image volumes")
    container = local.run(["create", "--network", "none", "--label", stack.LABEL + "=" + local.manifest["project"], info["Id"]],
                          capture=True, timeout=30).stdout.strip()
    require(re.fullmatch(r"[0-9a-f]{64}", container), "Cannot identify owned inspection container")
    path = directory / "production-filesystem.tar"
    try:
        local.run(["export", "--output", str(path), container], capture=True, timeout=120)
        with tarfile.open(path, "r:") as archive:
            result = production_files(archive)
        return dict(result, image=info["Id"], export_sha256=digest(path), export=path.name)
    finally:
        # Only the never-started container created above is removed; images/data are retained.
        local.run(["rm", container], capture=True, timeout=30)


def github_status(evidence):
    return "pass" if all(item["status"] in ("pass", "not_applicable") for item in evidence.values()) else "incomplete"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("policy", "audit"))
    parser.add_argument("--github", action="store_true", help="Read live GitHub settings with gh; never modify them")
    parser.add_argument("--repository", default="Sayarinu/PlatinumWonderTrade")
    parser.add_argument("--workspace", type=Path, default=stack.ROOT / "game/local-e2e")
    parser.add_argument("--production-image", help="Locally built final C++ image; never the build/test stage")
    args = parser.parse_args()
    os.umask(0o077)
    report = {"version": 1, "evidence_layer": "local-isolation", "status": "running", "hosted": "not_run",
              "hostile_fork": "not_run", "off_site_durability": "not_run", "shared_namespace": "one trust domain"}
    directory = None
    try:
        report["policy"] = policy.audit(stack.ROOT)
        report["source_commit"] = stack.command(["git", "-C", str(stack.ROOT), "rev-parse", "HEAD"], capture=True).stdout.strip()
        report["source_dirty"] = bool(stack.command(["git", "-C", str(stack.ROOT), "status", "--porcelain"], capture=True).stdout.strip())
        if args.github:
            report["github_repository"] = "https://github.com/" + args.repository
            report["github"] = policy.github(json.loads((stack.ROOT / ".github/repository-settings.json").read_text()), args.repository)
        if args.action == "audit":
            require(bool(args.production_image), "Supply the locally built final production image")
            workspace = stack.workspace_path(args.workspace)
            stack.read_manifest(workspace)
            descriptor = os.open(workspace / ".lock", os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600)
            with os.fdopen(descriptor, "a+") as lock:
                stack.private_file(workspace / ".lock")
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                local = stack.Stack(workspace, stack.read_manifest(workspace))
                local.volumes()
                local.database_identity()
                local.ready()
                parent = workspace / "isolation"
                parent.mkdir(mode=0o700, exist_ok=True)
                require(not parent.is_symlink(), "Linked evidence directory")
                directory = parent / (time.strftime("%Y%m%dT%H%M%S") + "-" + uuid.uuid4().hex[:8])
                directory.mkdir(mode=0o700)
                config = json.loads(local.compose("--profile", "tools", "config", "--format", "json", capture=True, timeout=30).stdout)
                report["compose"] = compose_evidence(local, config)
                containers = {}
                for name in stack.SERVICES:
                    identifier = local.compose("ps", "-q", name, capture=True, timeout=10).stdout.strip()
                    require(re.fullmatch(r"[0-9a-f]{64}", identifier), "Missing/ambiguous local container")
                    containers[name] = json.loads(local.run(["inspect", identifier], capture=True, timeout=10).stdout)[0]
                report["runtime"] = runtime_evidence(local, containers)
                report["listeners"] = listener_evidence(local)
                report["database_system_id"] = local.manifest["database_system_id"]
                report["production"] = production_evidence(local, args.production_image, directory)
                binaries = local.compose("exec", "-T", "gateway", "sha256sum", "/build/platinumwt-server", "/build/platinumwt-backend", "/build/platinumwt-ledger",
                                         capture=True, timeout=30).stdout
                compiled = {}
                for line in binaries.splitlines():
                    checksum, filename = line.split()
                    require(re.fullmatch(r"[0-9a-f]{64}", checksum) and filename.startswith("/build/"), "Invalid compiled program digest")
                    compiled["usr/local/bin/" + filename.removeprefix("/build/")] = checksum
                expected = {path: checksum for path, checksum in report["production"]["files"].items() if path.startswith("usr/local/bin/")}
                require(compiled == expected, "Final production programs differ from the tested build image")
                report["compiled_programs"] = compiled
                local.compose("exec", "-T", "backend", "/build/platinumwt-ledger", "verify", capture=True, timeout=60)
                report["ledger_replay"] = "pass"
        report["status"] = github_status(report["github"]) if args.github else "pass"
    except (OSError, ValueError, KeyError, TypeError, RuntimeError, subprocess.SubprocessError, policy.yaml.YAMLError) as error:
        report.update(status="fail", failure=type(error).__name__ + ": " + str(error))
    finally:
        if directory:
            with (directory / "report.json").open("x") as output:
                json.dump(report, output, indent=2, sort_keys=True)
                output.write("\n")
            print("Private isolation evidence: " + str(directory / "report.json"), file=sys.stderr)
        print(json.dumps(report, indent=2, sort_keys=True))
    return 0 if report["status"] == "pass" else 1


if __name__ == "__main__":
    sys.exit(main())
