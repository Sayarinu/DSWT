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
    assert(offset == banks[(writes - 1) / 3] + ((writes - 1) % 3 == 1 ? 0 : 4092));
    assert(size == ((writes - 1) % 3 == 1 ? 4092 : 4));
    if (writes == failWrite) return -1;
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
    const int cuts[] = { 1, 4, 15, 16, 24, 1440, 1444, 1476, 4060, 4092, 4093, 4095 };
    CpJournalHostStore host;
    WtJournalIO io = CpJournalHostIO(&host, &file);
    for (a = 0; a <= 255; a += 255) for (b = 0; b <= 255; b += 255) {
        Reset(a, b);
        assert(CpJournal_Detect(&file) == CP_JOURNAL_UNMODIFIED_NEVER_USED);
        assert(writes == 0);
        assert(CpJournal_Provision(&file, &journal, io, seed) == 0);
        assert(writes == 6 && syncs == 6);
        assert(WtJournal_Open(&journal, io) == 0);
        assert(WtJournal_Read(&journal, &entry) == 0 && entry.phase == WT_JOURNAL_EMPTY);
        assert(entry.generation == 1 && !memcmp(entry.seed, seed, 32));
        memcpy(record, bytes + banks[0], sizeof(record));
        assert(!memcmp(record, bytes + banks[1], sizeof(record)));
        assert(CpJournal_Detect(&file) == CP_JOURNAL_HAS_JOURNAL);
        memcpy(before, bytes, sizeof(bytes));
        assert(CpJournal_Provision(&file, &journal, io, seed) != 0 && !journal.usable);
        assert(writes == 6 && !memcmp(before, bytes, sizeof(bytes)));
        for (bank = 0; bank < 2; ++bank) {
            for (cut = 0; cut < (int)(sizeof(cuts) / sizeof(*cuts)); ++cut) {
                Reset(a, b);
                memcpy(bytes + banks[bank], record, (size_t)cuts[cut]);
                assert(WtJournal_Unused(bytes + banks[0], bytes + banks[1], CpJournal_Hash));
                assert(CpJournal_Detect(&file) == CP_JOURNAL_HAS_JOURNAL);
                assert(CpJournal_Provision(&file, &journal, io, seed) != 0 && writes == 0);
                memcpy(bytes + banks[bank ^ 1], record, sizeof(record));
                assert(WtJournal_Open(&journal, io) == 0);
                assert(CpJournal_Detect(&file) == CP_JOURNAL_HAS_JOURNAL);
            }
        }
    }
    for (bank = 0; bank < 2; ++bank) {
        const unsigned offsets[] = { 0, 1, 4095, 4096, 65535 };
        unsigned i;
        for (i = 0; i < sizeof(offsets) / sizeof(*offsets); ++i) {
            Reset(0, 0); bytes[banks[bank] + offsets[i]] = 255;
            assert(CpJournal_Detect(&file) == CP_JOURNAL_REFUSE);
            assert(CpJournal_Provision(&file, &journal, io, seed) != 0 && writes == 0);
        }
        Reset(0, 0); bytes[banks[bank] + 100] = 3;
        assert(CpJournal_Detect(&file) == CP_JOURNAL_REFUSE);
        assert(CpJournal_Provision(&file, &journal, io, seed) != 0 && writes == 0);
        Reset(0, 0); memcpy(bytes + banks[bank], record, sizeof(record));
        bytes[banks[bank] + 60000] = 42;
        assert(CpJournal_Detect(&file) == CP_JOURNAL_REFUSE);
        Reset(0, 0); memcpy(bytes + banks[bank], record, sizeof(record));
        bytes[banks[bank] + 6] = 255; /* Sealed but invalid phase. */
        assert(CpJournal_Hash(bytes + banks[bank], 4060, bytes + banks[bank] + 4060) == 0);
        assert(CpJournal_Detect(&file) == CP_JOURNAL_REFUSE);
    }
    Reset(0, 0);
    memset(&entry, 0, sizeof(entry));
    entry.phase = WT_JOURNAL_IDLE; entry.generation = 3; entry.nextCounter = 1;
    memset(entry.seed, 7, 32); memset(entry.saveID, 1, 16);
    memset(entry.credential, 2, 32); memset(entry.enrollmentID, 3, 16);
    assert(WtJournal_Encode(&entry, CpJournal_Hash, record) == 0);
    memcpy(bytes + banks[0], record, sizeof(record));
    memcpy(bytes + banks[1], record, sizeof(record));
    memset(bytes + banks[1] + 4092, 0, 4);
    assert(WtJournal_Open(&journal, io) == 0);
    assert(CpJournal_Detect(&file) == CP_JOURNAL_HAS_JOURNAL);
    assert(CpJournal_Provision(&file, &journal, io, seed) != 0 && writes == 0);
    entry.saveID[0] = 99;
    assert(WtJournal_Encode(&entry, CpJournal_Hash, bytes + banks[1]) == 0);
    assert(CpJournal_Detect(&file) == CP_JOURNAL_REFUSE); /* Conflicting sealed mirrors. */
    Reset(0, 0); length--;
    assert(CpJournal_Detect(&file) == CP_JOURNAL_REFUSE);
    for (cut = 1; cut <= 256; ++cut) {
        Reset(0, 0); failRead = cut;
        assert(CpJournal_Detect(&file) == CP_JOURNAL_REFUSE);
    }
    assert(CpJournal_Detect(NULL) == CP_JOURNAL_REFUSE);
    puts("PASS journal detection: independent blank values, mixed values, committed/torn records, foreign data, tails and read/size failures");
}

static void Failures(void)
{
    int failure;
    WtJournal journal;
    CpJournalHostStore host;
    WtJournalIO io = CpJournalHostIO(&host, &file);
    for (failure = 1; failure <= 6; ++failure) {
        Reset(255, 255); failWrite = failure;
        assert(CpJournal_Provision(&file, &journal, io, seed) != 0 && !journal.usable);
        Reset(255, 255); failSync = failure;
        assert(CpJournal_Provision(&file, &journal, io, seed) != 0 && !journal.usable);
    }
    Reset(255, 255); lie = 1;
    assert(CpJournal_Provision(&file, &journal, io, seed) != 0 && !journal.usable);
    Reset(0, 0); memset(seed, 0, sizeof(seed));
    assert(CpJournal_Provision(&file, &journal, io, seed) != 0 && writes == 0);
    memset(seed, 7, sizeof(seed));
    assert(CpJournal_Provision(&file, &journal, io, NULL) != 0 && writes == 0);
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
        assert(CpJournal_Provision(&disk, &journal, CpJournalHostIO(&host, &disk), seed) == 0);
        assert(close(fd) == 0);
        return 0;
    }
    Detection();
    Failures();
    return 0;
}
