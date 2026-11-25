#define _POSIX_C_SOURCE 200809L
#include "../adapter/save_fixture.h"

#include <fcntl.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include "save/journal_host_io.h"
#include "save/posix_file.h"
#include "session/visit.h"

static int Hash(const unsigned char *bytes, size_t size, unsigned char digest[32])
{
    return FxSha(bytes, size, digest);
}
#include "fake_server.h"
#include "retail_load.h"

/* The supervisor continues the uninterrupted operation. At each boundary a
 * forked child abandons the call stack, discards all client state, and opens
 * a private POSIX copy of just the bytes that survived the cut. No error is
 * returned to the interrupted writer, so its cleanup/retry cannot hide a bug.
 * _exit also discards allocations abandoned by the simulated power cut.
 *
 * Every recovery must pass: there are no known failures.
 * CP_FAULT_FULL=1 sweeps every journal byte, sector boundaries and odd save
 * offsets in both persistence orders; CI samples distinct journal cuts per
 * inventory step and cut kind (with separate first/odd/last-byte prefix tears),
 * while retaining cuts at every save write and sync.
 * CP_FAULT_SCENARIO selects one of names[] for a focused reproduction. */
enum Scenario { PROVISION, OFFER, CANCEL_RETURN, REJECT_RESTORE, CHECKPOINT, SCENARIOS };
static const char *names[] = { "provision", "offer", "cancel-return", "reject-restore", "checkpoint" };
static const char *steps[] = { "checkpoint/network", "prepare-remove", "remove", "verify-removed",
    "prepare-receive", "receive", "verify-received", "restore", "prepare-restore" };
enum CutKind { BEFORE_WRITE, LOST_WRITE, TORN_PREFIX, TAIL_FIRST, AFTER_WRITE, BEFORE_SYNC, AFTER_SYNC, CUT_KINDS };
static const char *cutNames[] = { "before-write", "lost-write", "torn-prefix", "tail-first", "after-write",
    "before-sync-lost-pending", "after-sync" };
static unsigned sampled[sizeof(steps) / sizeof(*steps)], cutMask;
static int journalWrite;
static enum Scenario scenario;
static jmp_buf reboot;
static CpSaveImage image;
static WtGame game;
static CpPosixFile posix;
static CpFile disk, file;
static CpJournalHostStore host;
static WtJournalIO journalIO;
static FxFile fixture;
static unsigned char live[CP_SAVE_FILE_SIZE], durable[CP_SAVE_FILE_SIZE];
static unsigned char digests[4][32];
static unsigned char expectedParty[CP_ADAPTER_PARTY_SIZE], expectedPC[540 * FX_BOX_MON];
enum Outcome { RECOVERED, JOURNAL_BLOCKED, SAVE_BLOCKED, STUCK, DUPLICATE, LOST, OUTCOMES };
static const char *outcomes[] = { "recovered", "journal-blocked", "save-blocked", "stuck", "duplicate", "lost" };
static unsigned unexpected;
static unsigned randomCounter, eventCount, saveWrites, journalWrites, syncs;
static unsigned attempts, duplicates, distinct, tally[OUTCOMES];
static unsigned prefixAttempts, tailAttempts, prefixStates, tailStates, stockCases, stockRecovered;
static unsigned journalByteTears, expectedJournalByteTears;
/* Hash both disk bytes and surviving server/replay state: equal files with a
 * different server history are different recovery cases. */
static unsigned char (*seen)[32];
static size_t seenCapacity;
static unsigned char crashBytes[CP_SAVE_FILE_SIZE], lastCrash[CP_SAVE_FILE_SIZE];
static unsigned lastEvent;
static unsigned step, stepMask, stockSaves;
static uint32_t stopOffset;
static size_t stopSize;
static int inject, counting, full, stockPlayed;
static char path[sizeof(CP_FAULT_BINARY_DIR) + 32];

/* Every distinct cut must recover; there are no known failures. */
static void CheckOutcome(const char *kind, size_t tear, unsigned stock, unsigned outcome)
{
    if (outcome == RECOVERED) return;
    ++unexpected;
    printf("FAIL unexpected-recovery %s event=%u kind=%s tear=%zu stock-saves=%u outcome=%s\n",
        names[scenario], eventCount, kind, tear, stock, outcomes[outcome]);
}

