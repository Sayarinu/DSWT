"""Generated structural fixtures only; these are not legal game encounters."""
import fcntl
import hashlib
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('verify_trade', Path(__file__).resolve().parents[1]/'verify-demo-trade.py')
verify = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verify)


def pk(species, pid):
    result = bytearray(136)
    struct.pack_into('<IHHHH', result, 0, pid, 0, 0, species, 10+species)
    result[20] = 200
    struct.pack_into('<H', result, 6, sum(struct.unpack_from('<64H', result, 8)) & 65535)
    return bytes(result)


def encrypt(canonical):
    pid, _, checksum = struct.unpack_from('<IHH', canonical)
    order = verify.ORDERS[((pid >> 13) & 31) % 24]
    shuffled = b''.join(canonical[8+i*32:40+i*32] for i in order)
    result = bytearray(canonical[:8]+shuffled)
    seed = checksum
    for offset in range(8, 136, 2):
        seed = (seed*0x41C64E6D+0x6073) & 0xFFFFFFFF
        struct.pack_into('<H', result, offset, struct.unpack_from('<H', result, offset)[0] ^ (seed >> 16))
    return result


def retail(party, box):
    raw = bytearray(verify.SIZE)
    for bank in (0, 0x40000):
        struct.pack_into('<II', raw, bank+152, 6, len(party))
        for i, mon in enumerate(party):
            raw[bank+160+i*236:bank+296+i*236] = encrypt(mon)
        for i in range(540):
            raw[bank+53040+i*136:bank+53176+i*136] = encrypt(box if i == 0 else bytes(136))
        for block, (offset, size) in enumerate(verify.BLOCKS):
            struct.pack_into('<IIIIBBH', raw, bank+offset+size-20, 10+bool(bank), 20+bool(bank), size,
                             0x20060623, block, 0, verify.crc(raw[bank+offset:bank+offset+size-20]))
    return raw


def idle(raw, identity, counter=3, cursor=4, generation=12, receipt=None):
    journal = bytearray(4096)
    journal[:6] = b'PWTJ\1\0'
    struct.pack_into('<HI', journal, 6, 2, 4096)
    struct.pack_into('<Q', journal, 16, generation)
    journal[32:48] = bytes([identity])*16
    journal[48:80] = bytes([identity+10])*32
    journal[80:96] = bytes([identity+20])*16
    struct.pack_into('<QQ', journal, 96, counter, cursor)
    journal[112:128] = bytes([identity+30 if receipt is None else receipt])*16
    if counter > 1:
        journal[128:144] = bytes([identity+40])*16
    struct.pack_into('<Q', journal, 144, counter-1)
    journal[1440:1444] = b'\1\0\x20\0'
    journal[1444:1476] = bytes([identity+50])*32
    journal[4060:4092] = verify.sha(journal[:4060])
    journal[4092:] = b'CMIT'
    for bank in (0x30000, 0x70000):
        raw[bank:bank+4096] = journal


def reseal(raw, offset, value):
    record = bytearray(raw[0x30000:0x31000])
    record[offset:offset+len(value)] = value
    record[4060:4092] = verify.sha(record[:4060])
    for bank in (0x30000, 0x70000):
        raw[bank:bank+4096] = record


def publication(path, raw, lineage):
    path.write_bytes(raw)
    Path(str(path)+'.platinumwt.lock').touch()
    store = Path(str(path)+'.platinumwt')
    store.mkdir(exist_ok=True)
    fields = struct.pack('<Q', 8)+bytes([lineage])*16+verify.sha(raw)
    snapshot = b'PWTHS001'+fields+raw
    snapshot += verify.sha(snapshot)
    head = b'PWTHD001'+fields+bytes(32)
    head += verify.sha(head)
    for name, data in [('A', snapshot), ('B', snapshot), ('HEAD', head)]:
        (store/name).write_bytes(data)


class DemoTradeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.before = [self.root/'before-a.sav', self.root/'before-b.sav']
        self.after = [self.root/'after-a.sav', self.root/'after-b.sav']
        self.parties = [[pk(10+i, 0x6000+i) for i in range(3)], [pk(50+i, 0xC000+i) for i in range(3)]]
        self.boxes = [pk(100, 7), pk(101, 8)]
        for i in range(2):
            raw = retail(self.parties[i], self.boxes[i])
            self.before[i].write_bytes(raw)
            party = self.parties[i].copy()
            party.pop(i)
            party.append(verify.traded(self.parties[1-i][1-i]))
            raw = retail(party, self.boxes[i])
            idle(raw, i+1)
            publication(self.after[i], raw, i+1)

    def test_reciprocal_inventory_and_private_inputs_remain_unchanged(self):
        hashes = [verify.sha(p.read_bytes()) for p in self.before+self.after]
        result = verify.verify(self.before, self.after, [0, 1])
        self.assertTrue(result['verified'])
        self.assertEqual(hashes, [verify.sha(p.read_bytes()) for p in self.before+self.after])
        self.assertNotIn('identity', str(result))
        self.assertNotIn('credential', str(result))

    def test_extra_received_copy_is_rejected_even_with_valid_crcs_and_journal(self):
        party = self.parties[0][1:]+[verify.traded(self.parties[1][1])]*2
        raw = retail(party, self.boxes[0]); idle(raw, 1); publication(self.after[0], raw, 1)
        with self.assertRaisesRegex(ValueError, 'beyond the reciprocal exchange'):
            verify.verify(self.before, self.after, [0, 1])

    def test_changed_held_item_is_rejected(self):
        mon = bytearray(verify.traded(self.parties[1][1])); mon[10] ^= 1
        struct.pack_into('<H', mon, 6, sum(struct.unpack_from('<64H', mon, 8)) & 65535)
        raw = retail(self.parties[0][1:]+[bytes(mon)], self.boxes[0]); idle(raw, 1); publication(self.after[0], raw, 1)
        with self.assertRaisesRegex(ValueError, 'beyond the reciprocal exchange'):
            verify.verify(self.before, self.after, [0, 1])

    def test_unacknowledged_or_replayed_mutation_count_is_rejected(self):
        raw = bytearray(self.after[0].read_bytes()); idle(raw, 1, 2); publication(self.after[0], raw, 1)
        with self.assertRaisesRegex(ValueError, 'mutation count|IDLE progress'):
            verify.verify(self.before, self.after, [0, 1])

    def test_torn_journal_and_stale_host_snapshot_are_rejected(self):
        raw = bytearray(self.after[0].read_bytes()); raw[0x30000+4092] = 0
        self.after[0].write_bytes(raw)
        with self.assertRaisesRegex(ValueError, 'Journal mirrors'):
            verify.verify(self.before, self.after, [0, 1])
        idle(raw, 1); self.after[0].write_bytes(raw)
        store = Path(str(self.after[0])+'.platinumwt')
        (store/'B').write_bytes(bytes(verify.SIZE+96))
        with self.assertRaisesRegex(ValueError, 'host snapshots'):
            verify.verify(self.before, self.after, [0, 1])

    def test_same_identity_and_running_emulator_are_rejected(self):
        raw = bytearray(self.after[1].read_bytes()); idle(raw, 1); publication(self.after[1], raw, 2)
        with self.assertRaisesRegex(ValueError, 'not independent'):
            verify.verify(self.before, self.after, [0, 1])
        with Path(str(self.after[0])+'.platinumwt.lock').open('rb') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            with self.assertRaises(BlockingIOError):
                verify.verify(self.before, self.after, [0, 1])

    def test_resealed_unknown_layouts_and_bank_tails_are_rejected(self):
        original = self.after[0].read_bytes()
        for offset in (12, 24, 152, 397, 1396, 1440, 1442, 1476, 1954, 2216):
            with self.subTest(offset=offset):
                raw = bytearray(original); reseal(raw, offset, b'\x02')
                with self.assertRaisesRegex(ValueError, 'layout|transaction evidence'):
                    verify.journal(raw)
        raw = bytearray(original); raw[0x31000] = 1
        with self.assertRaisesRegex(ValueError, 'bank tail'):
            verify.journal(raw)

    def test_seeded_empty_baseline_requires_known_layout_and_state(self):
        raw = bytearray(self.after[0].read_bytes())
        reseal(raw, 6, bytes(2)); reseal(raw, 32, bytes(1408))
        self.assertIsNone(verify.journal(raw, baseline=True))
        for offset, value in ((1440, b'\x02'), (1476, b'\x01'), (1444, bytes(32)), (32, b'\x01')):
            with self.subTest(offset=offset):
                changed = bytearray(raw); reseal(changed, offset, value)
                with self.assertRaisesRegex(ValueError, 'layout|entropy|EMPTY'):
                    verify.journal(changed, baseline=True)
        blank = bytearray(self.before[0].read_bytes()); blank[0x31000] = 1
        with self.assertRaisesRegex(ValueError, 'bank tail'):
            verify.journal(blank, baseline=True)

    def test_repeated_exchange_preserves_identity_and_advances_history(self):
        for i in range(2):
            raw = bytearray(self.before[i].read_bytes())
            idle(raw, i+1, counter=3, cursor=9, generation=12)
            self.before[i].write_bytes(raw)
            raw = bytearray(self.after[i].read_bytes())
            idle(raw, i+1, counter=5, cursor=12, generation=16, receipt=i+60)
            publication(self.after[i], raw, i+1)
        self.assertTrue(verify.verify(self.before, self.after, [0, 1])['verified'])
        original = self.after[0].read_bytes()
        for offset, value in ((48, b'\x03'), (80, b'\x03'), (104, struct.pack('<Q', 4))):
            with self.subTest(offset=offset):
                raw = bytearray(original); reseal(raw, offset, value)
                publication(self.after[0], raw, 1)
                with self.assertRaisesRegex(ValueError, 'history mismatch'):
                    verify.verify(self.before, self.after, [0, 1])


if __name__ == '__main__':
    unittest.main()
