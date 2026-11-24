/* Journal IO over a fake card: a write cache in front of 512-byte sectors,
 * with injected failures, lying syncs, torn flushes and power cuts. */
#include "platform/journal_io.h"
#include "test_sha256.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECTOR 512
#define SECTORS (CP_SAVE_SIZE / SECTOR)

typedef struct Card {
    unsigned char cache[CP_SAVE_SIZE], disk[CP_SAVE_SIZE], dirty[SECTORS];
    uint32_t size;
    unsigned ops;          /* writes, syncs and durable reads so far */
    unsigned failAt;       /* this op reports failure (1-based; 0: never) */
    unsigned cutAt;        /* power is cut before this op runs (0: never) */
    unsigned flushLimit;   /* a sync interrupted by the cut flushes this many sectors */
    unsigned resizeAt;     /* from this op on, the file reports another size (0: never) */
    int cut, lyingSync, corruptDurable;
    char trace[512];
} Card;

static void Trace(Card *card, const char *format, uint32_t offset, size_t size)
{
    char step[48];
    snprintf(step, sizeof(step), format, (unsigned)offset, (unsigned)size);
    strncat(card->trace, step, sizeof(card->trace) - strlen(card->trace) - 1);
}

/* Count an op; nonzero when it must fail (an injected error or no power). */
static int Op(Card *card)
{
    card->ops++;
    if (card->cutAt && card->ops >= card->cutAt) card->cut = 1;
    return card->cut || card->ops == card->failAt;
}

static int ReadAt(void *arg, uint32_t offset, void *bytes, size_t size)
{
    Card *card = arg;
    if (card->cut || offset > CP_SAVE_SIZE || size > CP_SAVE_SIZE - offset) return -1;
    memcpy(bytes, card->cache + offset, size);
    return 0;
}

static int WriteAt(void *arg, uint32_t offset, const void *bytes, size_t size)
{
    Card *card = arg;
    uint32_t sector;
    int cut = card->cutAt && card->ops + 1 >= card->cutAt;
    if (Op(card)) {
        if (cut) return -1;
        Trace(card, "W%x+%u! ", offset, size);
        return -1;
    }
    Trace(card, "W%x+%u ", offset, size);
    memcpy(card->cache + offset, bytes, size);
    for (sector = offset / SECTOR; sector <= (offset + (uint32_t)size - 1) / SECTOR; sector++) card->dirty[sector] = 1;
    return 0;
}

static int Sync(void *arg)
{
    Card *card = arg;
    unsigned sector, flushed = 0;
    int cut = card->cutAt && card->ops + 1 >= card->cutAt && !card->cut;
    if (Op(card)) {
        /* Power fails during this sync: only some sectors reach the card,
         * each one whole, in order. */
        if (cut)
            for (sector = 0; sector < SECTORS && flushed < card->flushLimit; sector++)
                if (card->dirty[sector]) { memcpy(card->disk + sector * SECTOR, card->cache + sector * SECTOR, SECTOR); flushed++; }
        if (!cut && !card->cut) Trace(card, "S! ", 0, 0);
        return -1;
    }
    Trace(card, "S ", 0, 0);
    if (card->lyingSync) return 0;
    for (sector = 0; sector < SECTORS; sector++)
        if (card->dirty[sector]) { memcpy(card->disk + sector * SECTOR, card->cache + sector * SECTOR, SECTOR); card->dirty[sector] = 0; }
    return 0;
}

static int Size(void *arg, uint32_t *size)
{
    Card *card = arg;
    if (card->cut) return -1;
    *size = card->resizeAt && card->ops >= card->resizeAt ? card->size + SECTOR : card->size;
    return 0;
}

static int ReadDurable(void *arg, uint32_t offset, void *bytes, size_t size)
{
    Card *card = arg;
    if (Op(card)) return -1;
    Trace(card, "D%x+%u ", offset, size);
    memcpy(bytes, card->disk + offset, size);
    if (card->corruptDurable) ((unsigned char *)bytes)[size / 2] ^= 1;
    return 0;
}

static Card *NewCard(unsigned char blank)
{
    Card *card = calloc(1, sizeof(*card));
    assert(card);
    memset(card->disk, blank, sizeof(card->disk));
    memcpy(card->cache, card->disk, sizeof(card->cache));
    card->size = CP_SAVE_SIZE;
    return card;
}

/* Power comes back: the cache is gone, the card holds what was flushed. */
static void Reboot(Card *card)
{
    memcpy(card->cache, card->disk, sizeof(card->cache));
    memset(card->dirty, 0, sizeof(card->dirty));
    card->cut = card->lyingSync = card->corruptDurable = 0;
    card->cutAt = card->failAt = card->flushLimit = 0;
    card->trace[0] = 0;
}