static int Random(void *arg, unsigned char *bytes, size_t size)
{
    unsigned char input[4], digest[32];
    (void)arg;
    assert(size <= sizeof(digest));
    FxPut32(input, ++randomCounter); FxSha(input, sizeof(input), digest);
    memcpy(bytes, digest, size);
    return 0;
}

static int Inventory(void *arg, enum WtInventoryStep operation, WtJournalEntry *entry, unsigned slot)
{
    int result;
    step = (unsigned)operation + 1;
    if (counting) stepMask |= 1u << operation;
    result = WtGame_Inventory(arg, operation, entry, slot);
    step = 0;
    return result;
}

static void OpenEngine(WtEngine *engine)
{
    WtEngineIO io = { &game, Random, Inventory, WtGame_CanHold };
    assert(WtEngine_Open(engine, journalIO, io) == 0);
}

static void OpenBytes(const unsigned char *bytes)
{
    int fd;
    strcpy(path, CP_FAULT_BINARY_DIR "/dswt-fault-XXXXXX");
    fd = mkstemp(path);
    assert(fd >= 0 && ftruncate(fd, CP_SAVE_FILE_SIZE) == 0);
    assert(CpPosixFile_Init(&disk, &posix, fd) == 0);
    assert(disk.writeAt(disk.context, 0, bytes, CP_SAVE_FILE_SIZE) == 0);
    assert(disk.sync(disk.context) == 0);
    /* Close/reopen even the POSIX handle, rather than reusing a cached image. */
    assert(close(fd) == 0);
    fd = open(path, O_RDWR);
    assert(fd >= 0 && CpPosixFile_Init(&disk, &posix, fd) == 0);
    assert(unlink(path) == 0); /* Also cleans up children that abort on an invariant. */
    if (bytes != live) memcpy(live, bytes, sizeof(live));
    memcpy(durable, bytes, sizeof(durable));
}

static void CloseFile(void)
{
    assert(close(posix.fd) == 0);
}

static void Finish(enum Outcome outcome)
{
    CloseFile();
    _exit((int)outcome + (stockPlayed ? OUTCOMES : 0));
}

static size_t HashSlot(const unsigned char digest[32], size_t capacity)
{
    return Fx32(digest) & (capacity - 1);
}

static int Seen(const unsigned char digest[32])
{
    size_t slot;
    if (!seenCapacity || distinct * 2 >= seenCapacity) {
        size_t oldCapacity = seenCapacity, i;
        unsigned char (*old)[32] = seen;
        seenCapacity = seenCapacity ? seenCapacity * 2 : 256;
        seen = calloc(seenCapacity, sizeof(*seen));
        assert(seen);
        for (i = 0; i < oldCapacity; ++i) if (memcmp(old[i], (unsigned char[32]){0}, 32) != 0) {
            slot = HashSlot(old[i], seenCapacity);
            while (memcmp(seen[slot], (unsigned char[32]){0}, 32) != 0) slot = (slot + 1) & (seenCapacity - 1);
            memcpy(seen[slot], old[i], 32);
        }
        free(old);
    }
    assert(memcmp(digest, (unsigned char[32]){0}, 32) != 0);
    slot = HashSlot(digest, seenCapacity);
    while (memcmp(seen[slot], (unsigned char[32]){0}, 32) != 0) {
        if (memcmp(seen[slot], digest, 32) == 0) return 1;
        slot = (slot + 1) & (seenCapacity - 1);
    }
    memcpy(seen[slot], digest, 32);
    return 0;
}

/* A prefix persists bytes before tear; tail-first persists the sectors after
 * tear while leaving the head old. Both start with only durable earlier writes. */
