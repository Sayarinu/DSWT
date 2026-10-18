import importlib.util
import json
from pathlib import Path
import struct
import sys
import unittest


ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("wire_reference", ROOT / "protocol/wire.py")
wire = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = wire
spec.loader.exec_module(wire)


class WireTests(unittest.TestCase):
    def request(self, operation=wire.Operation.STATUS, **changes):
        payload = bytes(wire.REQUEST_SIZES.get(operation, 5))
        if operation == wire.Operation.RECONCILE:
            payload = struct.pack("<QB", 0, 8)
        values = dict(
            operation=operation, request_id=b"\x01" * 16,
            save_id=bytes(16) if operation == wire.Operation.ENROLL else b"\x02" * 16,
            credential=b"\x03" * 32,
            counter=1 if operation in (wire.Operation.SUBMIT, wire.Operation.CANCEL, wire.Operation.ACKNOWLEDGE) else 0,
            payload=payload,
        )
        values.update(changes)
        return wire.Request(**values)

    def test_fixed_status_vector_and_header_sizes(self):
        expected = bytes.fromhex(
            "5754445301031000" + "01" * 16 + "02" * 16 + "03" * 32
            + "00" * 8 + "00" * 16
        )
        self.assertEqual(wire.REQUEST.size, 80)
        self.assertEqual(wire.RESPONSE.size, 44)
        self.assertEqual(wire.encode_request(self.request()), expected)
        self.assertEqual(wire.decode_request(expected), self.request())

    def test_all_operations_round_trip(self):
        for operation in wire.Operation:
            with self.subTest(operation=operation):
                request = self.request(operation)
                self.assertEqual(wire.decode_request(wire.encode_request(request)), request)
                payload = bytes(wire.RESPONSE_SIZES.get(operation, 10))
                response = wire.Response(operation, request.request_id, wire.Result.OK, 2, 1, payload)
                self.assertEqual(wire.decode_response(wire.encode_response(response)), response)

    def test_truncation_trailing_bytes_and_unknown_headers_fail_closed(self):
        packet = wire.encode_request(self.request())
        mutations = [packet[:i] for i in range(len(packet))]
        mutations += [packet + b"extra", bytes(wire.MAX_PACKET + 1)]
        for offset, value in ((0, 0), (4, 2), (5, 255), (6, 0)):
            changed = bytearray(packet)
            changed[offset] = value
            mutations.append(bytes(changed))
        for changed in mutations:
            with self.subTest(size=len(changed)):
                with self.assertRaises(ValueError):
                    wire.decode_request(changed)

    def test_identifier_padding_and_authentication_shape_are_rejected(self):
        for changes in (
            {"request_id": b"x" * 17}, {"request_id": bytes(16)},
            {"save_id": bytes(16)}, {"credential": bytes(32)},
            {"credential": b"x" * 33}, {"counter": 1},
        ):
            with self.subTest(changes=list(changes)):
                with self.assertRaises(ValueError):
                    wire.encode_request(self.request(**changes))
        for counter in (0, (1 << 64) - 1):
            with self.assertRaises(ValueError):
                wire.encode_request(self.request(wire.Operation.SUBMIT, counter=counter))
        with self.assertRaises(ValueError):
            wire.encode_request(self.request(wire.Operation.ENROLL, save_id=b"x" * 16))

    def test_hint_and_page_bounds(self):
        for length in (0, 64):
            request = self.request(wire.Operation.ENROLL, payload=struct.pack("<IB", 1, length) + b"x" * length)
            self.assertEqual(wire.decode_request(wire.encode_request(request)), request)
        for payload in (bytes(4), struct.pack("<IB", 1, 65) + b"x" * 65, struct.pack("<IB", 1, 4)):
            with self.assertRaises(ValueError):
                wire.encode_request(self.request(wire.Operation.ENROLL, payload=payload))
        for limit in (0, 9):
            with self.assertRaises(ValueError):
                wire.encode_request(self.request(wire.Operation.RECONCILE, payload=struct.pack("<QB", 0, limit)))
        for count in (0, 8):
            payload = struct.pack("<BBQ", count, 0, count) + bytes(count * wire.EVENT_SIZE)
            response = wire.Response(wire.Operation.RECONCILE, b"x" * 16, wire.Result.OK, 1, count, payload)
            encoded = wire.encode_response(response)
            self.assertEqual(wire.decode_response(encoded), response)
            self.assertLessEqual(len(encoded), 902)
        for payload in (bytes(9), struct.pack("<BBQ", 9, 0, 9) + bytes(9 * wire.EVENT_SIZE), struct.pack("<BBQ", 0, 2, 0)):
            with self.assertRaises(ValueError):
                wire.encode_response(wire.Response(wire.Operation.RECONCILE, b"x" * 16, wire.Result.OK, 1, 0, payload))

    def test_error_replies_cannot_be_mistaken_for_success_payloads(self):
        for result in wire.Result:
            if result == wire.Result.OK:
                continue
            response = wire.Response(wire.Operation.SUBMIT, b"x" * 16, result, 0, 0, b"")
            self.assertEqual(wire.decode_response(wire.encode_response(response)), response)
            with self.assertRaises(ValueError):
                wire.encode_response(wire.Response(wire.Operation.SUBMIT, b"x" * 16, result, 0, 0, b"unexpected"))
        response = bytearray(wire.encode_response(wire.Response(wire.Operation.STATUS, b"x" * 16, wire.Result.OK, 1, 0, bytes(wire.SNAPSHOT_SIZE))))
        for offset, value in ((24, 0), (24, 255), (26, 1)):
            changed = bytearray(response)
            changed[offset] = value
            with self.assertRaises(ValueError):
                wire.decode_response(bytes(changed))

    def test_debug_repr_does_not_expose_credentials_or_payloads(self):
        request = self.request(credential=b"private credential material!!!!!", payload=b"sensitive bytes!")
        self.assertNotIn("private credential", repr(request))
        self.assertNotIn("sensitive bytes", repr(request))
        self.assertNotIn("save_id=", repr(request))


