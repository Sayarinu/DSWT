#!/usr/bin/env python3
"""Read-only verification of a demo exchange without evolution or PC normalization.

Close both emulators first. Baselines must have the same inventory in both retail
copies. No ROM, save, key, credential or Pokémon bytes are included in the report.
"""

import argparse
from collections import Counter
from contextlib import ExitStack
import fcntl
import hashlib
import itertools
import json
import os
from pathlib import Path
import struct

SIZE = 524288
BLOCKS = ((0, 53036), (53036, 74212))
ORDERS = tuple(itertools.permutations(range(4)))


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha(data):
    return hashlib.sha256(data).digest()


def crc(data):
    value = 65535
    for byte in data:
        value ^= byte << 8
        for _ in range(8):
            value = ((value << 1) ^ (0x1021 if value & 0x8000 else 0)) & 65535
    return value


def canonical(raw):
    require(len(raw) == 136, 'Wrong box image size')
    pid, sanity, checksum = struct.unpack_from('<IHH', raw)
    require(sanity == 0, 'Unencrypted or invalid retail Pokémon')
    seed = checksum
    plain = bytearray()
    for offset in range(8, 136, 2):
        seed = (seed * 0x41C64E6D + 0x6073) & 0xFFFFFFFF
        plain += struct.pack('<H', struct.unpack_from('<H', raw, offset)[0] ^ (seed >> 16))
    require(sum(struct.unpack('<64H', plain)) & 65535 == checksum, 'Pokémon checksum mismatch')
    order = ORDERS[((pid >> 13) & 31) % 24]
    return raw[:8] + b''.join(plain[order.index(i)*32:order.index(i)*32+32] for i in range(4))


def inventory(raw, bank):
    require(len(raw) == SIZE, 'Expected a 512 KiB save')
    for block, (offset, size) in enumerate(BLOCKS):
        footer = raw[bank+offset+size-20:bank+offset+size]
        require(struct.unpack_from('<II', footer, 8) == (size, 0x20060623)
                and footer[16] == block
                and struct.unpack_from('<H', footer, 18)[0] == crc(raw[bank+offset:bank+offset+size-20]),
                'Retail block footer or CRC mismatch')
    capacity, count = struct.unpack_from('<II', raw, bank+152)
    require(capacity == 6 and 1 <= count <= 6, 'Invalid party shape')
    party = [canonical(raw[bank+160+i*236:bank+296+i*236]) for i in range(count)]
    boxes = [canonical(raw[bank+53040+i*136:bank+53176+i*136]) for i in range(540)]
    return party, [pk for pk in boxes if struct.unpack_from('<H', pk, 8)[0]]


def journal(raw, baseline=False):
    require(len(raw) == SIZE, 'Expected a 512 KiB save')
    for bank in (0x30000, 0x70000):
        tail = raw[bank+4096:bank+65536]
        require(tail in (bytes(len(tail)), b'\xff'*len(tail)), 'Unknown reserved journal bank tail')
    a, b = (raw[bank:bank+4096] for bank in (0x30000, 0x70000))
    if baseline and all(record in (bytes(4096), b'\xff'*4096) for record in (a, b)):
        return None
    require(a == b and a[:6] == b'PWTJ\1\0' and a[4092:] == b'CMIT'
            and struct.unpack_from('<I', a, 8)[0] == 4096 and sha(a[:4060]) == a[4060:4092],
            'Journal mirrors are not identical committed v1 records')
    require(a[1440:1444] == b'\1\0\x20\0' and any(a[1444:1476])
            and all(not any(a[start:end]) for start, end in
                    ((12, 16), (24, 32), (152, 160), (1476, 1952), (2216, 4060))),
            'Unsupported journal layout or entropy state')
    phase = struct.unpack_from('<H', a, 6)[0]
    require(not any(a[160:1440]) and not any(a[1952:2216]), 'IDLE retains transaction evidence')
    if baseline and phase == 0:
        require(not any(a[32:1440]), 'Unsupported EMPTY journal state')
        return None
    require(phase == 2 and any(a[32:48]) and any(a[48:80]) and any(a[80:96]),
            'Trade must be fully acknowledged and IDLE')
    counter, cursor = struct.unpack_from('<QQ', a, 96)
    require(counter >= (1 if baseline else 3) and (baseline or cursor >= 4)
            and bool(any(a[128:144])) == (counter > 1)
            and struct.unpack_from('<Q', a, 144)[0] == counter-1
            and 0 < struct.unpack_from('<Q', a, 16)[0] < (1 << 64)-1, 'Invalid IDLE progress')
    return {'identity': a[32:48], 'binding': a[32:96], 'cursor': cursor, 'receipt': a[112:128],
            'counter': counter, 'generation': struct.unpack_from('<Q', a, 16)[0]}