static void Cut(enum CutKind kind, int losePending, uint32_t offset, const void *data, size_t size,
    size_t tear, int tail)
{
    pid_t child;
    int status, played = 0;
    unsigned plan, worst = RECOVERED;
    unsigned char digest[32];
    CpSaveSha256 hash;
    unsigned *sample;
    unsigned sampleBit = 1u << kind;
    const char *model = cutNames[kind];
    if (!inject || !counting) return;
    cutMask |= 1u << kind;
    sample = &sampled[step];
    if (kind == TORN_PREFIX) {
        if (tear == 1) sampleBit = 1u << CUT_KINDS;
        else if (tear == size - 1) sampleBit = 1u << (CUT_KINDS + 1);
        else if (tear == 513) sampleBit = 1u << (CUT_KINDS + 2);
    }
    if (!full && journalWrite && (*sample & sampleBit)) return;
    ++attempts;
    if (data) { if (tail) ++tailAttempts; else ++prefixAttempts; }
    memcpy(crashBytes, losePending ? durable : live, sizeof(crashBytes));
    if (data) {
        assert(tear > 0 && tear < size);
        if (tail) memcpy(crashBytes + offset + tear, (const unsigned char *)data + tear, size - tear);
        else memcpy(crashBytes + offset, data, tear);
    }
    /* Adjacent byte offsets in a mostly unchanged journal record often
     * produce identical files. Avoid hashing the whole save for those aliases. */
    if (lastEvent == eventCount && memcmp(lastCrash, crashBytes, sizeof(lastCrash)) == 0) {
        ++duplicates; return;
    }
    lastEvent = eventCount; memcpy(lastCrash, crashBytes, sizeof(lastCrash));
    CpSaveSha256_Init(&hash);
    CpSaveSha256_Update(&hash, crashBytes, sizeof(crashBytes));
    CpSaveSha256_Update(&hash, (const unsigned char *)&server, sizeof(server));
    CpSaveSha256_Update(&hash, (const unsigned char *)&replayCount, sizeof(replayCount));
    CpSaveSha256_Update(&hash, (const unsigned char *)replay, replayCount * sizeof(*replay));
    CpSaveSha256_Update(&hash, (const unsigned char *)&randomCounter, sizeof(randomCounter));
    CpSaveSha256_Finish(&hash, digest);
    if (Seen(digest)) { ++duplicates; return; }
    if (journalWrite) *sample |= sampleBit;
    ++distinct;
    if (data) { if (tail) ++tailStates; else ++prefixStates; }
    /* The prompt says save once. Also check two stock saves, which refresh
     * both alternating halves, so neither recovery path can mask the other. */
    for (plan = 1; plan <= 2; ++plan) {
        unsigned outcome;
        fflush(NULL);
        child = fork();
        assert(child >= 0);
        if (child == 0) {
            inject = 0; stockPlayed = 0; stockSaves = plan;
            memcpy(live, crashBytes, sizeof(live));
            longjmp(reboot, 1);
        }
        assert(waitpid(child, &status, 0) == child);
        if (!WIFEXITED(status) || WEXITSTATUS(status) >= 2 * OUTCOMES) {
            fprintf(stderr, "FAIL %s event=%u model=%s step=%s offset=%u size=%zu tear=%zu stock-saves=%u status=%d\n",
                names[scenario], eventCount, model, steps[step], stopOffset, stopSize, tear, plan, status);
            CloseFile();
            exit(1);
        }
        outcome = (unsigned)WEXITSTATUS(status) % OUTCOMES;
        played = WEXITSTATUS(status) >= OUTCOMES;
        if (outcome > worst) worst = outcome;
        CheckOutcome(model, tear, played ? plan : 0, outcome);
        if (!played) break;
    }
    /* Each disk/server state counts once, using its worst recovery outcome. */
    ++tally[worst];
    if (played) { ++stockCases; if (worst == RECOVERED) ++stockRecovered; }

}

static void Tear(size_t tear, uint32_t offset, const void *bytes, size_t size, int tail)
{
    if (tear > 0 && tear < size)
        Cut(tail ? TAIL_FIRST : TORN_PREFIX, 1, offset, bytes, size, tear, tail);
}

static int ReadAt(void *arg, uint32_t offset, void *bytes, size_t size)
{
    (void)arg;
    return disk.readAt(disk.context, offset, bytes, size);
}

