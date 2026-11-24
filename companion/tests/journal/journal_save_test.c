#include "save/journal_save.h"
#include "save/journal_host_io.h"
#include "save/posix_file.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned char bytes[0x80000], before[0x80000];
static const uint32_t banks[2] = { 0x30000, 0x70000 };
static int writes, syncs, reads, failWrite, failSync, failRead, lie;
static uint64_t length = sizeof(bytes);
static unsigned char seed[32];
static struct { uint32_t offset; size_t size; unsigned char data[4092]; } events[12];
static unsigned eventCount;
static int capture;

static int Read(void *arg, uint32_t offset, void *data, size_t size)
{
    (void)arg;
    if (++reads == failRead || offset + size > sizeof(bytes)) return -1;
    memcpy(data, bytes + offset, size);
    return 0;
}
static int Write(void *arg, uint32_t offset, const void *data, size_t size)
{
    (void)arg;
    ++writes;
    /* Every bank publication must invalidate, write body, then seal. */
    assert(syncs == writes - 1);
    assert(offset == banks[0] + ((writes - 1) % 3 == 1 ? 0 : 4092)
        || offset == banks[1] + ((writes - 1) % 3 == 1 ? 0 : 4092));
    assert(size == ((writes - 1) % 3 == 1 ? 4092 : 4));
    if (writes == failWrite) return -1;
    if (capture) {
        assert(eventCount < 12);
        events[eventCount].offset = offset;
        events[eventCount].size = size;
        memcpy(events[eventCount++].data, data, size);
    }
    if (!lie) memcpy(bytes + offset, data, size);
    return 0;
}
static int Sync(void *arg) { (void)arg; return ++syncs == failSync ? -1 : 0; }
static int Size(void *arg, uint64_t *size) { (void)arg; *size = length; return 0; }
static CpFile file = { NULL, Read, Write, Sync, Size };

static void Reset(int first, int second)
{
    memset(bytes, 0x57, sizeof(bytes));
    memset(bytes + banks[0], first, 0x10000);
    memset(bytes + banks[1], second, 0x10000);
    writes = syncs = reads = failWrite = failSync = failRead = lie = 0;
    length = sizeof(bytes);
}

