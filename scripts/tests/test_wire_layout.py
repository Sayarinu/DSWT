import importlib.util
from pathlib import Path
import re
import struct
import sys
import unittest


ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("wire_layout_reference", ROOT / "protocol/wire.py")
wire = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = wire
spec.loader.exec_module(wire)


def client_layout():
    text = (ROOT / "client/include/wonder_trade/wire_layout.h").read_text()
    return {name: int(value, 0) for name, value in re.findall(r"^#define WT_WIRE_(\w+) (\d\w*)$", text, re.M)}


def server_layout():
    text = (ROOT / "server/src/wire_layout.h").read_text()
    values = dict(re.findall(r"\b(k\w+) = (\w+)[,;]", text))
    return {name: int(value, 0) for name, value in values.items()}


def server_name(client_name):
    return "k" + "".join(part.capitalize() for part in client_name.split("_"))


def offsets(fmt, names):
    """Offsets of each named field in a little-endian struct format, plus its total size."""
    codes = re.findall(r"\d*[a-zA-Z]", fmt.lstrip("<"))
    result = {name: struct.calcsize("<" + "".join(codes[:i])) for i, name in enumerate(names) if name}
    result["SIZE"] = struct.calcsize(fmt)
    return result


class WireLayoutTests(unittest.TestCase):
    def test_client_and_server_name_the_same_layout(self):
        client = client_layout()
        server = server_layout()
        operations = {"kEnroll", "kSubmit", "kStatus", "kCancel", "kCollect", "kAcknowledge", "kReconcile"}
        self.assertGreater(len(client), 60)
        self.assertEqual({server_name(name): value for name, value in client.items() if name != "MAGIC_SIZE"},
                         {name: value for name, value in server.items() if name not in operations | {"kMaxPacket"}})
        self.assertEqual({name: server[name] for name in operations},
                         {server_name(op.name.lower()): int(op) for op in wire.Operation})
        self.assertEqual(server["kMaxPacket"], wire.MAX_PACKET)
        magic = '"WTDS\\1"'
        self.assertIn(f"#define WT_WIRE_MAGIC {magic}", (ROOT / "client/include/wonder_trade/wire_layout.h").read_text())
        self.assertIn(f"kMagic{{{magic}, 5}}", (ROOT / "server/src/wire_layout.h").read_text())

    def test_layout_matches_the_reference_codec(self):
        c = client_layout()
        self.assertEqual(c["MAGIC_SIZE"], 5)
        request = offsets(wire.REQUEST.format, [None, None, "OPERATION", "BODY_LENGTH", "ID", "SAVE_ID", "CREDENTIAL", "COUNTER"])
        self.assertEqual({k: c["REQUEST_HEADER_SIZE" if k == "SIZE" else "REQUEST_" + k] for k in request}, request)
        reply = offsets(wire.RESPONSE.format,
                        [None, None, "OPERATION", "BODY_LENGTH", "REQUEST_ID", "RESULT", "RESERVED", "NEXT_COUNTER", "HIGHWATER"])
        self.assertEqual({k: c["REPLY_HEADER_SIZE" if k == "SIZE" else "REPLY_" + k] for k in reply}, reply)

        snapshot = offsets("<16sBB16s32s16s32s16sQ", ["TRADE", "STATE", "DELIVERY_KIND", "OFFERED_ASSET", "OFFERED_DIGEST",
                                                      "RECEIVED_ASSET", "RECEIVED_DIGEST", "RECEIPT", "LAST_EVENT"])
        self.assertEqual({k: c["SNAPSHOT_" + k] for k in snapshot}, snapshot)
        self.assertEqual(c["SNAPSHOT_SIZE"], wire.SNAPSHOT_SIZE)
        event = offsets("<QB16s16s16s32s16sB", ["SEQUENCE", "KIND", "TRADE", "OPERATION", "ASSET", "DIGEST", "RECEIPT", "STATE"])
        self.assertEqual({k: c["EVENT_" + k] for k in event}, event)
        self.assertEqual(c["EVENT_SIZE"], wire.EVENT_SIZE)

        submit = offsets("<Q136s32s", ["GENERATION", "POKEMON", "DIGEST"])
        self.assertEqual({k: c["SUBMIT_" + k] for k in submit}, submit)
        ack = offsets("<16s16s32s32sQ", ["TRADE", "RECEIPT", "DELIVERED_DIGEST", "APPLIED_DIGEST", "GENERATION"])
        self.assertEqual({k: c["ACK_" + k] for k in ack}, ack)
        reconcile = offsets("<QB", ["AFTER", "LIMIT"])
        self.assertEqual({k: c["RECONCILE_" + k] for k in reconcile}, reconcile)
        self.assertEqual((c["ENROLL_BUILD"], c["ENROLL_HINT_LENGTH"], c["ENROLL_HINTS"]), (0, 4, 5))
        sizes = {wire.Operation.SUBMIT: c["SUBMIT_SIZE"], wire.Operation.STATUS: c["TRADE_REQUEST_SIZE"],
                 wire.Operation.CANCEL: c["TRADE_REQUEST_SIZE"], wire.Operation.COLLECT: c["TRADE_REQUEST_SIZE"],
                 wire.Operation.ACKNOWLEDGE: c["ACK_SIZE"], wire.Operation.RECONCILE: c["RECONCILE_SIZE"]}
        self.assertEqual(sizes, wire.REQUEST_SIZES)

        self.assertEqual((c["OUTCOME_EVENT"], c["OUTCOME_SNAPSHOT"], c["COLLECTION_SNAPSHOT"], c["COLLECTION_POKEMON"]),
                         (0, 8, 0, wire.SNAPSHOT_SIZE))
        self.assertEqual(c["OUTCOME_SIZE"], wire.RESPONSE_SIZES[wire.Operation.SUBMIT])
        self.assertEqual(c["COLLECTION_SIZE"], wire.RESPONSE_SIZES[wire.Operation.COLLECT])
        self.assertEqual(c["ID_SIZE"], wire.RESPONSE_SIZES[wire.Operation.ENROLL])
        page = offsets("<BBQ", ["COUNT", "HAS_MORE", "END"])
        self.assertEqual((c["PAGE_COUNT"], c["PAGE_HAS_MORE"], c["PAGE_END"], c["PAGE_EVENTS"]),
                         (page["COUNT"], page["HAS_MORE"], page["END"], page["SIZE"]))
        self.assertEqual((c["PK4_SIZE"], c["PK4_SANITY"], c["PK4_CHECKSUM"], c["PK4_BLOCKS"]), (136, 4, 6, 8))

    def test_client_bounds_cover_the_largest_frames(self):
        header = (ROOT / "client/include/wonder_trade/wire.h").read_text()
        request_max = int(re.search(r"#define WT_WIRE_REQUEST_MAX (\d+)", header)[1])
        reply_max = int(re.search(r"#define WT_WIRE_REPLY_MAX (\d+)", header)[1])
        c = client_layout()
        largest_request = max(c["REQUEST_HEADER_SIZE"] + size for size in wire.REQUEST_SIZES.values())
        self.assertLessEqual(largest_request, request_max)
        self.assertEqual(reply_max, c["REPLY_HEADER_SIZE"] + c["PAGE_EVENTS"] + c["RECONCILE_LIMIT_MAX"] * c["EVENT_SIZE"])


if __name__ == "__main__":
    unittest.main()
