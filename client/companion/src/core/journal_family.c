#include "core/journal_family.h"

#include "wonder_trade/journal.h"

/* The two builds of journal.c (journal_rename.h). */
#define CP_JOURNAL_DECLARE(prefix) \
    int prefix##_Encode(const WtJournalEntry *, WtJournalHash, unsigned char *); \
    int prefix##_Decode(const unsigned char *, WtJournalHash, WtJournalEntry *); \
    int prefix##_Open(WtJournal *, WtJournalIO); \
    int prefix##_OpenEmpty(WtJournal *, WtJournalIO); \
    int prefix##_Unused(const unsigned char *, const unsigned char *, WtJournalHash); \
    int prefix##_Initialize(WtJournal *, WtJournalIO, const unsigned char *); \
    int prefix##_Read(const WtJournal *, WtJournalEntry *); \
    int prefix##_Repair(WtJournal *); \
    int prefix##_Commit(WtJournal *, const WtJournalEntry *); \
    void prefix##_Close(WtJournal *);

CP_JOURNAL_DECLARE(CpJournalGen4)
CP_JOURNAL_DECLARE(CpJournalGen5)

static CpCoreFamily sFamily = CP_CORE_GEN4;

void CpCore_SelectFamily(CpCoreFamily family)
{
    sFamily = family == CP_CORE_GEN5 ? CP_CORE_GEN5 : CP_CORE_GEN4;
}

CpCoreFamily CpCore_Family(void)
{
    return sFamily;
}

#define CP_JOURNAL_CALL(name, args) \
    (sFamily == CP_CORE_GEN5 ? CpJournalGen5_##name args : CpJournalGen4_##name args)

int WtJournal_Encode(const WtJournalEntry *entry, WtJournalHash hash, unsigned char *record)
{
    return CP_JOURNAL_CALL(Encode, (entry, hash, record));
}

int WtJournal_Decode(const unsigned char *record, WtJournalHash hash, WtJournalEntry *entry)
{
    return CP_JOURNAL_CALL(Decode, (record, hash, entry));
}

int WtJournal_Open(WtJournal *store, WtJournalIO io)
{
    return CP_JOURNAL_CALL(Open, (store, io));
}

int WtJournal_OpenEmpty(WtJournal *store, WtJournalIO io)
{
    return CP_JOURNAL_CALL(OpenEmpty, (store, io));
}

int WtJournal_Unused(const unsigned char *first, const unsigned char *second, WtJournalHash hash)
{
    return CP_JOURNAL_CALL(Unused, (first, second, hash));
}

int WtJournal_Initialize(WtJournal *store, WtJournalIO io, const unsigned char *seed)
{
    return CP_JOURNAL_CALL(Initialize, (store, io, seed));
}

int WtJournal_Read(const WtJournal *store, WtJournalEntry *entry)
{
    return CP_JOURNAL_CALL(Read, (store, entry));
}

int WtJournal_Repair(WtJournal *store)
{
    return CP_JOURNAL_CALL(Repair, (store));
}

int WtJournal_Commit(WtJournal *store, const WtJournalEntry *entry)
{
    return CP_JOURNAL_CALL(Commit, (store, entry));
}

void WtJournal_Close(WtJournal *store)
{
    if (sFamily == CP_CORE_GEN5) CpJournalGen5_Close(store);
    else CpJournalGen4_Close(store);
}