static void Detection(void)
{
    WtJournal journal;
    WtJournalEntry entry;
    unsigned char record[4096];
    int a, b, bank, cut;
    CpJournalHostStore host;
    WtJournalIO io = CpJournalHostIO(&host, &file);
    for (a = 0; a <= 255; a += 255) for (b = 0; b <= 255; b += 255) {
        Reset(a, b);
        assert(CpSaveJournal_Detect(&file) == CP_SAVE_JOURNAL_UNUSED);
        assert(writes == 0);
        assert(CpSaveJournal_Provision(&file, &journal, io, seed) == 0);
        assert(writes == 6 && syncs == 6);
        assert(WtJournal_Open(&journal, io) == 0);
        assert(WtJournal_Read(&journal, &entry) == 0 && entry.phase == WT_JOURNAL_EMPTY);
        assert(entry.generation == 1 && !memcmp(entry.seed, seed, 32));
        memcpy(record, bytes + banks[0], sizeof(record));
        assert(!memcmp(record, bytes + banks[1], sizeof(record)));
        assert(CpSaveJournal_Detect(&file) == CP_SAVE_JOURNAL_HAS_JOURNAL);
        memcpy(before, bytes, sizeof(bytes));
        assert(CpSaveJournal_Provision(&file, &journal, io, seed) != 0 && !journal.usable);
        assert(writes == 6 && !memcmp(before, bytes, sizeof(bytes)));
    }
    for (bank = 0; bank < 2; ++bank) {
        const unsigned offsets[] = { 4096, 65535 };
        unsigned i;
        for (i = 0; i < sizeof(offsets) / sizeof(*offsets); ++i) {
            Reset(0, 0); bytes[banks[bank] + offsets[i]] = 255;
            assert(CpSaveJournal_Detect(&file) == CP_SAVE_JOURNAL_REFUSE);
            assert(CpSaveJournal_Provision(&file, &journal, io, seed) != 0 && writes == 0);
        }
        Reset(0, 0); bytes[banks[bank] + 100] = 3;
        assert(CpSaveJournal_Detect(&file) == CP_SAVE_JOURNAL_REFUSE);
        assert(CpSaveJournal_Provision(&file, &journal, io, seed) != 0 && writes == 0);
        Reset(0, 0); memcpy(bytes + banks[bank], record, sizeof(record));
        bytes[banks[bank] + 60000] = 42;
        assert(CpSaveJournal_Detect(&file) == CP_SAVE_JOURNAL_REFUSE);
        Reset(0, 0); memcpy(bytes + banks[bank], record, sizeof(record));
        bytes[banks[bank] + 6] = 255; /* Sealed but invalid phase. */
        assert(CpSaveJournal_Hash(bytes + banks[bank], 4060, bytes + banks[bank] + 4060) == 0);
        assert(CpSaveJournal_Detect(&file) == CP_SAVE_JOURNAL_REFUSE);
    }
    Reset(0, 0);
    memset(&entry, 0, sizeof(entry));
    entry.phase = WT_JOURNAL_IDLE; entry.generation = 3; entry.nextCounter = 1;
    memset(entry.seed, 7, 32); memset(entry.saveID, 1, 16);
    memset(entry.credential, 2, 32); memset(entry.enrollmentID, 3, 16);
    assert(WtJournal_Encode(&entry, CpSaveJournal_Hash, record) == 0);
    memcpy(bytes + banks[0], record, sizeof(record));
    memcpy(bytes + banks[1], record, sizeof(record));
    memset(bytes + banks[1] + 4092, 0, 4);
    assert(WtJournal_Open(&journal, io) == 0);
    assert(CpSaveJournal_Detect(&file) == CP_SAVE_JOURNAL_HAS_JOURNAL);
    assert(CpSaveJournal_Provision(&file, &journal, io, seed) != 0 && writes == 0);
    entry.saveID[0] = 99;
    assert(WtJournal_Encode(&entry, CpSaveJournal_Hash, bytes + banks[1]) == 0);
    assert(CpSaveJournal_Detect(&file) == CP_SAVE_JOURNAL_REFUSE); /* Conflicting sealed mirrors. */
    Reset(0, 0); length--;
    assert(CpSaveJournal_Detect(&file) == CP_SAVE_JOURNAL_REFUSE);
    for (cut = 1; cut <= 242; ++cut) {
        Reset(0, 0); failRead = cut;
        assert(CpSaveJournal_Detect(&file) == CP_SAVE_JOURNAL_REFUSE);
    }
    assert(CpSaveJournal_Detect(NULL) == CP_SAVE_JOURNAL_REFUSE);
    puts("PASS journal detection: independent blank values, mixed values, committed/torn records, foreign data, tails and read/size failures");
}

/* Capture the REAL backend's tag-zero/body/tag publications, including the
 * core's erase pass on retry, then cut each individual write at every byte. */
