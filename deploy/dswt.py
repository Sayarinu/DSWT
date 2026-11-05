#!/usr/bin/env python3
"""Build DSWT images here, ship them to the VPS and run them with Docker Compose."""

import argparse
import hashlib
import http.client
import importlib.util
import json
import os
from pathlib import Path
import shlex
import ssl
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent
LOCAL = Path(subprocess.check_output(["git", "-C", str(ROOT), "rev-parse", "--path-format=absolute", "--git-common-dir"],
                                     text=True).strip()).parent / "game/dswt-production"  # Survives worktree removal.
REMOTE = "/opt/dswt"
REMOTE_USER = "dswt"
ADMIN = "dswt_admin"
VOLUMES = ("data", "socket", "archive", "prometheus", "alertmanager")
APPLICATIONS = ("netns", "validator", "gateway", "backend", "matcher", "prometheus", "alertmanager",
                "node-exporter", "grafana")


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def pin(key):
    public = subprocess.check_output(["openssl", "pkey", "-in", str(key), "-pubout", "-outform", "DER"])
    return hashlib.sha256(public).hexdigest()


def create_keys(hostname, ipv4):
    """Create both pinned keys once. The ROM builds them in, so they are never replaced."""
    connection = load_module("connection", ROOT / "scripts/configure-connection.py")
    LOCAL.mkdir(mode=0o700, parents=True, exist_ok=True)
    directory = LOCAL / "tls"
    if directory.exists():
        raise ValueError(f"{directory} already exists; replacing it would strand every patched ROM")
    with tempfile.TemporaryDirectory(prefix=".tls-", dir=LOCAL) as temporary:
        stage = Path(temporary)
        for name in ("current", "next"):
            subprocess.run([
                "openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256",
                "-pkeyopt", "ec_param_enc:named_curve", "-nodes",
                "-keyout", str(stage / f"{name}.key"), "-out", str(stage / f"{name}.pem"), "-days", "3650",
                "-subj", f"/CN={hostname}", "-addext", f"subjectAltName=DNS:{hostname}",
                "-addext", "basicConstraints=critical,CA:TRUE",
            ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            (stage / f"{name}.key").chmod(0o600)
        profile = {"hostname": hostname, "ipv4": ipv4, "port": 443,
                   "current_spki_sha256": pin(stage / "current.key"), "next_spki_sha256": pin(stage / "next.key")}
        connection.header(profile)
        (stage / "connection.json").write_text(json.dumps(profile, indent=2) + "\n")
        stage.rename(directory)
    print(f"Created production keys and ROM profile in {directory}")
    print("Save tls/next.key somewhere safe outside this machine. It never goes on the server.")


class Target:
    def __init__(self, ssh_key):
        self.profile = json.loads((LOCAL / "tls/connection.json").read_text())
        self.address = f"{REMOTE_USER}@{self.profile['ipv4']}"
        self.ssh = ["ssh", "-i", str(ssh_key), "-o", "IdentitiesOnly=yes", "-o", "BatchMode=yes"]

    def run(self, script, capture=False, check=True):
        result = subprocess.run([*self.ssh, self.address, script], text=True, check=False,
                                stdout=subprocess.PIPE if capture else None)
        if check and result.returncode:
            raise RuntimeError(f"Server command failed ({result.returncode}): {script}")
        return result

    def compose(self, *arguments, capture=False, check=True):
        command = " ".join(shlex.quote(argument) for argument in arguments)
        return self.run(f"cd {REMOTE} && docker compose -p dswt {command}", capture, check)

    def sql(self, query):
        return self.compose("exec", "-T", "database", "psql", "-XAt", "-v", "ON_ERROR_STOP=1", "-U", ADMIN,
                            "-d", "dswt", "-c", query, capture=True).stdout.strip()

    def build_and_load(self, version):
        built = []
        for image, dockerfile, *target in (("dswt-server", "server/Dockerfile", "--target", "production"),
                                           ("dswt-validator", "validator/Dockerfile")):
            tag = f"{image}:{version}"
            subprocess.run(["docker", "build", "-f", dockerfile, *target, "-t", tag, "."], cwd=ROOT, check=True)
            built.append(tag)
        save = subprocess.Popen(["docker", "save", *built], stdout=subprocess.PIPE)
        compress = subprocess.Popen(["gzip", "-1"], stdin=save.stdout, stdout=subprocess.PIPE)
        save.stdout.close()
        load = subprocess.run([*self.ssh, self.address, "gunzip | docker load"], stdin=compress.stdout, check=False)
        compress.stdout.close()
        if save.wait() or compress.wait() or load.returncode:
            raise RuntimeError("Could not transfer the images")

    def sync(self, version):
        transport = " ".join(shlex.quote(part) for part in self.ssh)
        self.run(f"install -d -m 0700 {REMOTE}/tls {REMOTE}/secrets && install -d -m 0755 {REMOTE}/observability")
        rsync = ["rsync", "-rt", "--delete", "--chmod=D755,F644", "-e", transport]
        subprocess.run([*rsync, f"{ROOT}/deploy/observability/", f"{self.address}:{REMOTE}/observability/"], check=True)
        subprocess.run(["rsync", "-t", "--chmod=F644", "-e", transport, f"{ROOT}/deploy/compose.yaml",
                        f"{self.address}:{REMOTE}/compose.yaml"], check=True)
        subprocess.run(["rsync", "-t", "--chmod=F755", "-e", transport, f"{ROOT}/server/wal-archive.sh",
                        f"{self.address}:{REMOTE}/wal-archive.sh"], check=True)
        # Only the current key goes to the server, and an installed key is never overwritten.
        subprocess.run(["rsync", "-t", "--ignore-existing", "--chmod=F600", "-e", transport,
                        str(LOCAL / "tls/current.key"), str(LOCAL / "tls/current.pem"),
                        f"{self.address}:{REMOTE}/tls/"], check=True)
        served = self.run(f"openssl x509 -in {REMOTE}/tls/current.pem -pubkey -noout | openssl pkey -pubin -outform DER"
                          " | sha256sum", capture=True).stdout.split()[0]
        if served != self.profile["current_spki_sha256"]:
            raise RuntimeError("The server's certificate does not match the ROM profile")
        self.run(f"test -e {REMOTE}/secrets/grafana-admin-password || "
                 f"(umask 077; openssl rand -hex 32 > {REMOTE}/secrets/grafana-admin-password)")
        self.run(f"printf 'DSWT_VERSION={version}\\nDSWT_UID=%s\\nDSWT_GID=%s\\n' \"$(id -u)\" \"$(id -g)\""
                 f" > {REMOTE}/.env.new && mv {REMOTE}/.env.new {REMOTE}/.env")
        for volume in VOLUMES:
            self.run(f"docker volume create dswt-{volume} >/dev/null")

    def prepare_database(self):
        """Serve as an unprivileged owner, and start the archive chain from a base backup."""
        if self.sql("SELECT count(*) FROM pg_roles WHERE rolname='dswt_service'") == "0":
            self.sql("CREATE ROLE dswt_service LOGIN NOSUPERUSER NOCREATEDB NOCREATEROLE")
        self.sql("ALTER DATABASE dswt OWNER TO dswt_service")
        self.sql("GRANT EXECUTE ON FUNCTION pg_switch_wal() TO dswt_service")
        archive = ("exec", "-T", "--user", "postgres", "database")
        if self.compose(*archive, "test", "-e", "/archive/chain.armed", check=False).returncode == 0:
            return
        self.compose(*archive, "/usr/local/bin/wal-archive.sh", "init", "/archive",
                     self.sql("SELECT system_identifier FROM pg_control_system()"),
                     self.sql("SELECT pg_walfile_name(pg_current_wal_insert_lsn())"))
        self.compose(*archive, "pg_basebackup", "-h", "/var/run/postgresql", "-U", ADMIN, "-D", "/archive/base",
                     "-X", "stream", "-c", "fast")
        self.compose(*archive, "/usr/local/bin/wal-archive.sh", "arm", "/archive")

    def backup(self):
        """Copy the database off the server before a migration can change it."""
        directory = LOCAL / "backups"
        directory.mkdir(mode=0o700, exist_ok=True)
        final = directory / f"dswt-{time.strftime('%Y%m%dT%H%M%SZ', time.gmtime())}.dump"
        partial = final.with_suffix(".partial")
        with partial.open("wb") as output:
            dump = subprocess.run([*self.ssh, self.address, f"cd {REMOTE} && docker compose -p dswt exec -T database "
                                   f"pg_dump -Fc -U {ADMIN} dswt"], stdout=output, check=False)
        with partial.open("rb") as written:
            if dump.returncode or written.read(5) != b"PGDMP":
                raise RuntimeError(f"The database backup failed, so nothing was migrated: {partial}")
        partial.rename(final)
        print(f"Saved database backup {final}")

    def tidy(self):
        """Run after a verified release: only then is the old archive chain safe to replace."""
        archive = ("exec", "-T", "--user", "postgres", "database")
        self.compose(*archive, "rm", "-rf", "/archive/base.next")
        self.compose(*archive, "pg_basebackup", "-h", "/var/run/postgresql", "-U", ADMIN, "-D", "/archive/base.next",
                     "-X", "stream", "-c", "fast")
        self.compose(*archive, "pg_verifybackup", "/archive/base.next")
        self.compose(*archive, "/usr/local/bin/wal-archive.sh", "rotate", "/archive", "/archive/base.next")
        self.run("docker image prune --all --force >/dev/null")
        self.run("df -h / | tail -1")

    def start(self):
        self.compose("stop", *reversed(APPLICATIONS))
        # Recreated so the database mounts the wal-archive.sh just synced; a bind mount keeps the replaced file.
        self.compose("up", "-d", "--force-recreate", "--wait", "--wait-timeout", "120", "database")
        self.prepare_database()
        self.backup()
        self.compose("run", "--rm", "--no-deps", "migrate")
        self.compose("up", "-d", "--force-recreate", "--remove-orphans", *APPLICATIONS)

    def ready(self):
        """Ask the public endpoint for status over the only TLS suite the ROM offers."""
        wire = load_module("deploy_wire", ROOT / "protocol/wire.py")
        context = ssl.create_default_context(cafile=str(LOCAL / "tls/current.pem"))
        context.minimum_version = context.maximum_version = ssl.TLSVersion.TLSv1_2
        context.set_ciphers("ECDHE-ECDSA-CHACHA20-POLY1305")
        request = wire.Request(wire.Operation.STATUS, os.urandom(16), b"\x02" * 16, b"\x03" * 32, 0, bytes(16))
        connection = http.client.HTTPSConnection(self.profile["hostname"], self.profile["port"], context=context, timeout=5)
        try:
            connection.request("POST", "/wt/v1", wire.encode_request(request),
                               {"Content-Type": "application/octet-stream", "Connection": "close"})
            response = connection.getresponse()
            reply = wire.decode_response(response.read(wire.MAX_PACKET + 1))
            if response.status != 200 or reply.request_id != request.request_id or reply.result != wire.Result.UNAUTHENTICATED:
                raise ValueError("Unexpected reply from the trading endpoint")
        finally:
            connection.close()

    def wait_ready(self):
        deadline = time.monotonic() + 90
        while True:
            try:
                self.ready()
                return
            except (OSError, ValueError, http.client.HTTPException) as error:
                if time.monotonic() >= deadline:
                    raise RuntimeError(f"The public endpoint did not become ready: {error}") from error
                time.sleep(2)

    def verify(self):
        self.wait_ready()
        self.compose("exec", "-T", "backend", "/usr/local/bin/dswt-ledger", "verify")
        self.compose("ps", "--format", "table {{.Service}}\t{{.Status}}")
        print("Public endpoint and ledger verified")


def source_version():
    def git(*arguments):
        return subprocess.check_output(["git", "-C", str(ROOT), *arguments], text=True).strip()
    if git("status", "--porcelain"):
        raise ValueError("Deploy from a clean commit, so the running images trace back to it")
    return git("rev-parse", "--short=12", "HEAD")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("keys", "deploy", "verify", "tidy"))
    parser.add_argument("--hostname", help="keys: the public TLS hostname")
    parser.add_argument("--ipv4", help="keys: the server address built into the ROM")
    parser.add_argument("--ssh-key", type=Path, default=Path.home() / ".ssh/dswt")
    args = parser.parse_args()
    os.umask(0o077)
    try:
        if args.action == "keys":
            if not args.hostname or not args.ipv4:
                parser.error("keys needs --hostname and --ipv4")
            create_keys(args.hostname, args.ipv4)
            return 0
        target = Target(args.ssh_key)
        if args.action == "deploy":
            version = source_version()
            target.build_and_load(version)
            target.sync(version)
            target.start()
        target.verify()
        if args.action != "verify":
            target.tidy()
        return 0
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"Cannot complete {args.action}: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
