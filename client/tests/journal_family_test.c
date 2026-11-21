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
    return 0;
}
