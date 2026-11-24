#include "platform/journal_io.h"

#include <string.h>

static const uint32_t sBank[2] = { CP_JOURNAL_BANK_0, CP_JOURNAL_BANK_1 };

static int Fail(CpJournalStore *store, int error)
{
    if (!store->error) store->error = error;
    return -1;
}

static int SizeMatches(const CpPlatformFile *file)
{
    uint32_t size = 0;
    return file->size(file->arg, &size) == 0 && size == CP_SAVE_SIZE;
}

int CpJournal_ReadBank(void *arg, int bank, unsigned char record[WT_JOURNAL_RECORD_SIZE])
{
    CpJournalStore *store = arg;
    const CpPlatformFile *file;
    unsigned char page[512];
    uint32_t offset;
    int blank = -1;
    size_t i;
    if (!store || !store->file || !record || bank < 0 || bank > 1) return -1;
    file = store->file;
    if (!SizeMatches(file)) return Fail(store, CP_JOURNAL_ERROR_SIZE);
    if (file->readAt(file->arg, sBank[bank], record, WT_JOURNAL_RECORD_SIZE) != 0)
        return Fail(store, CP_JOURNAL_ERROR_READ);
    /* Like the game's flash access: the rest of the reserved area must be
     * one blank value, or this is not a save the journal may use. */
    for (offset = WT_JOURNAL_RECORD_SIZE; offset < CP_JOURNAL_RESERVED_SIZE; offset += sizeof(page)) {
        if (file->readAt(file->arg, sBank[bank] + offset, page, sizeof(page)) != 0)
            return Fail(store, CP_JOURNAL_ERROR_READ);
        for (i = 0; i < sizeof(page); i++) {
            if (blank == -1) blank = page[i];
            if ((blank != 0 && blank != 255) || page[i] != blank) return Fail(store, CP_JOURNAL_ERROR_FOREIGN);
        }
    }
    return 0;
}

static int Stage(CpJournalStore *store, uint32_t offset, const unsigned char *bytes, size_t size)
{
    const CpPlatformFile *file = store->file;
    if (file->writeAt(file->arg, offset, bytes, size) != 0) return Fail(store, CP_JOURNAL_ERROR_WRITE);
    if (file->sync(file->arg) != 0) return Fail(store, CP_JOURNAL_ERROR_SYNC);
    return 0;
}

int CpJournal_WriteBank(void *arg, int bank, const unsigned char record[WT_JOURNAL_RECORD_SIZE])
{
    static const unsigned char uncommitted[4] = { 0, 0, 0, 0 };
    enum { TAG = WT_JOURNAL_RECORD_SIZE - 4 };
    CpJournalStore *store = arg;
    const CpPlatformFile *file;
    unsigned char check[WT_JOURNAL_RECORD_SIZE];
    uint32_t address;
    int result = -1;
    if (!store || !store->file || !record || bank < 0 || bank > 1) return -1;
    file = store->file;
    if (!file->writeAt || !file->readDurable) return Fail(store, CP_JOURNAL_ERROR_READ_ONLY);
    if (!SizeMatches(file)) return Fail(store, CP_JOURNAL_ERROR_SIZE);
    address = sBank[bank];
    if (store->hold) store->hold(1);
    if (Stage(store, address + TAG, uncommitted, sizeof(uncommitted)) != 0
        || Stage(store, address, record, TAG) != 0
        || Stage(store, address + TAG, record + TAG, 4) != 0) goto done;
    if (file->readDurable(file->arg, address, check, sizeof(check)) != 0) { Fail(store, CP_JOURNAL_ERROR_READ); goto done; }
    if (memcmp(check, record, sizeof(check)) != 0) { Fail(store, CP_JOURNAL_ERROR_VERIFY); goto done; }
    if (!SizeMatches(file)) { Fail(store, CP_JOURNAL_ERROR_SIZE); goto done; }
    result = 0;
done:
    if (store->hold) store->hold(0);
    memset(check, 0, sizeof(check));
    return result;
}

WtJournalIO CpJournal_IO(CpJournalStore *store, WtJournalHash hash)
{
    WtJournalIO io = { store, CpJournal_ReadBank, CpJournal_WriteBank, hash };
    return io;
}
