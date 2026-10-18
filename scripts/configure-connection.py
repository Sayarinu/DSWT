#!/usr/bin/env python3
"""Validate public endpoint/pins and emit the game build's connection header."""

import argparse
import ipaddress
import json
from pathlib import Path
import re
import sys


def header(profile):
    if set(profile) != {"hostname", "ipv4", "port", "current_spki_sha256", "next_spki_sha256"}:
        raise ValueError("Profile must contain exactly hostname, ipv4, port, current_spki_sha256 and next_spki_sha256")
    hostname = profile["hostname"]
    if not isinstance(hostname, str) or len(hostname) > 253 or not re.fullmatch(r"[A-Za-z0-9]+(?:[A-Za-z0-9.-]*[A-Za-z0-9])?", hostname):
        raise ValueError("Invalid TLS hostname")
    address = ipaddress.IPv4Address(profile["ipv4"])
    if address.is_unspecified or address.is_multicast or int(address) == 0xffffffff:
        raise ValueError("Use a unicast IPv4 server address")
    port = profile["port"]
    if type(port) is not int or not 1 <= port <= 65535:
        raise ValueError("Port must be 1..65535")
    pins = []
    for name in ("current_spki_sha256", "next_spki_sha256"):
        value = profile[name]
        if not isinstance(value, str) or not re.fullmatch(r"[0-9a-fA-F]{64}", value) or int(value, 16) == 0:
            raise ValueError(f"{name} must be a nonzero SHA-256 of the DER SubjectPublicKeyInfo")
        pins.append("{ " + ", ".join(f"0x{byte:02x}" for byte in bytes.fromhex(value)) + " }")
    if pins[0] == pins[1]:
        raise ValueError("Current and next public keys must be distinct")
    return (
        "/* Generated from a local public connection profile. No private keys or save seeds. */\n"
        f'#define WONDER_TRADE_HOSTNAME "{hostname}"\n'
        f'#define WONDER_TRADE_AUTHORITY "{hostname}:{port}"\n'
        f"#define WONDER_TRADE_IPV4 0x{int(address):08x}UL\n"
        f"#define WONDER_TRADE_PORT {port}\n"
        f"#define WONDER_TRADE_PINS {{ {', '.join(pins)} }}\n"
    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("profile", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    try:
        content = header(json.loads(args.profile.read_text()))
        if args.output.is_symlink():
            raise ValueError("Refusing a symlink output")
        args.output.write_text(content)
    except (OSError, ValueError, TypeError) as error:
        parser.exit(1, f"Cannot configure connection: {error}\n")
