#ifndef __NDS__
#include "journal_host_io.h"

static int Read(void *context, uint32_t offset, void *data, size_t size)
{
    const CpFile *file = ((CpJournalHostStore *)context)->file;
    return file->readAt(file->context, offset, data, size);
}

static int Write(void *context, uint32_t offset, const void *data, size_t size)
{
    const CpFile *file = ((CpJournalHostStore *)context)->file;
    return file->writeAt(file->context, offset, data, size);
}

static int Sync(void *context)
{
    const CpFile *file = ((CpJournalHostStore *)context)->file;
    return file->sync(file->context);
}

static int Size(void *context, uint32_t *size)
{
    const CpFile *file = ((CpJournalHostStore *)context)->file;
    uint64_t length;
    if (file->size(file->context, &length) != 0 || length > UINT32_MAX) return -1;
    *size = (uint32_t)length;
    return 0;
}

WtJournalIO CpJournalHostIO(CpJournalHostStore *store, const CpFile *file)
{
    store->file = file;
    store->platform.arg = store;
    store->platform.readAt = Read;
    store->platform.writeAt = file->writeAt && file->sync ? Write : NULL;
    store->platform.sync = file->sync ? Sync : NULL;
    store->platform.size = Size;
    store->platform.readDurable = Read;
    store->store.file = &store->platform;
    store->store.hold = NULL;
    store->store.error = 0;
    return CpJournal_IO(&store->store, CpSaveJournal_Hash);
}
#endif
