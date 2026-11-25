/* The first enrollment commit, cut in every sector order an SD card may use.
 * The body is one 4092-byte write over eight 512-byte sectors: a card can
 * persist its tail sectors before its head, so the bank's header can stay
 * EMPTY while its tail already holds the enrollment packet. Every cut must
 * open on the record before the write or the record it wrote, and the next
 * commit must finish. Synthetic records only. */
#include "save/journal_save.h"
#include "save/journal_host_io.h"
#include "wonder_trade/wire.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define RECORD WT_JOURNAL_RECORD_SIZE
#define BODY (RECORD - 4)
#define SECTOR 512

static unsigned char bytes[0x80000];
static const uint32_t banks[2] = { CP_JOURNAL_BANK_0, CP_JOURNAL_BANK_1 };
static unsigned char empty[RECORD], enrolling[RECORD], next[RECORD];

static int Read(void *arg, uint32_t offset, void *data, size_t size)
{
    (void)arg;
    if (offset + size > sizeof(bytes)) return -1;
    memcpy(data, bytes + offset, size);
    return 0;
}
static int Write(void *arg, uint32_t offset, const void *data, size_t size)
{
    (void)arg;
    if (offset + size > sizeof(bytes)) return -1;
    memcpy(bytes + offset, data, size);
    return 0;
}
static int Sync(void *arg) { (void)arg; return 0; }
static int Size(void *arg, uint64_t *size) { (void)arg; *size = sizeof(bytes); return 0; }
static CpFile file = { NULL, Read, Write, Sync, Size };

/* EMPTY (as provisioned), the engine's initial ENROLLING record after it,
 * and the IDLE record that follows that. */
static void Records(unsigned hints)
{
    CpJournalHostStore host;
    WtJournalIO io = CpJournalHostIO(&host, &file);
    WtJournal journal;
    WtJournalEntry entry;
    WtWireRequest request;
    unsigned char seed[32];
    size_t size;
    unsigned i;
    memset(bytes, 0xFF, sizeof(bytes));
    memset(seed, 0x5A, sizeof(seed));
    assert(CpSaveJournal_Provision(&file, &journal, io, seed) == 0);
    assert(WtJournal_Read(&journal, &entry) == 0 && entry.phase == WT_JOURNAL_EMPTY);
    WtJournal_Close(&journal);
    memcpy(empty, bytes + banks[0], RECORD);
    for (i = 0; i < 32; i++) entry.credential[i] = (unsigned char)(0xC1 + 7 * i);
    for (i = 0; i < 16; i++) entry.enrollmentID[i] = (unsigned char)(0x31 + 11 * i);
    memset(&request, 0, sizeof(request));
    request.operation = WT_OP_ENROLL;
    memcpy(request.requestID, entry.enrollmentID, 16); memcpy(request.credential, entry.credential, 32);
    request.body.enroll.build = 0x01020304u;
    request.body.enroll.hintLength = (unsigned char)hints;
    memset(request.body.enroll.hints, 0x48, hints);
    assert(WtWire_Encode(&request, CpSaveJournal_Hash, entry.request, sizeof(entry.request), &size) == 0);
    entry.requestLength = (uint16_t)size;
    entry.phase = WT_JOURNAL_ENROLLING; entry.nextCounter = 1; entry.generation++;
    assert(WtJournal_Encode(&entry, CpSaveJournal_Hash, enrolling) == 0);
    entry.phase = WT_JOURNAL_IDLE; entry.generation++;
    memset(entry.saveID, 0x77, 16); entry.eventCursor = 1;
    entry.requestLength = 0; memset(entry.request, 0, sizeof(entry.request));
    assert(WtJournal_Encode(&entry, CpSaveJournal_Hash, next) == 0);
}

/* The companion's bank write: the tag is zeroed and synced first, then the
 * body's sectors persist in order[], the last of them only up to bytes. */
static void Cut(unsigned char *bank, const unsigned char *old, const unsigned char *new,
                const unsigned *order, unsigned sectors, unsigned bytesOfNext)
{
    unsigned i;
    memcpy(bank, old, RECORD);
    memset(bank + BODY, 0, 4);
    for (i = 0; i < sectors; i++) {
        unsigned start = order[i] * SECTOR, end = start + SECTOR > BODY ? BODY : start + SECTOR;
        memcpy(bank + start, new + start, end - start);
    }
    if (sectors < 8 && bytesOfNext) {
        unsigned start = order[sectors] * SECTOR, end = start + SECTOR > BODY ? BODY : start + SECTOR;
        if (start + bytesOfNext < end) end = start + bytesOfNext;
        memcpy(bank + start, new + start, end - start);
    }
}

