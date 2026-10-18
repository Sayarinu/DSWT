"""Cross-language codec checks using only synthetic protocol values."""

import hashlib
import importlib.util
from pathlib import Path
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("wire_oracle", ROOT / "protocol/wire.py")
wire = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = wire
spec.loader.exec_module(wire)
ZERO = bytes(16)
SNAPSHOT = struct.Struct("<16sBB16s32s16s32s16sQ")


def pokemon(species):
    data = bytearray(136)
    struct.pack_into("<I", data, 0, 0x12345678)
    struct.pack_into("<H", data, 6, species)
    struct.pack_into("<H", data, 8, species)
    return bytes(data)


def snapshot(state, kind, event):
    offered, incoming = hashlib.sha256(pokemon(25)).digest(), hashlib.sha256(pokemon(2)).digest()
    return SNAPSHOT.pack(b"\x44" * 16, state, kind, b"\x55" * 16, offered,
                         ZERO if not kind else b"\x55" * 16 if kind == 2 else b"\x66" * 16,
                         bytes(32) if not kind else offered if kind == 2 else incoming,
                         ZERO if not kind else b"\x77" * 16, event)


def event(sequence, kind):
    empty = kind == 1
    rejected = kind == 6
    return struct.pack("<QB16s16s16s32s16sB", sequence, kind,
                       ZERO if empty or rejected else b"\x44" * 16,
                       ZERO if empty or kind == 3 else b"\x11" * 16,
                       ZERO if empty or rejected else b"\x55" * 16,
                       bytes(32) if empty or rejected else b"\x88" * 32,
                       ZERO if empty or rejected or kind == 2 else b"\x77" * 16,
                       0 if empty or rejected else kind - 1)


def run(binary):
    incoming_digest = hashlib.sha256(pokemon(2)).digest()
    bodies = {
        1: struct.pack("<IB", 0x01020304, 0),
        2: struct.pack("<Q", 7) + pokemon(25) + hashlib.sha256(pokemon(25)).digest(),
        3: ZERO, 4: b"\x44" * 16, 5: b"\x44" * 16,
        6: b"\x44" * 16 + b"\x77" * 16 + incoming_digest + b"\x99" * 32 + struct.pack("<Q", 12),
        7: struct.pack("<QB", 0, 8),
    }
    requests = {op: wire.encode_request(wire.Request(
        wire.Operation(op), b"\x11" * 16, ZERO if op == 1 else b"\x22" * 16,
        b"\x33" * 32, 1 if op in (2, 4, 6) else 0, body)) for op, body in bodies.items()}
    encoded = subprocess.check_output([binary, "requests"], timeout=10)
    for operation in range(1, 8):
        size, = struct.unpack_from("<H", encoded)
        actual, encoded = encoded[2:2 + size], encoded[2 + size:]
        assert actual == requests[operation], f"Independent request bytes differ for operation {operation}"
        assert wire.decode_request(actual).operation == operation
    assert not encoded

    page = struct.pack("<BBQ", 8, 0, 8) + b"".join(event(i + 1, kind) for i, kind in enumerate((1, 2, 3, 5, 2, 4, 5, 6)))
    replies = {
        1: (1, 1, b"\x22" * 16),
        2: (2, 2, struct.pack("<Q", 2) + snapshot(1, 0, 2)),
        3: (2, 3, snapshot(2, 1, 3)),
        4: (2, 3, struct.pack("<Q", 3) + snapshot(3, 2, 3)),
        5: (2, 3, snapshot(2, 1, 3) + pokemon(2)),
        6: (2, 4, struct.pack("<Q", 4) + snapshot(4, 1, 4)),
        7: (2, 8, page),
    }
    cases = []

    def add(request, reply, expected):
        cases.append((request, reply, expected))

    for operation, (counter, highwater, body) in replies.items():
        reply = wire.encode_response(wire.Response(
            wire.Operation(operation), b"\x11" * 16, wire.Result.OK, counter, highwater, body))
        add(requests[operation], reply, 1)
        for offset, value in ((4, 2), (5, 0), (8, 0x12), (24, 12), (26, 1)):
            changed = bytearray(reply)
            changed[offset] = value
            add(requests[operation], changed, 0)
        add(requests[operation], reply[:-1], 0)
        if len(reply) < 902:
            add(requests[operation], reply + b"\0", 0)
        for result in list(wire.Result)[1:]:
            response = wire.Response(wire.Operation(operation), b"\x11" * 16, result, 0, 0, b"")
            add(requests[operation], wire.encode_response(response), result)
        if operation in (2, 4, 6):
            # A retained immutable outcome can carry newer live counter/highwater.
            changed = bytearray(reply)
            struct.pack_into("<QQ", changed, 28, 99, 101)
            add(requests[operation], changed, 1)
        if operation in (4, 5, 6):
            changed = bytearray(reply)
            changed[44 + (8 if operation in (4, 6) else 0)] ^= 1
            add(requests[operation], changed, 0)
    # Identity enrollment is not accepted with an absent server ID.
    enrollment = wire.encode_response(wire.Response(wire.Operation.ENROLL, b"\x11" * 16, wire.Result.OK, 1, 1, ZERO))
    add(requests[1], enrollment, 0)
    data = b"".join(struct.pack("<HH", len(request), len(reply)) + request + reply for request, reply, _ in cases)
    decoded = subprocess.run([binary, "decode"], input=data, capture_output=True, timeout=20, check=True)
    assert decoded.stdout == bytes(expected for _, _, expected in cases), "Independent response-vector results differ"
    print(f"Seven independent request encodings and {len(cases)} response/binding vectors passed")


if __name__ == "__main__":
    run(sys.argv[1])
