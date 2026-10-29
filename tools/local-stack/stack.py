#!/usr/bin/env python3
"""Manage an isolated local Wonder Trade stack without resetting retained state."""

import argparse
import fcntl
import http.client
import importlib.util
import json
import os
from pathlib import Path
import re
import ssl
import stat
import secrets
import subprocess
import sys
import time
import uuid


ROOT = Path(__file__).resolve().parents[2]
COMPOSE = Path(__file__).with_name("compose.yaml")
OBSERVABILITY = Path(__file__).with_name("compose.observability.yaml")
OBSERVABILITY_CONFIG = Path(__file__).with_name("observability")
LABEL = "studio.nouvellelune.platinumwt.local-workspace"
SERVICES = ("database", "gateway", "validator", "backend")
OBSERVABILITY_SERVICES = ("prometheus", "alertmanager", "loki", "alloy", "grafana", "capture")
DEFAULT_PORT = 8443
# Owned retained volumes; the tmpfs probe is a bounded filesystem for resource-pressure alert tests.
OBSERVABILITY_VOLUMES = ("prometheus", "alertmanager", "loki", "pressure")


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


wire = load_module("local_stack_wire", ROOT / "protocol/wire.py")


def workspace_path(value):
    path = Path(value).absolute()
    if path != path.resolve() or not path.is_relative_to(ROOT / "game") or path == ROOT / "game":
        raise ValueError("Use a workspace below the ignored game directory without symlinks")
    return path


def private_file(path):
    info = path.lstat()
    if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or info.st_uid != os.getuid():
        raise ValueError("Workspace files must be owned regular files without links")
    if info.st_mode & 0o077:
        raise ValueError("Private workspace files must have mode 0600")