def host(path, raw):
    store = Path(str(path)+'.platinumwt')
    head, a, b = ((store/name).read_bytes() for name in ('HEAD', 'A', 'B'))
    require(len(head) == 128 and head[:8] == b'PWTHD001' and sha(head[:-32]) == head[-32:], 'Invalid host HEAD')
    require(a == b and len(a) == SIZE+96 and a[:8] == b'PWTHS001' and sha(a[:-32]) == a[-32:], 'Invalid host snapshots')
    require(a[8:64] == head[8:64] and a[64:-32] == raw and sha(raw) == head[32:64], 'Host publication mismatch')
    require(any(head[16:32]), 'Missing host lineage')
    return head[16:32], struct.unpack_from('<Q', head, 8)[0]


def traded(pk):
    result = bytearray(pk)
    result[20] = 70
    struct.pack_into('<H', result, 6, sum(struct.unpack_from('<64H', result, 8)) & 65535)
    return bytes(result)


def verify(before, after, slots):
    with ExitStack() as stack:
        for path in sorted(set(after)):
            lock = stack.enter_context(Path(str(path)+'.platinumwt.lock').open('rb'))
            fcntl.flock(lock, fcntl.LOCK_SH | fcntl.LOCK_NB)
        sources = [p.read_bytes() for p in before]
        results = [p.read_bytes() for p in after]
        initial, final, journals, lineages, reports = [], [], [], [], []
        for i in range(2):
            party, boxes = inventory(sources[i], 0)
            require(Counter(party+boxes) == Counter(sum(inventory(sources[i], 0x40000), [])), 'Ambiguous baseline inventory')
            require(0 <= slots[i] < len(party), 'Offered slot is outside baseline party')
            initial.append((party, boxes))
            old = journal(sources[i], baseline=True)
            new = journal(results[i])
            require(new['counter'] == (old['counter'] if old else 1)+2 and any(new['receipt']), 'Wrong completed mutation count')
            if old:
                require(old['binding'] == new['binding'] and old['receipt'] != new['receipt']
                        and new['cursor'] > old['cursor'] and new['generation'] > old['generation'],
                        'Identity or receipt history mismatch')
            lineage, generation = host(after[i], results[i])
            journals.append(new); lineages.append(lineage)
            current_party, current_boxes = inventory(results[i], 0)
            final.append((current_party, current_boxes))
            require(Counter(current_party+current_boxes) == Counter(sum(inventory(results[i], 0x40000), [])), 'Retail inventories disagree')
            for offset, size in BLOCKS:
                require(results[i][offset:offset+size-20] == results[i][0x40000+offset:0x40000+offset+size-20], 'Retail bodies disagree')
            reports.append({'party_species': [struct.unpack_from('<H', pk, 8)[0] for pk in current_party],
                            'pc_count': len(current_boxes), 'host_generation': generation,
                            'journal_generation': new['generation'], 'save_sha256': sha(results[i]).hex()})
        require(journals[0]['identity'] != journals[1]['identity'] and journals[0]['receipt'] != journals[1]['receipt']
                and lineages[0] != lineages[1], 'Demo clients are not independent')
        for i in range(2):
            party, boxes = initial[i]
            expected = Counter(party+boxes)
            expected[party[slots[i]]] -= 1
            expected[traded(initial[1-i][0][slots[1-i]])] += 1
            require(+expected == Counter(sum(final[i], [])), 'Inventory changed beyond the reciprocal exchange')
        require(all(path.read_bytes() == raw for path, raw in zip(before+after, sources+results)), 'Save changed during verification')
        return {'verified': True, 'scope': 'client saves; server ledger must be checked separately',
                'profiles': reports, 'baselines_sha256': [sha(raw).hex() for raw in sources]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('before-a', 'before-b', 'after-a', 'after-b'):
        parser.add_argument('--'+name, type=Path, required=True)
    for name in ('slot-a', 'slot-b'):
        parser.add_argument('--'+name, type=int, required=True)
    parser.add_argument('--report', type=Path, help='Create a new private JSON report; never overwrite a file')
    args = parser.parse_args()
    try:
        report = verify([args.before_a, args.before_b], [args.after_a, args.after_b], [args.slot_a, args.slot_b])
        output = json.dumps(report, indent=2)+'\n'
        if args.report:
            descriptor = os.open(args.report, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            with os.fdopen(descriptor, 'w') as stream:
                stream.write(output)
        print(output, end='')
    except (OSError, ValueError, struct.error) as error:
        parser.exit(1, 'Verification failed: '+str(error)+'\n')


if __name__ == '__main__':
    main()