static int JournalOffset(uint32_t offset)
{
    return (offset >= CP_JOURNAL_BANK_0 && offset < CP_JOURNAL_BANK_0 + CP_JOURNAL_RESERVED_SIZE)
        || (offset >= CP_JOURNAL_BANK_1 && offset < CP_JOURNAL_BANK_1 + CP_JOURNAL_RESERVED_SIZE);
}

static int WriteAt(void *arg, uint32_t offset, const void *bytes, size_t size)
{
    int result;
    (void)arg;
    journalWrite = JournalOffset(offset); /* Also classifies the following sync. */
    if (counting) {
        ++eventCount; stopOffset = offset; stopSize = size;
        if (JournalOffset(offset)) { ++journalWrites; expectedJournalByteTears += (unsigned)size - 1; }
        else ++saveWrites;
    }
    if (inject && counting) {
        size_t tear;
        Cut(BEFORE_WRITE, 0, 0, NULL, 0, 0, 0);
        Cut(LOST_WRITE, 1, 0, NULL, 0, 0, 0);
        if (full) {
            /* Journal records: every byte. Save writes: every absolute sector
             * boundary, including writes that start inside a sector. */
            size_t stride = JournalOffset(offset) ? 1 : 512;
            size_t first = stride == 1 ? 1 : 512 - offset % 512;
            for (tear = first; tear < size; tear += stride) {
                if (JournalOffset(offset)) ++journalByteTears;
                Tear(tear, offset, bytes, size, 0);
            }
        } else {
            Tear(512 - offset % 512, offset, bytes, size, 0);
        }
        Tear(1, offset, bytes, size, 0);
        Tear(513, offset, bytes, size, 0);
        Tear(size - 1, offset, bytes, size, 0);
        /* Persist complete tail sectors before any head sector. Full mode
         * sweeps every boundary; CI samples the first boundary. */
        for (tear = 512 - offset % 512; tear < size; tear += 512) {
            if (full || tear < 513)
                Tear(tear, offset, bytes, size, 1);
        }
    }
    result = disk.writeAt(disk.context, offset, bytes, size);
    assert(result == 0);
    memcpy(live + offset, bytes, size);
    Cut(AFTER_WRITE, 0, 0, NULL, 0, 0, 0);
    return result;
}

static int Sync(void *arg)
{
    (void)arg;
    if (counting) { ++eventCount; ++syncs; stopOffset = 0; stopSize = 0; }
    Cut(BEFORE_SYNC, 1, 0, NULL, 0, 0, 0);
    assert(disk.sync(disk.context) == 0);
    memcpy(durable, live, sizeof(durable));
    Cut(AFTER_SYNC, 0, 0, NULL, 0, 0, 0);
    return 0;
}

static int Size(void *arg, uint64_t *size)
{
    (void)arg;
    return disk.size(disk.context, size);
}

static void Bind(void)
{
    CpFile hooked = { NULL, ReadAt, WriteAt, Sync, Size };
    file = hooked;
    memset(&image, 0, sizeof(image)); memset(&game, 0, sizeof(game));
    game.image = &image; game.file = &file;
    journalIO = CpJournalHostIO(&host, &file);
}

static void Provision(void)
{
    unsigned char seed[32];
    WtJournal journal;
    memset(seed, 4, sizeof(seed)); /* Synthetic entropy, not player credentials. */
    assert(CpSaveJournal_Provision(&file, &journal, journalIO, seed) == 0);
    WtJournal_Close(&journal);
}

static WtJournalEntry ReadEntry(void)
{
    WtJournal journal;
    WtJournalEntry entry;
    assert(WtJournal_Open(&journal, journalIO) == 0);
    assert(WtJournal_Read(&journal, &entry) == 0);
    WtJournal_Close(&journal);
    return entry;
}

/* Stock LoadCheck chooses the RAM blocks, then play time changes and a full
 * stock save writes both blocks to their alternate halves. A second save, when
 * requested by Cut, refreshes the other halves too. No companion load/commit
 * implementation or rejected image participates in this recovery model. */
