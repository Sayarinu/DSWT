#ifndef CP_JOURNAL_HOST_IO_H
#define CP_JOURNAL_HOST_IO_H

#include "journal_save.h"
#include "platform/journal_io.h"

typedef struct {
    const CpFile *file;
    CpPlatformFile platform;
    CpJournalStore store;
} CpJournalHostStore;

/* Host-only bridge to the platform's shared tag-last journal backend.
 * CpFile.readAt must observe durable writes after sync. Keep store and file
 * alive while using the returned IO/journal. DSi uses CpJournal_IO directly
 * with its raw readDurable callback, never this host cached-read substitute. */
WtJournalIO CpJournalHostIO(CpJournalHostStore *store, const CpFile *file);

#endif