static CpPlatformFile Platform(Card *card)
{
    CpPlatformFile file = { card, ReadAt, WriteAt, Sync, Size, ReadDurable };
    return file;
}

static void Record(unsigned char record[WT_JOURNAL_RECORD_SIZE], unsigned char fill)
{
    unsigned i;
    for (i = 0; i < WT_JOURNAL_RECORD_SIZE; i++) record[i] = (unsigned char)(fill + i * 7);
}

static void TestStagingOrder(void)
{
    Card *card = NewCard(0xFF);
    CpPlatformFile file = Platform(card);
    CpJournalStore store = { &file, NULL, 0 };
    unsigned char record[WT_JOURNAL_RECORD_SIZE], back[WT_JOURNAL_RECORD_SIZE];
    Record(record, 3);
    assert(CpJournal_WriteBank(&store, 1, record) == 0 && store.error == 0);
    assert(strcmp(card->trace, "W70ffc+4 S W70000+4092 S W70ffc+4 S D70000+4096 ") == 0);
    assert(memcmp(card->disk + CP_JOURNAL_BANK_1, record, sizeof(record)) == 0);
    /* Bank 0 is untouched; reads see the written bank and a blank remainder. */
    assert(card->disk[CP_JOURNAL_BANK_0] == 0xFF);
    assert(CpJournal_ReadBank(&store, 1, back) == 0 && memcmp(back, record, sizeof(back)) == 0);
    assert(CpJournal_ReadBank(&store, 0, back) == 0);
    free(card);
}

static void TestVerifiedReadBack(void)
{
    Card *card = NewCard(0);
    CpPlatformFile file = Platform(card);
    CpJournalStore store = { &file, NULL, 0 };
    unsigned char record[WT_JOURNAL_RECORD_SIZE];
    Record(record, 9);
    /* A sync that reports success without reaching the card is caught. */
    card->lyingSync = 1;
    assert(CpJournal_WriteBank(&store, 0, record) != 0 && store.error == CP_JOURNAL_ERROR_VERIFY);
    Reboot(card);
    store.error = 0;
    card->corruptDurable = 1;
    assert(CpJournal_WriteBank(&store, 0, record) != 0 && store.error == CP_JOURNAL_ERROR_VERIFY);
    free(card);
}

static void TestInjectedFailures(void)
{
    unsigned failAt;
    for (failAt = 1; failAt <= 7; failAt++) {
        Card *card = NewCard(0xFF);
        CpPlatformFile file = Platform(card);
        CpJournalStore store = { &file, NULL, 0 };
        unsigned char record[WT_JOURNAL_RECORD_SIZE];
        Record(record, (unsigned char)failAt);
        card->failAt = failAt;
        assert(CpJournal_WriteBank(&store, 0, record) != 0);
        assert(store.error == (failAt == 7 ? CP_JOURNAL_ERROR_READ : failAt % 2 ? CP_JOURNAL_ERROR_WRITE : CP_JOURNAL_ERROR_SYNC));
        /* Nothing is attempted after the failing step. */
        assert(card->ops == failAt);
        free(card);
    }
}

static int gHolds, gHeld;
static void Hold(int on) { gHolds++; gHeld = on; }

static void TestRefusals(void)
{
    Card *card = NewCard(0xFF);
    CpPlatformFile file = Platform(card);
    CpJournalStore store = { &file, Hold, 0 };
    unsigned char record[WT_JOURNAL_RECORD_SIZE];
    Record(record, 1);

    card->size = CP_SAVE_SIZE - 1;
    assert(CpJournal_ReadBank(&store, 0, record) != 0 && store.error == CP_JOURNAL_ERROR_SIZE);
    store.error = 0;
    assert(CpJournal_WriteBank(&store, 0, record) != 0 && store.error == CP_JOURNAL_ERROR_SIZE && card->ops == 0);
    card->size = CP_SAVE_SIZE;

    /* Anything but one blank value after the record is foreign data. */
    store.error = 0;
    card->cache[CP_JOURNAL_BANK_0 + 0x8000] = 0x12;
    assert(CpJournal_ReadBank(&store, 0, record) != 0 && store.error == CP_JOURNAL_ERROR_FOREIGN);
    card->cache[CP_JOURNAL_BANK_0 + 0x8000] = 0xFF;
    card->cache[CP_JOURNAL_BANK_0 + WT_JOURNAL_RECORD_SIZE] = 0;
    store.error = 0;
    assert(CpJournal_ReadBank(&store, 0, record) != 0 && store.error == CP_JOURNAL_ERROR_FOREIGN);
    card->cache[CP_JOURNAL_BANK_0 + WT_JOURNAL_RECORD_SIZE] = 0xFF;

    store.error = 0;
    assert(CpJournal_ReadBank(&store, 2, record) != 0 && CpJournal_WriteBank(&store, -1, record) != 0);

    /* A read-only file never writes. */
    file.writeAt = NULL;
    assert(CpJournal_WriteBank(&store, 0, record) != 0 && store.error == CP_JOURNAL_ERROR_READ_ONLY && card->ops == 0);
    file.writeAt = WriteAt;

    /* A file that changed size under the write is refused after it. */
    store.error = 0;
    card->resizeAt = 1;
    assert(CpJournal_WriteBank(&store, 0, record) != 0 && store.error == CP_JOURNAL_ERROR_SIZE && card->ops == 7);
    card->resizeAt = 0;
    card->ops = 0;

    /* The console is held awake for the write and released after it, even on failure. */
    gHolds = 0;
    store.error = 0;
    card->failAt = 3;
    assert(CpJournal_WriteBank(&store, 0, record) != 0 && gHolds == 2 && gHeld == 0);
    free(card);
}