static void StockGameSave(void)
{
    unsigned selected[2], block, pass;
    uint32_t global, counters[2];
    unsigned char ram[CP_SAVE_HALF_SIZE];
    RetailLoad(live, selected);
    for (block = 0; block < 2; ++block) {
        const unsigned char *body = live + selected[block] * CP_SAVE_HALF_SIZE + blockOffsets[block];
        const unsigned char *footer = body + blockSizes[block] - 20;
        memcpy(ram + blockOffsets[block], body, blockSizes[block]);
        counters[block] = Fx32(footer + 4);
    }
    global = Fx32(ram + FX_GENERAL_SIZE - 20);
    ram[0x10] ^= 0x5a; /* The player plays: play time changes. */
    for (pass = 0; pass < stockSaves; ++pass) {
        ++global;
        for (block = 0; block < 2; ++block) {
            unsigned char *body, *footer;
            unsigned crc;
            selected[block] ^= 1;
            body = live + selected[block] * CP_SAVE_HALF_SIZE + blockOffsets[block];
            memcpy(body, ram + blockOffsets[block], blockSizes[block]);
            footer = body + blockSizes[block] - 20;
            FxPut32(footer, global); FxPut32(footer + 4, ++counters[block]);
            crc = FxCrc(body, blockSizes[block] - 20);
            FxPut16(footer + 18, crc);
        }
    }
    stockPlayed = 1;
    CloseFile(); OpenBytes(live); Bind();
}

static int Visit(int slot, unsigned action)
{
    WtEngine engine;
    CpVisit visit;
    int result;
    OpenEngine(&engine);
    result = CpVisit_Run(&engine, &game, 7, slot, action, &transport, &visit);
    WtEngine_Close(&engine);
    return result == 0 && visit.canLeave
        && visit.phase == (scenario == OFFER ? WT_JOURNAL_QUEUED : WT_JOURNAL_IDLE) ? 0 : -1;
}

static void CheckFinal(int capture)
{
    WtJournalEntry entry = ReadEntry();
    const unsigned char *party, *pc;
    assert(CpSaveImage_Load(&image, &file, CP_SAVE_MAGIC_INTERNATIONAL) == CP_SAVE_OK);
    Conservation(live, &entry, digests);
    party = CpSaveImage_Block(&image, 0) + FX_PARTY;
    pc = CpSaveImage_Block(&image, 1) + FX_PC;
    if (capture) {
        memcpy(expectedParty, party, sizeof(expectedParty)); memcpy(expectedPC, pc, sizeof(expectedPC));
    } else if (!stockPlayed) {
        /* Playing can trigger the restore fallback and change party order. */
        assert(memcmp(expectedParty, party, sizeof(expectedParty)) == 0);
        assert(memcmp(expectedPC, pc, sizeof(expectedPC)) == 0);
    }
    assert(entry.phase == (scenario == OFFER ? WT_JOURNAL_QUEUED : WT_JOURNAL_IDLE));
}

