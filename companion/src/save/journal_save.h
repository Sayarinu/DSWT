#ifndef CP_JOURNAL_SAVE_H
#define CP_JOURNAL_SAVE_H

#include "file.h"
#include "wonder_trade/journal.h"

typedef enum {
    CP_SAVE_JOURNAL_REFUSE,
    CP_SAVE_JOURNAL_UNUSED,
    CP_SAVE_JOURNAL_HAS_JOURNAL
} CpSaveJournalDetection;

/* Run on a worker with at least 32 KiB of stack. Requires exclusive access to
 * an exactly 512 KiB save for the entire operation.
 * REFUSE also covers I/O errors. Detection never writes or repairs anything. */
CpSaveJournalDetection CpSaveJournal_Detect(const CpFile *file);
/* io must address this same file and provide durable tag-last publication.
 * On DSi use the platform journal backend; host tests use journal_host_io.h.
 * seed is 32 fresh bytes from the caller's entropy source, never a timestamp.
 * Accepts unused/cut first writes recognized by WtJournal_Unused, reinitializing
 * them like the game. A committed journal must be opened instead. */
int CpSaveJournal_Provision(const CpFile *file, WtJournal *journal, WtJournalIO io,
                        const unsigned char seed[32]);
int CpSaveJournal_Hash(const unsigned char *data, size_t size, unsigned char digest[32]);

#endif
