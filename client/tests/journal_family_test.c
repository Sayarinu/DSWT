/* Built once per game family: the journal accepts only the build's own party
 * image size and box count (wonder_trade/game_family.h). Synthetic data only. */
#include "wonder_trade/game_family.h"
#include "wonder_trade/journal.h"

#include <assert.h>
#include <string.h>
#include <psa/crypto.h>

#include "journal_fixture.h"

static int Hash(const unsigned char *bytes, size_t length, unsigned char digest[32])
{
    size_t size = 0;
    return psa_hash_compute(PSA_ALG_SHA_256, bytes, length, digest, 32, &size) == PSA_SUCCESS && size == 32 ? 0 : -1;
}

/* The fixture's party source, with an image of length bytes, then zero. */
static void Source(WtJournalEntry *entry, unsigned length)
{
    memset(entry->original, 0, sizeof(entry->original)); memset(entry->original, 6, length);
    entry->source.length = (unsigned char)length;
    assert(Hash(entry->original, length, entry->originalDigest) == 0);
}

/* An escrow from a party slot. */
static int Party(unsigned length, unsigned char record[WT_JOURNAL_RECORD_SIZE])
{
    WtJournalEntry entry;
    assert(WtTest_JournalEntry(WT_JOURNAL_ESCROW, Hash, &entry) == 0);
    Source(&entry, length);
    return WtJournal_Encode(&entry, Hash, record);
}

/* An applied delivery into a box. */
static int Box(unsigned box, unsigned slot, unsigned char record[WT_JOURNAL_RECORD_SIZE])
{
    WtJournalEntry entry;
    assert(WtTest_JournalEntry(WT_JOURNAL_APPLYING, Hash, &entry) == 0);
    Source(&entry, WT_GAME_PARTY_SIZE);
    entry.destination.box = (unsigned char)box; entry.destination.slot = (unsigned char)slot;
    return WtJournal_Encode(&entry, Hash, record);
}

static unsigned char banks[2][WT_JOURNAL_RECORD_SIZE];
static int writes, cutWrite, cutAt, tagFirst;

static int ReadBank(void *arg, int bank, unsigned char record[WT_JOURNAL_RECORD_SIZE])
{
    (void)arg;
    memcpy(record, banks[bank], WT_JOURNAL_RECORD_SIZE);
    return 0;
}

/* The flash backend's order (wonder_trade/flash.c): the tag cleared, the body,
 * then the tag. Power lost at cutAt keeps only the steps before it. Without
 * tagFirst, the bytes go in order, as the crafted torn saves of LUNE-840 hold. */
static int WriteBank(void *arg, int bank, const unsigned char record[WT_JOURNAL_RECORD_SIZE])
{
    int step, count = ++writes == cutWrite ? cutAt : WT_JOURNAL_RECORD_SIZE + 4;
    (void)arg;
    for (step = 0; step < count; step++) {
        if (!tagFirst) { if (step < WT_JOURNAL_RECORD_SIZE) banks[bank][step] = record[step]; }
        else if (step < 4) banks[bank][4092 + step] = 0;
        else banks[bank][step - 4] = record[step - 4];
    }
    return writes == cutWrite ? -1 : 0;
}

/* Power lost at every byte of the first journal write, over either blank. A
 * cut in the first bank leaves no record: the banks are unused, and the next
 * initialization erases them, even when that is cut too. A cut in the second
 * keeps the first bank's record, which repairs the second. */
static void FirstWriteTears(void)
{
    WtJournalIO io = { NULL, ReadBank, WriteBank, Hash };
    WtJournal store;
    WtJournalEntry entry;
    unsigned char first[32], second[32], expected[WT_JOURNAL_RECORD_SIZE];
    int fill, bank, cut, erase, order;
    memset(first, 0x31, 32); memset(second, 0x52, 32);
    memset(&entry, 0, sizeof(entry)); entry.generation = 1; memcpy(entry.seed, second, 32);
    assert(WtJournal_Encode(&entry, Hash, expected) == 0);
    for (order = 0; order < 2; order++) for (fill = 0; fill < 2; fill++) for (bank = 1; bank <= 2; bank++)
    for (cut = 0; cut <= WT_JOURNAL_RECORD_SIZE + 4 - order * 4; cut++) {
        memset(banks, fill ? 255 : 0, sizeof(banks));
        tagFirst = !order; writes = 0; cutWrite = bank; cutAt = cut;
        assert(WtJournal_Initialize(&store, io, first) != 0 && !store.usable);
        cutWrite = 0; writes = 0;
        /* A first bank written in full, but reported failed, is a record. */
        if (bank == 2 || cut == WT_JOURNAL_RECORD_SIZE + 4 - order * 4) {
            assert(!WtJournal_Unused(banks[0], banks[1], Hash));
            assert(WtJournal_Open(&store, io) == 0 && WtJournal_Read(&store, &entry) == 0);
            assert(entry.phase == WT_JOURNAL_EMPTY && entry.generation == 1 && memcmp(entry.seed, first, 32) == 0);
            assert(WtJournal_Repair(&store) == 0 && memcmp(banks[0], banks[1], sizeof(banks[0])) == 0);
            WtJournal_Close(&store);
            writes = 0; assert(WtJournal_Initialize(&store, io, second) != 0 && writes == 0);
            continue;
        }
        assert(WtJournal_Unused(banks[0], banks[1], Hash));
        assert(WtJournal_Open(&store, io) == WT_JOURNAL_ERROR_UNKNOWN && WtJournal_OpenEmpty(&store, io) != 0);
        assert(WtJournal_Initialize(&store, io, NULL) == WT_JOURNAL_NEEDS_SEED && writes == 0);
        /* Cut the erasure (and any write after it) at a spread of bytes, for a
         * spread of first cuts: every cut leaves the banks unused. */
        for (erase = 0; cut % 512 == 3 && erase < WT_JOURNAL_RECORD_SIZE; erase += 61) {
            writes = 0; cutWrite = 1; cutAt = erase;
            assert(WtJournal_Initialize(&store, io, second) != 0 && !store.usable);
            assert(WtJournal_Unused(banks[0], banks[1], Hash));
        }
        cutWrite = 0; writes = 0;
        assert(WtJournal_Initialize(&store, io, second) == 0);
        assert(store.usable && memcmp(banks[0], expected, sizeof(expected)) == 0);
        assert(memcmp(banks[1], expected, sizeof(expected)) == 0);
        WtJournal_Close(&store);
    }
    tagFirst = 1;
}