static void Resume(void)
{
    WtEngine engine;
    WtJournalEntry entry, settled;
    unsigned char after[CP_SAVE_FILE_SIZE];
    CpSaveResult load;
    int slot = -1;
    /* The child inherits the server only. Recreate all companion handles. */
    assert(close(posix.fd) == 0);
    OpenBytes(live);
    Bind(); randomCounter += 1000; /* Fresh entropy after reboot. */
    load = CpSaveImage_Load(&image, &file, CP_SAVE_MAGIC_INTERNATIONAL);
    if (CpSaveJournal_Detect(&file) == CP_SAVE_JOURNAL_UNUSED) {
        assert(scenario == PROVISION);
        Provision();
    }
    {
        WtJournal journal;
        int result = WtJournal_Open(&journal, journalIO);
        if (result != 0) {
            WtJournalEntry available[2];
            int valid[2], bank;
            unsigned offsets[2] = { CP_JOURNAL_BANK_0, CP_JOURNAL_BANK_1 };
            for (bank = 0; bank < 2; ++bank)
                valid[bank] = WtJournal_Decode(live + offsets[bank], Hash, &available[bank]) == 0;
            assert(valid[0] || valid[1]);
            bank = !valid[0] || (valid[1] && available[1].generation > available[0].generation);
            Conservation(live, &available[bank], digests);
            assert(valid[0] != valid[1]);
            Finish(JOURNAL_BLOCKED);
        }
        assert(WtJournal_Read(&journal, &entry) == 0);
        WtJournal_Close(&journal);
    }
    Conservation(live, &entry, digests);
    if (load == CP_SAVE_NEEDS_GAME_SAVE) {
        assert(!image.loaded && CpSaveImage_Block(&image, 0) == NULL);
        assert(!WtGame_CanLeave(&game, &entry));
        StockGameSave();
        load = CpSaveImage_Load(&image, &file, CP_SAVE_MAGIC_INTERNATIONAL);
    }
    if (load != CP_SAVE_OK) Finish(SAVE_BLOCKED);
    OpenEngine(&engine);
    /* This is the local boot path used by WtBoot_Check, without DS glue/TLS. */
    {
        int result = WtEngine_Settle(&engine);
        int ownership = Ownership(live, &engine.entry, digests, stockPlayed);
        if (ownership > 0) Finish(DUPLICATE);
        if (ownership < 0) Finish(LOST);
        if (result != 0 || engine.entry.phase == WT_JOURNAL_ESCROW
            || engine.entry.phase == WT_JOURNAL_RESTORING || engine.entry.phase == WT_JOURNAL_APPLYING)
            Finish(STUCK);
    }
    settled = engine.entry; memcpy(after, live, sizeof(after));
    assert(Ownership(live, &settled, digests, stockPlayed) == 0);
    WtEngine_Close(&engine);
    OpenEngine(&engine);
    assert(WtEngine_Settle(&engine) == 0);
    assert(memcmp(&settled, &engine.entry, sizeof(settled)) == 0);
    assert(memcmp(after, live, sizeof(after)) == 0);
    assert(Ownership(live, &engine.entry, digests, stockPlayed) == 0);
    if ((scenario == OFFER || scenario == REJECT_RESTORE)
        && engine.entry.phase <= WT_JOURNAL_IDLE && server.eventCount <= 1) slot = 1;
    WtEngine_Close(&engine);
    if (scenario == CHECKPOINT) assert(WtGame_Checkpoint(&game) == 0);
    {
        int result = Visit(slot, scenario == CANCEL_RETURN ? WT_OP_CANCEL : 0);
        WtJournalEntry final = ReadEntry();
        int ownership = Ownership(live, &final, digests, stockPlayed);
        if (ownership > 0) Finish(DUPLICATE);
        if (ownership < 0) Finish(LOST);
        if (result != 0) Finish(STUCK);
    }
    CheckFinal(0);
    Finish(RECOVERED);
}

static void Setup(void)
{
    FxSpec specs[4];
    unsigned i;
    inject = counting = 0; step = 0; randomCounter = 0;
    memset(&server, 0, sizeof(server)); replayCount = 0;
    memset(replay, 0, sizeof(replay));
    for (i = 0; i < 4; ++i) {
        unsigned char canonical[136];
        specs[i] = FxDefault(i); FxCanonical(&specs[i], canonical); FxSha(canonical, sizeof(canonical), digests[i]);
    }
    FxBuild(&fixture, specs, 4);
    OpenBytes(fixture.bytes); Bind();
    assert(CpSaveImage_Load(&image, &file, CP_SAVE_MAGIC_INTERNATIONAL) == CP_SAVE_OK);
    if (scenario != PROVISION) Provision();
    if (scenario == CANCEL_RETURN) {
        WtEngine engine;
        CpVisit visit;
        OpenEngine(&engine);
        assert(CpVisit_Run(&engine, &game, 7, 1, 0, &transport, &visit) == 0);
        assert(visit.phase == WT_JOURNAL_QUEUED);
        WtEngine_Close(&engine);
    }
    if (scenario == REJECT_RESTORE) server.rejectSubmit = 1;
    eventCount = saveWrites = journalWrites = syncs = stepMask = 0;
    attempts = duplicates = distinct = prefixAttempts = tailAttempts = 0;
    prefixStates = tailStates = stockCases = stockRecovered = lastEvent = 0;
    journalByteTears = expectedJournalByteTears = 0;
    memset(sampled, 0, sizeof(sampled)); cutMask = 0;
    memset(tally, 0, sizeof(tally));
    free(seen); seen = NULL; seenCapacity = 0;
    counting = 1;
}

