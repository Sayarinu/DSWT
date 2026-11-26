/* LUNE-881: one companion build holds the Generation IV and Generation V
 * journals (src/core/journal_family.h). Each family accepts its own locators
 * and refuses the other's, and the selection switches between them. */
#include "core/journal_family.h"
#include "wonder_trade/journal.h"

#include <assert.h>
#include <string.h>

static int Hash(const unsigned char *bytes, size_t size, unsigned char digest[32])
{
    size_t i;
    memset(digest, 0, 32);
    for (i = 0; i < size; ++i) digest[i % 32] = (unsigned char)(digest[i % 32] * 31 + bytes[i] + 1);
    return 0;
}

static unsigned char sRecord[WT_JOURNAL_RECORD_SIZE];

/* A held-for-recovery entry with a party source of `length` bytes and a PC
 * destination in box `box`. */
static int Encodes(unsigned length, unsigned box)
{
    WtJournalEntry entry;
    WtJournalEntry decoded;
    memset(&entry, 0, sizeof(entry));
    entry.phase = WT_JOURNAL_RECOVERY_REQUIRED;
    memset(entry.seed, 1, sizeof(entry.seed));
    memset(entry.credential, 2, sizeof(entry.credential));
    memset(entry.enrollmentID, 3, sizeof(entry.enrollmentID));
    memset(entry.saveID, 4, sizeof(entry.saveID));
    entry.nextCounter = 1;
    entry.source.kind = 1; entry.source.length = (unsigned char)length;
    memset(entry.original, 5, length);
    entry.destination.kind = 2; entry.destination.box = (unsigned char)box; entry.destination.length = 136;
    if (WtJournal_Encode(&entry, Hash, sRecord) != 0) return 0;
    assert(WtJournal_Decode(sRecord, Hash, &decoded) == 0);
    assert(decoded.source.length == length && decoded.destination.box == box);
    return 1;
}

int main(void)
{
    assert(CpCore_Family() == CP_CORE_GEN4);
    assert(Encodes(236, 17) && !Encodes(220, 17) && !Encodes(236, 18));
    CpCore_SelectFamily(CP_CORE_GEN5);
    assert(CpCore_Family() == CP_CORE_GEN5);
    assert(Encodes(220, 23) && !Encodes(236, 17) && !Encodes(220, 24));
    /* A Gen V record does not decode as Gen IV. */
    assert(Encodes(220, 20));
    CpCore_SelectFamily(CP_CORE_GEN4);
    {
        WtJournalEntry entry;
        assert(WtJournal_Decode(sRecord, Hash, &entry) != 0);
    }
    CpCore_SelectFamily((CpCoreFamily)7);
    assert(CpCore_Family() == CP_CORE_GEN4);
    return 0;
}