static int Opens(const unsigned char *expected, unsigned *settled)
{
    CpJournalHostStore host;
    WtJournalIO io = CpJournalHostIO(&host, &file);
    WtJournal journal;
    WtJournalEntry entry, expect;
    int result;
    if (CpSaveJournal_Detect(&file) != CP_SAVE_JOURNAL_HAS_JOURNAL) return 0;
    if (WtJournal_Open(&journal, io) != 0) return 0;
    assert(WtJournal_Read(&journal, &entry) == 0);
    assert(WtJournal_Decode(expected, CpSaveJournal_Hash, &expect) == 0);
    result = memcmp(&entry, &expect, sizeof(entry)) == 0;
    /* The journal continues: the next commit overwrites the torn bank. */
    if (result) {
        WtJournalEntry following;
        const unsigned char *after = expect.phase == WT_JOURNAL_EMPTY ? enrolling : next;
        assert(WtJournal_Decode(after, CpSaveJournal_Hash, &following) == 0);
        assert(WtJournal_Commit(&journal, &following) == 0);
        assert(!memcmp(bytes + banks[0], after, RECORD) && !memcmp(bytes + banks[1], after, RECORD));
        ++*settled;
    }
    WtJournal_Close(&journal);
    return result;
}

static const unsigned orders[3][8] = {
    { 0, 1, 2, 3, 4, 5, 6, 7 },   /* in order */
    { 7, 6, 5, 4, 3, 2, 1, 0 },   /* tail first */
    { 3, 4, 2, 5, 1, 6, 0, 7 }    /* middle first */
};
static const char *orderNames[3] = { "prefix", "tail-first", "middle-first" };

/* Both writes of the first enrollment commit, into either bank: the first
 * leaves the other bank EMPTY (open on EMPTY), the second leaves it holding
 * the ENROLLING record (open on that). Every sector boundary in each order,
 * every byte within the header sector, and every byte of the commit tag. */
static void Sweep(void)
{
    const unsigned hintLengths[3] = { 0, 23, 64 };
    unsigned h, o, write, bank, sectors, part, cases = 0, settled = 0;
    for (h = 0; h < 3; h++) {
        Records(hintLengths[h]);
        for (o = 0; o < 3; o++) for (write = 0; write < 2; write++) for (bank = 0; bank < 2; bank++) {
            for (sectors = 0; sectors <= 8; sectors++) {
                /* Every byte of the header sector, with one packet size. */
                unsigned parts = h == 2 && sectors < 8 && orders[o][sectors] == 0 ? SECTOR : 1;
                for (part = 0; part < parts; part++) {
                    const unsigned char *other = write ? enrolling : empty;
                    memcpy(bytes + banks[bank ^ 1], other, RECORD);
                    Cut(bytes + banks[bank], empty, enrolling, orders[o], sectors, part);
                    if (!Opens(other, &settled)) {
                        fprintf(stderr, "FAIL hints=%u order=%s write=%u bank=%u sectors=%u bytes=%u\n",
                            hintLengths[h], orderNames[o], write + 1, bank, sectors, part);
                        assert(0);
                    }
                    cases++;
                }
            }
            /* The body is whole; the tag is cut at each byte, then complete. */
            for (part = 1; part <= 4; part++) {
                memcpy(bytes + banks[bank ^ 1], write ? enrolling : empty, RECORD);
                memcpy(bytes + banks[bank], enrolling, RECORD);
                memset(bytes + banks[bank] + BODY + part, 0, 4 - part);
                assert(Opens(part == 4 || write ? enrolling : empty, &settled));
                cases++;
            }
        }
    }
    assert(settled == cases);
    printf("PASS torn first enrollment: %u cuts open on the record before or after the write and commit on\n", cases);
}

/* The reproduction's shape: tail first, cut at the first sector boundary. */
static void Reproduction(void)
{
    unsigned settled = 0;
    Records(64);
    memcpy(bytes + banks[0], empty, RECORD);
    Cut(bytes + banks[1], empty, enrolling, orders[1], 7, 0);
    assert(!memcmp(bytes + banks[1], empty, 512) && memcmp(bytes + banks[1] + 1952, empty + 1952, 264));
    assert(Opens(empty, &settled) && settled == 1);
    puts("PASS reproduction: EMPTY header over the enrollment packet opens on the EMPTY bank");
}