class SaveLayoutTests(unittest.TestCase):
    def test_journal_erase_sectors_do_not_overlap_any_retail_block(self):
        layout = json.loads((ROOT / "protocol/save-layout.json").read_text())
        pin = json.loads((ROOT / "client/upstream.json").read_text())
        self.assertEqual(layout["upstream_revision"], pin["revision"])
        ranges = []
        for half in (0, layout["half_size"]):
            for region in layout["retail_regions_per_half"]:
                ranges.append((half + region["offset"], half + region["offset"] + region["size"], region["name"]))
        for bank in layout["journal_banks"]:
            self.assertEqual(bank % layout["bank_reserved_size"], 0)
            ranges.append((bank, bank + layout["bank_reserved_size"], "journal"))
        ranges.sort()
        for start, end, _ in ranges:
            self.assertTrue(0 <= start < end <= layout["flash_size"])
        for previous, current in zip(ranges, ranges[1:]):
            self.assertLessEqual(previous[1], current[0], (previous, current))
        sections = layout["record_sections"]
        end = 0
        for section in sections:
            self.assertEqual(section["offset"], end)
            self.assertGreater(section["size"], 0)
            end += section["size"]
        self.assertEqual(end, layout["record_size"])
        self.assertLessEqual(end, layout["bank_reserved_size"])
        sizes = {section["name"]: section["size"] for section in sections}
        self.assertGreaterEqual(sizes["pokemon_images"], 3 * 236 + 2 * 136)
        self.assertGreaterEqual(sizes["identity"], 16 + 32 + 16)
        self.assertGreaterEqual(sizes["pending_request"], 8 + wire.REQUEST.size + max(wire.REQUEST_SIZES.values()))


if __name__ == "__main__":
    unittest.main()