static void Run(void)
{
    if (scenario == PROVISION) Provision();
    if (scenario == CHECKPOINT) assert(WtGame_Checkpoint(&game) == 0);
    if (scenario == PROVISION || scenario == CHECKPOINT) counting = 0;
    assert(Visit(scenario == OFFER || scenario == REJECT_RESTORE ? 1 : -1,
        scenario == CANCEL_RETURN ? WT_OP_CANCEL : 0) == 0);
}

int main(void)
{
    const char *only = getenv("CP_FAULT_SCENARIO");
    volatile unsigned ran = 0, failures = 0;
    full = getenv("CP_FAULT_FULL") != NULL;
    if (setjmp(reboot)) Resume();
    for (scenario = PROVISION; scenario < SCENARIOS; ++scenario) {
        unsigned checkpoints;
        if (only && strcmp(only, names[scenario]) != 0) continue;
        ++ran;
        Setup(); Run(); CheckFinal(1); checkpoints = eventCount; CloseFile();
        Setup(); inject = 1; Run(); inject = 0; CheckFinal(0);
        assert(eventCount == checkpoints && eventCount == saveWrites + journalWrites + syncs);
        assert(saveWrites > 0 || scenario == PROVISION);
        assert(journalWrites > 0 || scenario == CHECKPOINT);
        assert(syncs == saveWrites + journalWrites);
        assert(attempts == distinct + duplicates);
        assert(distinct == tally[RECOVERED] + tally[JOURNAL_BLOCKED] + tally[SAVE_BLOCKED]
            + tally[STUCK] + tally[DUPLICATE] + tally[LOST]);
        assert(prefixStates > 0 && tailStates > 0);
        assert(cutMask == (1u << CUT_KINDS) - 1);
        assert(stockCases > 0 || scenario == PROVISION);
        assert(!full || journalByteTears == expectedJournalByteTears);
        failures += distinct - tally[RECOVERED];
        if (scenario == OFFER || scenario == REJECT_RESTORE) {
            unsigned remove = (1u << WT_INVENTORY_PREPARE_REMOVE) | (1u << WT_INVENTORY_REMOVE)
                | (1u << WT_INVENTORY_VERIFY_REMOVED);
            assert((stepMask & remove) == remove);
        }
        if (scenario == REJECT_RESTORE)
            assert((stepMask & (1u << WT_INVENTORY_PREPARE_RESTORE)) && (stepMask & (1u << WT_INVENTORY_RESTORE)));
        if (scenario == CANCEL_RETURN) {
            unsigned receive = (1u << WT_INVENTORY_PREPARE_RECEIVE) | (1u << WT_INVENTORY_RECEIVE)
                | (1u << WT_INVENTORY_VERIFY_RECEIVED);
            assert((stepMask & receive) == receive);
        }
        printf("%s: checkpoints=%u save-writes=%u journal-writes=%u syncs=%u attempts=%u "
            "identical-states=%u distinct-states=%u prefix-attempts=%u tail-attempts=%u "
            "distinct-prefix=%u distinct-tail=%u stock-save-cases=%u stock-save-recovered=%u journal-byte-tears=%u cut-kinds=%#x\n",
            names[scenario], checkpoints, saveWrites, journalWrites, syncs, attempts, duplicates, distinct,
            prefixAttempts, tailAttempts, prefixStates, tailStates, stockCases, stockRecovered, journalByteTears, cutMask);
        printf("  recovered=%u journal-blocked=%u save-blocked=%u stuck=%u duplicate=%u lost=%u\n",
            tally[RECOVERED], tally[JOURNAL_BLOCKED], tally[SAVE_BLOCKED], tally[STUCK], tally[DUPLICATE], tally[LOST]);
        CloseFile();
    }
    assert(ran != 0);
    assert(only || ran == SCENARIOS);
    free(seen);
    printf("%s recovery: %u unexpected outcomes, %u distinct cuts not recovered\n",
        unexpected ? "FAIL" : "PASS", unexpected, failures);
    return unexpected ? 1 : 0;
}