static void PowerCuts(void)
{
    static unsigned char initial[0x80000];
    WtJournal journal;
    WtJournalEntry entry;
    unsigned char record[4096];
    CpJournalHostStore host;
    WtJournalIO io = CpJournalHostIO(&host, &file);
    int blank, retry;
    unsigned event, previous, checked = 0;
    size_t cut;
    memset(&entry, 0, sizeof(entry)); entry.generation = 1;
    memcpy(entry.seed, seed, sizeof(seed));
    assert(WtJournal_Encode(&entry, CpSaveJournal_Hash, record) == 0);
    for (blank = 0; blank <= 255; blank += 255) for (retry = 0; retry < 3; ++retry) {
        Reset(blank, blank);
        if (retry) {
            memcpy(bytes + banks[0], record, 1445);
            memset(bytes + banks[0] + 4092, 0, 4);
        }
        if (retry == 2) {
            /* Both a zeroed record + FF tail and an FF record + zero tail. */
            memset(bytes + banks[0], 0, 4096);
            memset(bytes + banks[1], 255, 4096);
        }
        memcpy(initial, bytes, sizeof(bytes));
        capture = 1; eventCount = 0;
        assert(CpSaveJournal_Provision(&file, &journal, io, seed) == 0);
        capture = 0;
        for (event = 0; event < eventCount; ++event) {
            memcpy(bytes, initial, sizeof(bytes));
            for (previous = 0; previous < event; ++previous)
                memcpy(bytes + events[previous].offset, events[previous].data, events[previous].size);
            for (cut = 0; cut <= events[event].size; ++cut) {
                CpSaveJournalDetection expected;
                if (cut) bytes[events[event].offset + cut - 1] = events[event].data[cut - 1];
                if (WtJournal_Open(&journal, io) == 0) {
                    WtJournal_Close(&journal);
                    expected = CP_SAVE_JOURNAL_HAS_JOURNAL;
                } else {
                    assert(WtJournal_Unused(bytes + banks[0], bytes + banks[1], CpSaveJournal_Hash));
                    expected = CP_SAVE_JOURNAL_UNUSED;
                }
                assert(CpSaveJournal_Detect(&file) == expected);
                /* Check actual re-initialization at each boundary and at the
                 * originally reported tag-only interruption. */
                if (expected == CP_SAVE_JOURNAL_UNUSED && (cut == 0 || cut == events[event].size)) {
                    memcpy(before, bytes, sizeof(bytes));
                    writes = syncs = 0;
                    assert(CpSaveJournal_Provision(&file, &journal, io, seed) == 0);
                    memcpy(bytes, before, sizeof(bytes));
                }
                ++checked;
            }
        }
    }
    printf("PASS %u byte cut points in real tag-zero/body/tag and retry erase order (00/FF blanks)\n", checked);
}

static void Failures(void)
{
    int failure;
    WtJournal journal;
    CpJournalHostStore host;
    WtJournalIO io = CpJournalHostIO(&host, &file);
    for (failure = 1; failure <= 6; ++failure) {
        Reset(255, 255); failWrite = failure;
        assert(CpSaveJournal_Provision(&file, &journal, io, seed) != 0 && !journal.usable);
        Reset(255, 255); failSync = failure;
        assert(CpSaveJournal_Provision(&file, &journal, io, seed) != 0 && !journal.usable);
    }
    Reset(255, 255); lie = 1;
    assert(CpSaveJournal_Provision(&file, &journal, io, seed) != 0 && !journal.usable);
    Reset(0, 0); memset(seed, 0, sizeof(seed));
    assert(CpSaveJournal_Provision(&file, &journal, io, seed) != 0 && writes == 0);
    memset(seed, 7, sizeof(seed));
    assert(CpSaveJournal_Provision(&file, &journal, io, NULL) != 0 && writes == 0);
    {
        CpFile noSync = file;
        noSync.sync = NULL;
        Reset(255, 255);
        io = CpJournalHostIO(&host, &noSync);
        assert(host.platform.sync == NULL);
        assert(CpSaveJournal_Provision(&noSync, &journal, io, seed) != 0 && writes == 0);
    }
    puts("PASS provisioning: tag-last/sync order, write/sync failures, false success read-back and entropy refusal");
}

int main(int argc, char **argv)
{
    memset(seed, 7, sizeof(seed));
    if (argc == 3 && !strcmp(argv[1], "--provision")) {
        CpFile disk;
        CpPosixFile context;
        CpJournalHostStore host;
        WtJournal journal;
        int fd = open(argv[2], O_RDWR);
        assert(fd >= 0 && CpPosixFile_Init(&disk, &context, fd) == 0);
        assert(CpSaveJournal_Provision(&disk, &journal, CpJournalHostIO(&host, &disk), seed) == 0);
        assert(close(fd) == 0);
        return 0;
    }
    Reset(255, 255);
    memset(bytes + banks[0] + 4092, 0, 4);
    assert(WtJournal_Unused(bytes + banks[0], bytes + banks[1], CpSaveJournal_Hash));
    assert(CpSaveJournal_Detect(&file) == CP_SAVE_JOURNAL_UNUSED);
    Detection();
    PowerCuts();
    Failures();
    return 0;
}
