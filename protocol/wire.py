"""Reference v1 envelope codec for conformance tests and integration clients.

This does not provide a transport, authentication service or legality verdict.
"""

from dataclasses import dataclass, field
from enum import IntEnum
import struct


class Operation(IntEnum):
    ENROLL = 1
    SUBMIT = 2
    STATUS = 3
    CANCEL = 4
    COLLECT = 5
    ACKNOWLEDGE = 6
    RECONCILE = 7


class Result(IntEnum):
    OK = 1
    PROCESSING = 2
    INVALID_REQUEST = 3
    UNAUTHENTICATED = 4
    CONFLICT = 5
    NOT_FOUND = 6
    CAPACITY = 7
    ILLEGAL_POKEMON = 8
    COUNTER_MISMATCH = 9
    CURSOR_AHEAD = 10
    UNAVAILABLE = 11


REQUEST = struct.Struct("<4sBBH16s16s32sQ")
RESPONSE = struct.Struct("<4sBBH16sHHQQ")
MAX_PACKET = 4096
SNAPSHOT_SIZE = 137
EVENT_SIZE = 106
REQUEST_SIZES = {
    Operation.SUBMIT: 176, Operation.STATUS: 16, Operation.CANCEL: 16,
    Operation.COLLECT: 16, Operation.ACKNOWLEDGE: 104, Operation.RECONCILE: 9,
}
RESPONSE_SIZES = {
    Operation.ENROLL: 16, Operation.SUBMIT: 145, Operation.STATUS: SNAPSHOT_SIZE,
    Operation.CANCEL: 145, Operation.COLLECT: SNAPSHOT_SIZE + 136,
    Operation.ACKNOWLEDGE: 145,
}


@dataclass(frozen=True)
class Request:
    operation: Operation
    request_id: bytes
    save_id: bytes = field(repr=False)
    credential: bytes = field(repr=False)
    counter: int
    payload: bytes = field(repr=False)


@dataclass(frozen=True)
class Response:
    operation: Operation
    request_id: bytes
    result: Result
    next_counter: int
    event_highwater: int
    payload: bytes = field(repr=False)


def _header(packet, header):
    if not header.size <= len(packet) <= MAX_PACKET:
        raise ValueError("Invalid packet size")
    values = header.unpack_from(packet)
    if values[0] != b"WTDS" or values[1] != 1:
        raise ValueError("Unsupported magic or version")
    if values[3] != len(packet) - header.size:
        raise ValueError("Payload length mismatch")
    return values, packet[header.size:]


def decode_request(packet):
    values, payload = _header(packet, REQUEST)
    _, _, opcode, _, request_id, save_id, credential, counter = values
    operation = Operation(opcode)
    if not any(request_id) or not any(credential):
        raise ValueError("Missing request ID or credential")
    if (operation == Operation.ENROLL) != (not any(save_id)):
        raise ValueError("Invalid save identity")
    mutates = operation in (Operation.SUBMIT, Operation.CANCEL, Operation.ACKNOWLEDGE)
    if mutates != (counter != 0) or counter == (1 << 64) - 1:
        raise ValueError("Invalid mutation counter")
    if operation == Operation.ENROLL:
        if not 5 <= len(payload) <= 69 or payload[4] != len(payload) - 5:
            raise ValueError("Invalid enrollment hints")
    elif len(payload) != REQUEST_SIZES[operation]:
        raise ValueError("Invalid operation payload size")
    if operation == Operation.RECONCILE and not 1 <= payload[8] <= 8:
        raise ValueError("Invalid reconciliation limit")
    return Request(operation, request_id, save_id, credential, counter, payload)


def encode_request(request):
    for value, size in ((request.request_id, 16), (request.save_id, 16), (request.credential, 32)):
        if len(value) != size:
            raise ValueError("Invalid identifier or credential length")
    packet = REQUEST.pack(
        b"WTDS", 1, request.operation, len(request.payload), request.request_id,
        request.save_id, request.credential, request.counter,
    ) + request.payload
    decode_request(packet)
    return packet


def decode_response(packet):
    values, payload = _header(packet, RESPONSE)
    _, _, opcode, _, request_id, result, reserved, counter, highwater = values
    operation, result = Operation(opcode), Result(result)
    if reserved or not any(request_id):
        raise ValueError("Invalid response header")
    if result != Result.OK:
        if payload:
            raise ValueError("Only successful responses have a payload")
    elif operation == Operation.RECONCILE:
        if len(payload) < 10:
            raise ValueError("Truncated reconciliation page")
        count, has_more = payload[:2]
        if count > 8 or has_more > 1 or len(payload) != 10 + count * EVENT_SIZE:
            raise ValueError("Invalid reconciliation page size")
    elif len(payload) != RESPONSE_SIZES[operation]:
        raise ValueError("Invalid response payload size")
    return Response(operation, request_id, result, counter, highwater, payload)


def encode_response(response):
    if len(response.request_id) != 16:
        raise ValueError("Invalid request ID length")
    packet = RESPONSE.pack(
        b"WTDS", 1, response.operation, len(response.payload), response.request_id,
        response.result, 0, response.next_counter, response.event_highwater,
    ) + response.payload
    decode_response(packet)
    return packet