/* Once a record has been committed, unwritten-looking bytes are a torn write
 * as before: never unused, and kept beside an active record. */
static void CommittedTears(void)
{
    WtJournalIO io = { NULL, ReadBank, WriteBank, Hash };
    WtJournal store;
    WtJournalEntry entry;
    unsigned phase;
    for (phase = WT_JOURNAL_ENROLLING; phase <= WT_JOURNAL_RECOVERY_REQUIRED; phase++) {
        assert(WtTest_JournalEntry(phase, Hash, &entry) == 0);
        if (entry.source.kind) Source(&entry, WT_GAME_PARTY_SIZE);
        assert(WtJournal_Encode(&entry, Hash, banks[1]) == 0);
        memset(banks[0], 255, sizeof(banks[0])); memcpy(banks[0], "DSWJ", 4);
        assert(!WtJournal_Unused(banks[0], banks[1], Hash) && !WtJournal_Unused(banks[1], banks[0], Hash));
        writes = 0;
        assert(WtJournal_Open(&store, io) != 0 && WtJournal_Initialize(&store, io, entry.seed) != 0 && writes == 0);
    }
}

int main(void)
{
    unsigned char record[WT_JOURNAL_RECORD_SIZE];
    WtJournalEntry entry, decoded;
#if WT_GAME_FAMILY == WT_FAMILY_GEN4
    assert(WT_GAME_PARTY_SIZE == 236 && WT_GAME_BOX_COUNT == 18 && WT_GAME_SAVE_MAGIC == 0x20060623);
    assert(WT_GAME_BANK_RESERVED_SIZE == 0x10000 && WT_PERSISTENCE_ADDRESS == 0x2FF00);
#elif WT_GAME_FAMILY == WT_FAMILY_BW
    assert(WT_GAME_PARTY_SIZE == 220 && WT_GAME_BOX_COUNT == 24 && WT_GAME_SAVE_MAGIC == 0x31053527);
    assert(WT_GAME_BANK_RESERVED_SIZE == 0x1000 && WT_PERSISTENCE_ADDRESS == 0x47FC0);
#else
    assert(WT_GAME_PARTY_SIZE == 220 && WT_GAME_BOX_COUNT == 24 && WT_GAME_SAVE_MAGIC == 0x31053527);
    assert(WT_GAME_BANK_RESERVED_SIZE == 0x1000 && WT_PERSISTENCE_ADDRESS == 0x4BFC0);
#endif
    assert(Party(WT_GAME_PARTY_SIZE, record) == 0);
    assert(WtJournal_Decode(record, Hash, &decoded) == 0 && decoded.source.length == WT_GAME_PARTY_SIZE);
    assert(Party(WT_GAME_PARTY_SIZE == 236 ? 220 : 236, record) != 0);
    assert(Box(WT_GAME_BOX_COUNT - 1, WT_GAME_BOX_SLOTS - 1, record) == 0);
    assert(Box(WT_GAME_BOX_COUNT, 0, record) != 0 && Box(0, WT_GAME_BOX_SLOTS, record) != 0);
    /* A shorter image must leave the rest of its field zero. */
    assert(WtTest_JournalEntry(WT_JOURNAL_ESCROW, Hash, &entry) == 0);
    Source(&entry, WT_GAME_PARTY_SIZE);
    if (WT_GAME_PARTY_SIZE < sizeof(entry.original)) {
        entry.original[sizeof(entry.original) - 1] = 1;
        assert(Hash(entry.original, WT_GAME_PARTY_SIZE, entry.originalDigest) == 0);
        assert(WtJournal_Encode(&entry, Hash, record) != 0);
    }
    FirstWriteTears();
    CommittedTears();
    return 0;
}
