#include "wonder_trade/recovery.h"

#include <string.h>
#include <mbedtls/platform_util.h>

enum { UNCHECKED, CHECKING, SAFE, BLOCKED };
static volatile int sState;
static volatile int sInternalWrite;

typedef struct BootRead {
    WtJournalIO source;
    unsigned char banks[2][4096];
    unsigned seen;
} BootRead;

static int Read(void *arg, int bank, unsigned char record[4096])
{
    BootRead *read = arg;
    if (bank < 0 || bank > 1 || read->source.read(read->source.arg, bank, record) != 0) return -1;
    memcpy(read->banks[bank], record, 4096);
    read->seen |= 1u << bank;
    return 0;
}

static int NoWrite(void *arg, int bank, const unsigned char record[4096])
{
    (void)arg; (void)bank; (void)record;
    return -1;
}

static int Blank(const unsigned char record[4096])
{
    size_t i;
    if (record[0] != 0 && record[0] != 255) return 0;
    for (i = 1; i < 4096; i++) if (record[i] != record[0]) return 0;
    return 1;
}

int WtRecovery_Started(void) { return sState != UNCHECKED; }
int WtRecovery_CanWrite(void) { return sState == SAFE || sInternalWrite; }
int WtRecovery_CanClear(void) { return sState == UNCHECKED || sState == SAFE; }
void WtRecovery_Begin(void) { sState = CHECKING; }
void WtRecovery_Block(void) { sState = BLOCKED; }
void WtRecovery_InternalWrite(int allowed) { sInternalWrite = allowed != 0; }
void WtRecovery_Allow(void) { sState = SAFE; }

int WtRecovery_Check(WtJournalIO source)
{
    BootRead read;
    WtJournal store;
    WtJournalEntry entry;
    WtJournalIO io = { &read, Read, NoWrite, source.hash };
    int result = WT_JOURNAL_ERROR_UNKNOWN;
    WtRecovery_Begin();
    memset(&read, 0, sizeof(read)); read.source = source;
    if (!source.read || !source.hash) goto done;
    result = WtJournal_OpenEmpty(&store, io);
    if (result == 0) {
        if (WtJournal_Read(&store, &entry) != 0 || entry.generation == UINT64_MAX) result = WT_JOURNAL_ERROR_UNKNOWN;
        WtJournal_Close(&store);
    } else if (result == WT_JOURNAL_ERROR_UNKNOWN && read.seen == 3 && Blank(read.banks[0]) && Blank(read.banks[1])) {
        result = 0;
    }
done:
    mbedtls_platform_zeroize(&entry, sizeof(entry));
    mbedtls_platform_zeroize(&read, sizeof(read));
    sState = result == 0 ? SAFE : BLOCKED;
    return result;
}
