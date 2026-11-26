#ifndef COMPANION_PLATFORM_JOURNAL_IO_H
#define COMPANION_PLATFORM_JOURNAL_IO_H

#include "platform/file.h"
#include "wonder_trade/journal.h"

/* The journal banks in a Generation IV save (journal_banks and
 * bank_reserved_size in client/core/save-layouts/save-layout.json, -dp and -hgss): each bank
 * is a 4 KiB record followed by blank bytes up to 64 KiB. */
#define CP_JOURNAL_BANK_0 WT_GEN4_BANK_A
#define CP_JOURNAL_BANK_1 WT_GEN4_BANK_B
#define CP_JOURNAL_RESERVED_SIZE 0x10000u

/* Where a game keeps its journal banks, and the flash reserved from each
 * bank's start. Black/White and Black 2/White 2 keep two adjacent 4 KiB banks
 * in the musical distribution save (client/core/save-layouts/save-layout-bw.json, -b2w2). */
typedef struct CpJournalArea {
    uint32_t bank[2], reserved;
} CpJournalArea;

extern const CpJournalArea CpJournalArea_Gen4, CpJournalArea_BlackWhite, CpJournalArea_Black2White2;

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
 * while a bank is written. error keeps the first failure. hash, set by
 * CpJournal_IO, lets a read recognise a cut first enrollment write. area is
 * the game's journal area; NULL is Generation IV's. */
typedef struct CpJournalStore {
    const CpPlatformFile *file;
    void (*hold)(int);
    int error;
    WtJournalHash hash;
    const CpJournalArea *area;
} CpJournalStore;

/* The WtJournalIO read and write callbacks over a save file. With a hash,
 * a read shows a bank as blank when it holds only what a cut first
 * enrollment write can leave next to the other, valid bank (see
 * CpJournal_CutFirstEnrollment), so the journal opens on that valid bank.
 * A write zeroes
 * the bank's 4-byte commit tag, syncs, writes the body, syncs, writes the
 * tag, syncs, then reads the whole record back from the card, bypassing
 * every cache. The file must be exactly 512 KiB before and after. */
int CpJournal_ReadBank(void *store, int bank, unsigned char record[WT_JOURNAL_RECORD_SIZE]);
int CpJournal_WriteBank(void *store, int bank, const unsigned char record[WT_JOURNAL_RECORD_SIZE]);
WtJournalIO CpJournal_IO(CpJournalStore *, WtJournalHash);
/* Nonzero when torn, which is not a valid record, holds bytes of only two
 * records: an EMPTY record and the initial ENROLLING record that follows it,
 * one of which valid is. Only the companion can leave this: an SD card may
 * persist a multi-sector write's sectors in any order, so the header can stay
 * EMPTY while the tail already holds the enrollment packet. */
int CpJournal_CutFirstEnrollment(const unsigned char torn[WT_JOURNAL_RECORD_SIZE],
                                 const unsigned char valid[WT_JOURNAL_RECORD_SIZE], WtJournalHash);

#endif
