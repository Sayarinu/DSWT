#ifndef COMPANION_CORE_JOURNAL_RENAME_H
#define COMPANION_CORE_JOURNAL_RENAME_H

/* Force-included (-include) into each family's build of client journal.c,
 * with CP_JOURNAL_PREFIX set to CpJournalGen4 or CpJournalGen5, and into
 * journal_family.c to declare both builds. Renames every external symbol of
 * journal.c; a symbol missed here fails the companion's link (two
 * definitions). */
#define CP_JOURNAL_CAT2(a, b) a##b
#define CP_JOURNAL_CAT(a, b) CP_JOURNAL_CAT2(a, b)
#define CP_JOURNAL_NAME(name) CP_JOURNAL_CAT(CP_JOURNAL_PREFIX, name)

#define WtJournal_Encode CP_JOURNAL_NAME(_Encode)
#define WtJournal_Decode CP_JOURNAL_NAME(_Decode)
#define WtJournal_Open CP_JOURNAL_NAME(_Open)
#define WtJournal_OpenEmpty CP_JOURNAL_NAME(_OpenEmpty)
#define WtJournal_Unused CP_JOURNAL_NAME(_Unused)
#define WtJournal_Initialize CP_JOURNAL_NAME(_Initialize)
#define WtJournal_Read CP_JOURNAL_NAME(_Read)
#define WtJournal_Repair CP_JOURNAL_NAME(_Repair)
#define WtJournal_Commit CP_JOURNAL_NAME(_Commit)
#define WtJournal_Close CP_JOURNAL_NAME(_Close)

#endif
