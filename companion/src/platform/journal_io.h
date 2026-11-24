#ifndef COMPANION_PLATFORM_JOURNAL_IO_H
#define COMPANION_PLATFORM_JOURNAL_IO_H

#include "platform/file.h"
#include "wonder_trade/journal.h"

/* The journal banks in a Generation IV save (journal_banks and
 * bank_reserved_size in protocol/save-layout.json, -dp and -hgss): each bank
 * is a 4 KiB record followed by blank bytes up to 64 KiB. */
#define CP_JOURNAL_BANK_0 0x30000u
#define CP_JOURNAL_BANK_1 0x70000u
#define CP_JOURNAL_RESERVED_SIZE 0x10000u

enum CpJournalError {
    CP_JOURNAL_ERROR_SIZE = -50,     /* the file is not a 512 KiB save */
    CP_JOURNAL_ERROR_READ = -51,
    CP_JOURNAL_ERROR_FOREIGN = -52,  /* the reserved bytes are not blank */
    CP_JOURNAL_ERROR_WRITE = -53,
    CP_JOURNAL_ERROR_SYNC = -54,
    CP_JOURNAL_ERROR_VERIFY = -55,   /* the card does not hold what was written */
    CP_JOURNAL_ERROR_READ_ONLY = -56
};

/* WtJournalIO's argument. hold, when set, keeps the console awake (nonzero)
 * while a bank is written. error keeps the first failure. */
typedef struct CpJournalStore {
    const CpPlatformFile *file;
    void (*hold)(int);
    int error;
} CpJournalStore;

/* The WtJournalIO read and write callbacks over a save file. A write zeroes
 * the bank's 4-byte commit tag, syncs, writes the body, syncs, writes the
 * tag, syncs, then reads the whole record back from the card, bypassing
 * every cache. The file must be exactly 512 KiB before and after. */
int CpJournal_ReadBank(void *store, int bank, unsigned char record[WT_JOURNAL_RECORD_SIZE]);
int CpJournal_WriteBank(void *store, int bank, const unsigned char record[WT_JOURNAL_RECORD_SIZE]);
WtJournalIO CpJournal_IO(CpJournalStore *, WtJournalHash);

#endif