def save_manifest(workspace, manifest):
    temporary = workspace / ".manifest.new"
    descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        with os.fdopen(descriptor, "w") as output:
            json.dump(manifest, output, indent=2)
            output.write("\n")
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, workspace / "manifest.json")
        directory = os.open(workspace, os.O_RDONLY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        temporary.unlink(missing_ok=True)


def valid_port(value):
    return type(value) is int and 1024 <= value <= 65535


def valid_ports(port, observability):
    if observability is not None and (not isinstance(observability, dict) or set(observability) != {"grafana_port", "log_port"}):
        return False
    ports = [port, *(observability or {}).values()]
    return all(valid_port(item) for item in ports) and len(set(ports)) == len(ports)


def initialize(workspace, port=DEFAULT_PORT, observability=None):
    if not valid_ports(port, observability):
        raise ValueError("Use distinct host ports between 1024 and 65535")
    workspace.parent.mkdir(parents=True, exist_ok=True)
    workspace.mkdir(mode=0o700)  # Exclusive: an existing run is never reinitialized.
    (workspace / "control").mkdir(mode=0o700)
    # Docker cannot create a nested socket mount below the read-only control bind.
    (workspace / "control/run").mkdir(mode=0o700)
    peer = load_module("local_stack_tls_peer", ROOT / "scripts/tls-test-server.py")
    peer.prepare(workspace / "tls", "10.64.0.1", "localhost", port)
    manifest = {
        "version": 1, "project": "platinumwt-local-" + uuid.uuid4().hex,
        "workspace": str(workspace), "profile": "tls/connection.json",
        "host_endpoint": f"https://localhost:{port}/wt/v1", "volumes_created": False,
        "database_system_id": None, "source_commit": None, "source_dirty": None, "images": {},
    }
    if port != DEFAULT_PORT:
        manifest["https_port"] = port
    if observability:
        observe = workspace / "observability"
        observe.mkdir(mode=0o700)
        for child in ("alerts", "textfile"):
            (observe / child).mkdir(mode=0o700)
        secret = os.open(observe / "grafana-admin-password", os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        with os.fdopen(secret, "w") as output:
            output.write(secrets.token_hex(32))
        manifest["observability"] = dict(observability)
    save_manifest(workspace, manifest)


def https_port(manifest):
    return manifest.get("https_port", DEFAULT_PORT)


def read_manifest(workspace):
    info = workspace.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077:
        raise ValueError("Use the original owned private workspace directory")
    path = workspace / "manifest.json"
    private_file(path)
    manifest = json.loads(path.read_text())
    if (not isinstance(manifest, dict) or manifest.get("version") != 1 or manifest.get("workspace") != str(workspace)
            or not isinstance(manifest.get("project"), str)
            or not re.fullmatch(r"platinumwt-local-[0-9a-f]{32}", manifest["project"])
            or type(manifest.get("volumes_created")) is not bool
            or manifest.get("profile") != "tls/connection.json"
            or ("https_port" in manifest and (not valid_port(manifest["https_port"]) or manifest["https_port"] == DEFAULT_PORT))
            or manifest.get("host_endpoint") != f"https://localhost:{https_port(manifest)}/wt/v1"
            or ("observability" in manifest and manifest["observability"] is None)
            or not valid_ports(https_port(manifest), manifest.get("observability"))
            or (manifest.get("database_system_id") is not None
                and (not isinstance(manifest["database_system_id"], str)
                     or not re.fullmatch(r"[0-9]+", manifest["database_system_id"])))):
        raise ValueError("Unsupported or moved workspace manifest; preserve its state")
    for child in ("tls", "control", "control/run"):
        if (workspace / child).is_symlink() or not (workspace / child).is_dir():
            raise ValueError("Workspace directories must not be replaced or linked")
    for name in ("current", "next", "untrusted"):
        private_file(workspace / "tls" / (name + ".key"))
    if "observability" in manifest:
        for child in ("observability", "observability/alerts", "observability/textfile"):
            if (workspace / child).is_symlink() or not (workspace / child).is_dir():
                raise ValueError("Workspace directories must not be replaced or linked")
        private_file(workspace / "observability/grafana-admin-password")
    return manifest


def command(arguments, *, env=None, capture=False, timeout=None, check=True):
    return subprocess.run(arguments, env=env, text=True, check=check,
                          stdout=subprocess.PIPE if capture else None,
                          stderr=subprocess.PIPE if capture else None, timeout=timeout)


class Stack:
    def __init__(self, workspace, manifest):
        self.workspace, self.manifest = workspace, manifest
        self.env = {key: value for key, value in os.environ.items()
                    if not key.startswith(("COMPOSE_", "PWT_LOCAL_", "PG"))
                    and key not in ("PLATINUMWT_DATABASE_URL", "DATABASE_URL", "DOCKER_DEFAULT_PLATFORM")}
        endpoint = None
        if os.environ.get("DOCKER_HOST") and not os.environ.get("DOCKER_CONTEXT"):
            endpoint = os.environ["DOCKER_HOST"]
        if endpoint is None:
            context = json.loads(command(["docker", "context", "inspect"], env=self.env,
                                         capture=True, timeout=10).stdout)
            endpoint = context[0]["Endpoints"]["docker"]["Host"]
        if not isinstance(endpoint, str) or not endpoint.startswith("unix:///"):
            raise ValueError("Local stack requires a local Unix Docker socket; remote contexts are refused")
        self.env.pop("DOCKER_HOST", None)
        self.env.pop("DOCKER_CONTEXT", None)
        self.docker = ["docker", "--host", endpoint]
        architecture = self.run(["info", "--format", "{{.OSType}}/{{.Architecture}}"], capture=True).stdout.strip()
        platforms = {"linux/aarch64": "linux/arm64", "linux/arm64": "linux/arm64",
                     "linux/x86_64": "linux/amd64", "linux/amd64": "linux/amd64"}
        if architecture not in platforms:
            raise ValueError("Use a local Linux Docker engine on arm64 or amd64")
        self.env.update(PWT_LOCAL_WORKSPACE=str(workspace), PWT_LOCAL_PROJECT=manifest["project"],
                        PWT_LOCAL_UID=str(os.getuid()), PWT_LOCAL_GID=str(os.getgid()),
                        PWT_LOCAL_PLATFORM=platforms[architecture], PWT_LOCAL_HTTPS_PORT=str(https_port(manifest)))
        self.observability = manifest.get("observability")
        if self.observability:
            self.env.update(PWT_OBS_GRAFANA_PORT=str(self.observability["grafana_port"]),
                            PWT_OBS_LOG_PORT=str(self.observability["log_port"]))
        self.services = SERVICES + (OBSERVABILITY_SERVICES if self.observability else ())

    def run(self, arguments, **kwargs):
        return command(self.docker + arguments, env=self.env, **kwargs)

    def compose(self, *arguments, **kwargs):
        files = ["--file", str(COMPOSE)] + (["--file", str(OBSERVABILITY)] if self.observability else [])
        return self.run(["compose", "--env-file", os.devnull, "--project-directory", str(COMPOSE.parent),
                         *files, "--project-name", self.manifest["project"], *arguments], **kwargs)

    def volumes(self, create=False):
        listing = self.run(["volume", "ls", "--format", "{{json .}}"], capture=True).stdout
        existing = {json.loads(line)["Name"] for line in listing.splitlines()}
        for suffix in ("data", "socket") + (OBSERVABILITY_VOLUMES if self.observability else ()):
            name = self.manifest["project"] + "-" + suffix
            if name not in existing:
                if not create or self.manifest["volumes_created"]:
                    raise ValueError("Retained local volume is missing; refusing to create a replacement: " + name)
                options = []
                if suffix == "pressure":
                    options = ["--opt", "type=tmpfs", "--opt", "device=tmpfs",
                               "--opt", f"o=size=16m,uid={os.getuid()},gid={os.getgid()},mode=0700"]
                self.run(["volume", "create", "--label", LABEL + "=" + self.manifest["project"], *options, name], capture=True)
            info = json.loads(self.run(["volume", "inspect", name], capture=True).stdout)[0]
            if (info.get("Labels") or {}).get(LABEL) != self.manifest["project"]:
                raise ValueError("Local volume ownership does not match this workspace")
        if not self.manifest["volumes_created"]:
            self.manifest["volumes_created"] = True
            save_manifest(self.workspace, self.manifest)

    def database_identity(self):
        result = self.compose("exec", "-T", "database", "psql", "-XAt", "-U", "platinumwt_local",
                              "-d", "platinumwt_e2e", "-c", "SELECT system_identifier FROM pg_control_system()",
                              capture=True, timeout=10)
        identity = result.stdout.strip()
        if not re.fullmatch(r"[0-9]+", identity):
            raise ValueError("Cannot identify the local database")
        prior = self.manifest["database_system_id"]
        if prior is not None and identity != prior:
            raise ValueError("Database identity changed; preserve the workspace and inspect recovery evidence")
        if prior is None:
            self.manifest["database_system_id"] = identity
            save_manifest(self.workspace, self.manifest)

    def ready(self, request_id=b"\x01" * 16):
        self.compose("exec", "-T", "backend", "/build/tests/platinumwt-device-fixture", "--check",
                     capture=True, timeout=10)
        context = ssl.create_default_context(cafile=str(self.workspace / "tls/current.pem"))
        context.minimum_version = context.maximum_version = ssl.TLSVersion.TLSv1_2
        context.set_ciphers("ECDHE-ECDSA-CHACHA20-POLY1305")
        request = wire.Request(wire.Operation.STATUS, request_id, b"\x02" * 16, b"\x03" * 32, 0, bytes(16))
        connection = http.client.HTTPSConnection("localhost", https_port(self.manifest), context=context, timeout=5)
        try:
            connection.request("POST", "/wt/v1", wire.encode_request(request),
                               {"Content-Type": "application/octet-stream", "Connection": "close"})
            response = connection.getresponse()
            packet = response.read(wire.MAX_PACKET + 1)
            if response.status != 200 or response.getheader("Content-Type") != "application/octet-stream":
                raise ValueError("HTTPS trading endpoint is not ready")
            reply = wire.decode_response(packet)
            if (reply.request_id != request.request_id or reply.operation != request.operation
                    or reply.result != wire.Result.UNAUTHENTICATED or reply.next_counter or reply.event_highwater):
                raise ValueError("Unexpected local trading readiness response")
        finally:
            connection.close()

    def up(self):
        self.volumes(create=True)
        self.compose("build", "gateway", "validator")
        applications = [service for service in self.services if service != "database"]
        self.compose("stop", *reversed(applications))
        self.compose("up", "-d", "--wait", "--wait-timeout", "90", "database")
        self.database_identity()
        self.compose("run", "--rm", "--no-deps", "migrate")
        self.compose("up", "-d", "--force-recreate", *applications)
        deadline = time.monotonic() + 60
        while True:
            try:
                self.ready()
                break
            except (OSError, ValueError, subprocess.SubprocessError):
                if time.monotonic() >= deadline:
                    raise RuntimeError("Local stack did not become ready; state retained for inspection")
                time.sleep(1)
        self.manifest["source_commit"] = command(["git", "-C", str(ROOT), "rev-parse", "HEAD"], capture=True).stdout.strip()
        self.manifest["source_dirty"] = bool(command(["git", "-C", str(ROOT), "status", "--porcelain"], capture=True).stdout.strip())
        self.manifest["images"] = {}
        for service in self.services:
            container = self.compose("ps", "-q", service, capture=True).stdout.strip()
            self.manifest["images"][service] = self.run(["inspect", "--format", "{{.Image}}", container], capture=True).stdout.strip()
        save_manifest(self.workspace, self.manifest)
        print("Local stack ready:", self.manifest["host_endpoint"] + "; ROM profile:", self.workspace / self.manifest["profile"])
        if self.observability:
            print(f"Private Grafana: http://127.0.0.1:{self.observability['grafana_port']}/ (admin password in "
                  f"{self.workspace / 'observability/grafana-admin-password'})")

    def verify(self):
        self.volumes()
        self.database_identity()
        self.ready()
        self.compose("exec", "-T", "backend", "/build/platinumwt-ledger", "verify")
        print("Verified local services and ledger; no off-site durability claim")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace", type=Path, default=ROOT / "game/local-e2e")
    parser.add_argument("action", choices=("init", "up", "down", "status", "verify"))
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help="init: host loopback HTTPS port")
    parser.add_argument("--observability", nargs=2, type=int, metavar=("GRAFANA_PORT", "LOG_PORT"),
                        help="init: enable the private local observability profile on these loopback ports")
    args = parser.parse_args()
    try:
        workspace = workspace_path(args.workspace)
        if args.action == "init":
            initialize(workspace, args.port, args.observability and
                       {"grafana_port": args.observability[0], "log_port": args.observability[1]})
            print("Initialized private local workspace:", workspace)
            return 0
        read_manifest(workspace)
        descriptor = os.open(workspace / ".lock", os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600)
        try:
            private_file(workspace / ".lock")
            fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
            stack = Stack(workspace, read_manifest(workspace))
            if args.action == "up":
                stack.up()
            elif args.action == "down":
                stack.compose("down")
                print("Stopped local stack; database, keys and workspace retained")
            elif args.action == "status":
                stack.compose("ps", "--all")
                stack.volumes()
                stack.database_identity()
                stack.ready()
                print("Local stack ready")
            else:
                stack.verify()
        finally:
            os.close(descriptor)
        return 0
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.SubprocessError) as error:
        print("Local stack failed:", error, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
