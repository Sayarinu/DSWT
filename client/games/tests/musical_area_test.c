/* Black 2/White 2: leftover musical data in the journal banks is erased, one
 * bank at a time, after the musical area's release is durable and before the
 * journal is created. Every interruption leaves no journal or a valid one. */
#include "wonder_trade/bw2/musical_area.h"
#include "wonder_trade/journal.h"

#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <psa/crypto.h>

#define PAGE 256

static unsigned char banks[2][4096];
static int marker, steps, killAt, erases, readFailure, reads;
static jmp_buf killed;
static const char kMusical[] = "musical program!";

static void Step(void)
{
    if (++steps == killAt) longjmp(killed, 1);
}

static int Hash(const unsigned char *bytes, size_t size, unsigned char digest[32])
{
    size_t actual;
    return psa_hash_compute(PSA_ALG_SHA_256, bytes, size, digest, 32, &actual) == PSA_SUCCESS && actual == 32 ? 0 : -1;
}

static int Read(void *arg, int bank, unsigned char bytes[4096])
{
    (void)arg;
    if (++reads == readFailure) return -1;
    memcpy(bytes, banks[bank], 4096);
    return 0;
}

/* Flash is programmed a page at a time; a kill can land between any two. */
static int Erase(void *arg, int bank)
{
    int page;
    (void)arg;
    ++erases;
    for (page = 0; page < 4096 / PAGE; ++page) {
        Step();
        memset(banks[bank] + page * PAGE, 0xff, PAGE);
    }
    return 0;
}

static void Program(int bank, int offset, const unsigned char *bytes, int length)
{
    int done;
    for (done = 0; done < length; done += PAGE) {
        int size = length - done < PAGE ? length - done : PAGE;
        Step();
        memcpy(banks[bank] + offset + done, bytes + done, size);
    }
}

/* Tag-last, as wonder_trade/flash.c writes a bank. */
static int Write(void *arg, int bank, const unsigned char record[4096])
{
    static const unsigned char uncommitted[4] = { 0, 0, 0, 0 };
    (void)arg;
    Program(bank, 4092, uncommitted, 4);
    Program(bank, 0, record, 4092);
    Program(bank, 4092, record + 4092, 4);
    return 0;
}

static const WtMusicalAreaIO kArea = { NULL, Read, Erase };
static const WtJournalIO kJournal = { NULL, Read, Write, Hash };

static void Fill(int bank, int musical, unsigned char blank)
{
    int offset;
    memset(banks[bank], blank, 4096);
    if (musical) for (offset = 0; offset < 4096; offset += 16) memcpy(banks[bank] + offset, kMusical, 16);
}

static int Musical(void)
{
    int bank, offset;
    for (bank = 0; bank < 2; ++bank)
        for (offset = 0; offset < 4096; offset += 16)
            if (memcmp(banks[bank] + offset, kMusical, 16) == 0) return 1;
    return 0;
}

/* The adapter's order on opening Wonder Trade before a journal exists: the
 * release (marker cleared and both save copies written), the erase, then the
 * journal. */
static int Open(void)
{
    WtJournal store;
    unsigned char seed[32];
    int result;
    if (WtMusicalArea_Inspect(kArea) != WT_MUSICAL_AREA_JOURNAL) { Step(); marker = 0; }
    if (WtMusicalArea_Erase(kArea) != 0) return -1;
    result = WtJournal_Open(&store, kJournal);
    if (result == WT_JOURNAL_ERROR_UNKNOWN) {
        memset(seed, 9, sizeof(seed));
        result = WtJournal_Initialize(&store, kJournal, seed);
    }
    WtJournal_Close(&store);
    return result;
}

static void JournalValid(void)
{
    WtJournal store;
    WtJournalEntry entry;
    assert(WtJournal_Open(&store, kJournal) == 0 && WtJournal_Read(&store, &entry) == 0);
    assert(entry.generation == 1 && entry.phase == WT_JOURNAL_EMPTY);
    WtJournal_Close(&store);
    assert(WtMusicalArea_Inspect(kArea) == WT_MUSICAL_AREA_JOURNAL && !Musical());
}

static void Reset(int first, int second, int set)
{
    Fill(0, first, 0xff); Fill(1, second, 0xff);
    marker = set; steps = killAt = erases = readFailure = reads = 0;
}

int main(void)
{
    unsigned char before[2][4096];
    int first, second, kill, total, bank, failure;
    /* Blank banks: nothing is erased, and the journal is created as before. */
    for (bank = 0; bank < 2; ++bank) {
        Reset(0, 0, 0); Fill(0, 0, bank ? 0 : 0xff); Fill(1, 0, 0xff);
        assert(WtMusicalArea_Inspect(kArea) == WT_MUSICAL_AREA_FREE);
        memcpy(before, banks, sizeof(before));
        assert(WtMusicalArea_Erase(kArea) == 0 && erases == 0 && memcmp(before, banks, sizeof(before)) == 0);
        assert(Open() == 0 && erases == 0);
        JournalValid();
        /* A journal is never erased. */
        memcpy(before, banks, sizeof(before));
        assert(WtMusicalArea_Erase(kArea) == 0 && erases == 0 && memcmp(before, banks, sizeof(before)) == 0);
    }
    /* Leftover data in either or both banks, any interruption. */
    for (first = 0; first < 2; ++first) {
        for (second = 0; second < 2; ++second) {
            if (!first && !second) continue;
            Reset(first, second, 1);
            assert(WtMusicalArea_Inspect(kArea) == WT_MUSICAL_AREA_LEFTOVER);
            if (setjmp(killed) == 0) assert(Open() == 0);
            total = steps;
            JournalValid();
            for (kill = 1; kill <= total; ++kill) {
                Reset(first, second, 1);
                killAt = kill;
                if (setjmp(killed) == 0) { Open(); assert(!"the kill point was not reached"); }
                /* The marker is cleared on the card before the first erase. */
                assert(!marker || kill == 1);
                if (marker) assert(Musical());
                /* Never a journal beside leftover data. */
                if (Musical()) assert(WtMusicalArea_Inspect(kArea) == WT_MUSICAL_AREA_LEFTOVER);
                /* The next start finishes: unless the very first journal write
                 * was torn after its magic, which the journal core reports. */
                killAt = 0;
                if (WtMusicalArea_Inspect(kArea) != WT_MUSICAL_AREA_JOURNAL) {
                    assert(Open() == 0);
                    JournalValid();
                }
            }
        }
    }
    /* Unreadable banks are reported and never erased. */
    for (failure = 1; failure <= 3; ++failure) {
        Reset(1, 1, 0); readFailure = failure;
        memcpy(before, banks, sizeof(before));
        assert(WtMusicalArea_Erase(kArea) != 0 && erases == 0 && memcmp(before, banks, sizeof(before)) == 0);
    }
    /* A torn first journal write that left only its cleared tag is no journal. */
    Reset(0, 0, 0); memset(banks[0] + 4092, 0, 4);
    assert(WtMusicalArea_Inspect(kArea) == WT_MUSICAL_AREA_LEFTOVER);
    assert(Open() == 0);
    JournalValid();
    puts("Musical area release, erase and journal creation passed");
    return 0;
}