static void Refused(const char *what)
{
    CpJournalHostStore host;
    WtJournalIO io = CpJournalHostIO(&host, &file);
    WtJournal journal;
    if (CpSaveJournal_Detect(&file) != CP_SAVE_JOURNAL_REFUSE || WtJournal_Open(&journal, io) == 0) {
        fprintf(stderr, "FAIL accepted %s\n", what);
        assert(0);
    }
}

/* Torn banks next to a valid one that are not a cut first enrollment write
 * stay refused, as do torn shapes without a valid other bank. */
static void Negatives(void)
{
    static const struct { unsigned offset; unsigned char value; const char *what; } changes[] = {
        { 0, 'X', "a foreign magic" }, { 6, 2, "another phase" }, { 7, 1, "a high phase byte" },
        { 17, 9, "a generation from elsewhere" }, { 32, 1, "a save ID" }, { 97, 1, "a large next counter" },
        { 104, 1, "an event cursor" }, { 160, 1, "an operation" }, { 1444, 0x11, "another seed" },
        { 3000, 1, "reserved bytes" }, { 1952 + 13, 2, "a Submit opcode" }, { 1952 + 8, 'X', "a foreign frame magic" },
        { 1952 + 12, 9, "an unknown wire version" }, { 1952 + 32, 1, "a save ID in the packet" },
        { 1952 + 80, 1, "a nonzero packet counter" }, { 1952 + 3, 1, "a reserved request header" },
        { 1952 + 260, 1, "bytes after the frame" }, { 1952 + 0, 200, "an impossible length" },
        { 1952 + 92, 3, "a hint length the frame size contradicts" }
    };
    unsigned i, bank;
    for (bank = 0; bank < 2; bank++) {
        for (i = 0; i < sizeof(changes) / sizeof(*changes); i++) {
            Records(64);
            memcpy(bytes + banks[bank ^ 1], empty, RECORD);
            Cut(bytes + banks[bank], empty, enrolling, orders[1], 7, 0);
            bytes[banks[bank] + changes[i].offset] = changes[i].value;
            Refused(changes[i].what);
        }
        /* The record's own identity contradicts the packet's. */
        Records(64);
        memcpy(bytes + banks[bank ^ 1], empty, RECORD);
        Cut(bytes + banks[bank], empty, enrolling, orders[1], 7, 0);
        bytes[banks[bank] + 48] = (unsigned char)(enrolling[48] ^ 0x80);
        Refused("a credential the packet contradicts");
        /* Next to the ENROLLING record: a packet that is not that record's. */
        Records(64);
        memcpy(bytes + banks[bank ^ 1], enrolling, RECORD);
        Cut(bytes + banks[bank], empty, enrolling, orders[1], 7, 0);
        bytes[banks[bank] + 1952 + 20] ^= 0x01;
        Refused("a packet other than the ENROLLING bank's");
        /* Next to an IDLE record, the same torn bank is not a first write. */
        Records(64);
        memcpy(bytes + banks[bank ^ 1], next, RECORD);
        Cut(bytes + banks[bank], empty, enrolling, orders[1], 7, 0);
        Refused("a torn enrollment next to an IDLE bank");
        /* Both banks torn: nothing valid to open on, and not unused. */
        Records(64);
        Cut(bytes + banks[bank ^ 1], empty, enrolling, orders[1], 7, 0);
        Cut(bytes + banks[bank], empty, enrolling, orders[1], 6, 0);
        Refused("two torn banks");
        /* A sealed bank that is not a valid record. */
        Records(64);
        memcpy(bytes + banks[bank ^ 1], empty, RECORD);
        Cut(bytes + banks[bank], empty, enrolling, orders[1], 7, 0);
        memcpy(bytes + banks[bank] + BODY, "CMIT", 4);
        assert(CpSaveJournal_Hash(bytes + banks[bank], 4060, bytes + banks[bank] + 4060) == 0);
        Refused("a sealed record that does not decode");
    }
    puts("PASS foreign or contradicting torn banks are still refused");
}

int main(void)
{
    Reproduction();
    Sweep();
    Negatives();
    return 0;
}