/* The core journal on top: initialize, commit, reopen. */
static void TestJournalLifecycle(void)
{
    Card *card = NewCard(0xFF);
    CpPlatformFile file = Platform(card);
    CpJournalStore store = { &file, NULL, 0 };
    WtJournalIO io = CpJournal_IO(&store, TestSha256);
    WtJournal journal;
    WtJournalEntry entry;
    unsigned char seed[32];
    memset(seed, 0x5A, sizeof(seed));
    assert(WtJournal_Initialize(&journal, io, NULL) == WT_JOURNAL_NEEDS_SEED);
    assert(WtJournal_Initialize(&journal, io, seed) == 0);
    WtJournal_Close(&journal);
    Reboot(card);
    assert(WtJournal_Open(&journal, io) == 0 && WtJournal_Read(&journal, &entry) == 0);
    assert(entry.generation == 1 && memcmp(entry.seed, seed, 32) == 0);
    entry.generation = 2;
    memset(entry.seed, 0x6B, 32);
    assert(WtJournal_Commit(&journal, &entry) == 0);
    WtJournal_Close(&journal);
    Reboot(card);
    assert(WtJournal_Open(&journal, io) == 0 && WtJournal_Read(&journal, &entry) == 0 && entry.generation == 2);
    WtJournal_Close(&journal);
    assert(card->size == CP_SAVE_SIZE && store.error == 0);
    free(card);
}

/* Cut power before every step of a commit, with every partial flush: after
 * the reboot the journal opens on the old or the new generation, never a
 * torn one, and repairs back to two matching banks. */
static void TestPowerCuts(void)
{
    unsigned cutAt, flush, outcomes[3] = { 0, 0, 0 };
    for (cutAt = 1; cutAt <= 15; cutAt++) {
        for (flush = 0; flush <= 8; flush++) {
            Card *card = NewCard(0xFF);
            CpPlatformFile file = Platform(card);
            CpJournalStore store = { &file, NULL, 0 };
            WtJournalIO io = CpJournal_IO(&store, TestSha256);
            WtJournal journal;
            WtJournalEntry entry;
            unsigned char seed[32];
            int result;
            memset(seed, 0x11, sizeof(seed));
            assert(WtJournal_Initialize(&journal, io, seed) == 0);
            WtJournal_Close(&journal);
            Reboot(card);
            assert(WtJournal_Open(&journal, io) == 0 && WtJournal_Read(&journal, &entry) == 0);
            entry.generation = 2;
            memset(entry.seed, 0x22, 32);
            card->ops = 0;
            card->cutAt = cutAt;
            card->flushLimit = flush;
            result = WtJournal_Commit(&journal, &entry);
            WtJournal_Close(&journal);
            Reboot(card);
            store.error = 0;
            assert(WtJournal_Open(&journal, io) == 0 && WtJournal_Read(&journal, &entry) == 0);
            assert(entry.generation == 1 || entry.generation == 2);
            assert(entry.seed[0] == (entry.generation == 1 ? 0x11 : 0x22));
            if (result == 0) assert(entry.generation == 2);
            outcomes[entry.generation]++;
            assert(WtJournal_Repair(&journal) == 0);
            WtJournal_Close(&journal);
            Reboot(card);
            assert(memcmp(card->disk + CP_JOURNAL_BANK_0, card->disk + CP_JOURNAL_BANK_1, WT_JOURNAL_RECORD_SIZE) == 0);
            free(card);
        }
    }
    /* Both outcomes occur, so the cuts landed on both sides of the commit point. */
    assert(outcomes[1] && outcomes[2]);
    printf("power cuts: %u kept generation 1, %u reached generation 2\n", outcomes[1], outcomes[2]);
}

int main(void)
{
    TestStagingOrder();
    TestVerifiedReadBack();
    TestInjectedFailures();
    TestRefusals();
    TestJournalLifecycle();
    TestPowerCuts();
    printf("journal_io_test: ok\n");
    return 0;
}
