#!/usr/bin/env python3
"""Local, ping-only TLS fixture for DS testing; never a production trade service."""

import argparse
import hashlib
import json
from pathlib import Path
import socket
import ssl
import subprocess
import tempfile
import threading
import time


ROOT = Path(__file__).resolve().parent.parent


def prepare(directory, ipv4, hostname, port):
    if directory.exists() or directory.is_symlink():
        raise ValueError("Test directory already exists; retain its keys or choose a new directory")
    directory.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".tls-peer-", dir=directory.parent) as temp:
        stage = Path(temp)
        pins = []
        for name in ("current", "next", "untrusted"):
            key, cert = stage / f"{name}.key", stage / f"{name}.pem"
            subprocess.run([
                "openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256",
                "-pkeyopt", "ec_param_enc:named_curve",
                "-nodes", "-keyout", str(key), "-out", str(cert), "-days", "30",
                "-subj", f"/CN={hostname}", "-addext", f"subjectAltName=DNS:{hostname}",
                "-addext", "basicConstraints=critical,CA:TRUE",
            ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            key.chmod(0o600)
            public = subprocess.check_output(["openssl", "pkey", "-in", str(key), "-pubout", "-outform", "DER"])
            pins.append(hashlib.sha256(public).hexdigest())
        profile = {"hostname": hostname, "ipv4": ipv4, "port": port,
                   "current_spki_sha256": pins[0], "next_spki_sha256": pins[1]}
        # Use the same validation as the ROM build before installing anything.
        import importlib.util
        spec = importlib.util.spec_from_file_location("connection", ROOT / "scripts/configure-connection.py")
        connection = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(connection)
        connection.header(profile)
        (stage / "connection.json").write_text(json.dumps(profile, indent=2) + "\n")
        stage.rename(directory)
    print(f"Created local peer keys and public profile in {directory}")


def handle_peer(raw, context, args):
    raw.settimeout(35)
    try:
        with context.wrap_socket(raw, server_side=True) as peer:
            request = b""
            while b"\r\n\r\n" not in request and len(request) < 2048:
                data = peer.recv(min(512, 2048 - len(request)))
                if not data:
                    break
                request += data
            if not request.startswith(b"GET /v1/ping HTTP/1.1\r\n") or b"\r\n\r\n" not in request:
                print("Rejected request", flush=True)
                return
            if args.stall:
                time.sleep(8)
            response = (b"HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
                        b"Content-Length: 12\r\nConnection: close\r\n\r\nDSWT_ACK_V1\n")
            peer.sendall(response[:-1] if args.truncate else response)
            print("Authenticated ping handled", flush=True)
    except (OSError, ssl.SSLError):
        raw.close()
        print("Connection ended before a complete authenticated ping", flush=True)


def serve(args):
    profile = json.loads((args.directory / "connection.json").read_text())
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.minimum_version = context.maximum_version = ssl.TLSVersion.TLSv1_2
    context.set_ciphers("ECDHE-ECDSA-CHACHA20-POLY1305")
    context.load_cert_chain(args.directory / f"{args.key}.pem", args.directory / f"{args.key}.key")
    with socket.socket() as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind((args.bind, profile["port"]))
        listener.listen(4)

        def worker():
            while True:
                raw, _ = listener.accept()
                handle_peer(raw, context, args)

        # Four fixed workers prevent one slow handshake from blocking the other client.
        for _ in range(3):
            threading.Thread(target=worker, daemon=True).start()
        print(f"Ping fixture listening on port {profile['port']}, key={args.key}", flush=True)
        worker()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    create = sub.add_parser("prepare")
    create.add_argument("--directory", type=Path, required=True)
    create.add_argument("--ipv4", required=True, help="IPv4 address reachable from the DS")
    create.add_argument("--hostname", default="localhost")
    create.add_argument("--port", type=int, default=8443)
    run = sub.add_parser("serve")
    run.add_argument("--directory", type=Path, required=True)
    run.add_argument("--bind", default="127.0.0.1", help="use 0.0.0.0 explicitly for a local device test")
    run.add_argument("--key", choices=("current", "next", "untrusted"), default="current")
    run.add_argument("--stall", action="store_true")
    run.add_argument("--truncate", action="store_true")
    args = parser.parse_args()
    try:
        if not args.directory.resolve().is_relative_to((ROOT / "game").resolve()):
            raise ValueError("Keep test keys in the ignored game directory")
        if args.command == "prepare":
            prepare(args.directory, args.ipv4, args.hostname, args.port)
        else:
            serve(args)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"TLS fixture failed: {error}\n")
