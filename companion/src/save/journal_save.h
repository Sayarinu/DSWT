#ifndef CP_JOURNAL_SAVE_H
#define CP_JOURNAL_SAVE_H

#include "file.h"
#include "wonder_trade/journal.h"

typedef enum {
    CP_JOURNAL_REFUSE,
    CP_JOURNAL_UNMODIFIED_NEVER_USED,
    CP_JOURNAL_HAS_JOURNAL
} CpJournalDetection;

/* Run on a worker with at least 32 KiB of stack. Requires exclusive access to
 * an exactly 512 KiB save for the entire operation.
 * REFUSE also covers I/O errors. Detection never writes or repairs anything. */
CpJournalDetection CpJournal_Detect(const CpFile *file);
/* io must address this same file and provide durable tag-last publication.
 * On DSi use the platform journal backend; host tests use journal_host_io.h.
 * seed is 32 fresh bytes from the caller's entropy source, never a timestamp.
 * Refuses every non-blank reserved area, including a torn first provisioning. */
int CpJournal_Provision(const CpFile *file, WtJournal *journal, WtJournalIO io,
                        const unsigned char seed[32]);
int CpJournal_Hash(const unsigned char *data, size_t size, unsigned char digest[32]);

#endif
