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
 * ctest runs all after-write/after-sync cuts. CP_FAULT_FULL=1 adds before,
 * half-prefix torn writes, and loss of unsynced bytes (run the binary directly
 * for the full matrix, beyond ctest's 120-second CI budget).
 * CP_FAULT_SCENARIO selects one of names[] for a focused reproduction.
 * CP_FAULT_REQUIRE_RESUME=1 makes the known production recovery fences fail;
 * default safety coverage reports them, without inventing a writable image. */
enum Scenario { PROVISION, OFFER, CANCEL_RETURN, REJECT_RESTORE, CHECKPOINT, SCENARIOS };
static const char *names[] = { "provision", "offer", "cancel-return", "reject-restore", "checkpoint" };
static const char *steps[] = { "checkpoint/network", "prepare-remove", "remove", "verify-removed",
    "prepare-receive", "receive", "verify-received", "restore", "prepare-restore" };
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
static unsigned randomCounter, eventCount, saveWrites, journalWrites, syncs, resumed, fenced, journalFenced;
static unsigned step, stopStep, stopEvent, stepMask;
static uint32_t stopOffset;
static size_t stopSize;
static const char *stopModel;
static int inject, counting, full, strict;
static char path[64];

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
    strcpy(path, "/tmp/dswt-fault-XXXXXX");
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

static void Cut(const char *model, int losePending, uint32_t offset, const void *data, size_t prefix)
{
    pid_t child;
    int status;
    if (!inject || !counting) return;
    fflush(NULL);
    child = fork();
    assert(child >= 0);
    if (child == 0) {
        inject = 0; stopModel = model; stopEvent = eventCount; stopStep = step;
        if (losePending) memcpy(live, durable, sizeof(live));
        if (prefix) memcpy(live + offset, data, prefix);
        longjmp(reboot, 1);
    }
    assert(waitpid(child, &status, 0) == child);
    if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0 && WEXITSTATUS(status) != 10 && WEXITSTATUS(status) != 11)) {
        fprintf(stderr, "FAIL %s event=%u model=%s step=%s offset=%u size=%zu status=%d\n",
            names[scenario], eventCount, model, steps[step], stopOffset, stopSize, status);
        CloseFile();
        exit(1);
    }
    if (WEXITSTATUS(status) == 10) ++fenced;
    else if (WEXITSTATUS(status) == 11) ++journalFenced;
    else ++resumed;
}

static int ReadAt(void *arg, uint32_t offset, void *bytes, size_t size)
{
    (void)arg;
    return disk.readAt(disk.context, offset, bytes, size);
}

static int WriteAt(void *arg, uint32_t offset, const void *bytes, size_t size)
{
    int result;
    (void)arg;
    if (counting) {
        ++eventCount; stopOffset = offset; stopSize = size;
        if ((offset >= CP_JOURNAL_BANK_0 && offset < CP_JOURNAL_BANK_0 + CP_JOURNAL_RESERVED_SIZE)
            || (offset >= CP_JOURNAL_BANK_1 && offset < CP_JOURNAL_BANK_1 + CP_JOURNAL_RESERVED_SIZE)) ++journalWrites;
        else ++saveWrites;
    }
    if (full) {
        Cut("before-write", 0, 0, NULL, 0);
        Cut("torn-write", 0, offset, bytes, size / 2);
        Cut("lost-write", 1, 0, NULL, 0);
    }
    result = disk.writeAt(disk.context, offset, bytes, size);
    assert(result == 0);
    memcpy(live + offset, bytes, size);
    Cut("after-write", 0, 0, NULL, 0);
    return result;
}

static int Sync(void *arg)
{
    (void)arg;
    if (counting) { ++eventCount; ++syncs; stopOffset = 0; stopSize = 0; }
    if (full) Cut("before-sync-lost-pending", 1, 0, NULL, 0);
    assert(disk.sync(disk.context) == 0);
    memcpy(durable, live, sizeof(durable));
    Cut("after-sync", 0, 0, NULL, 0);
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

static void CheckCursorFence(const WtJournalEntry *complete, unsigned partialBank)
{
    unsigned char record[WT_JOURNAL_RECORD_SIZE];
    const unsigned offsets[2] = { CP_JOURNAL_BANK_0, CP_JOURNAL_BANK_1 };
    WtJournalEntry candidate;
    /* Only the existing first-reconcile cursor fence is tolerated. Rebuild a
     * candidate solely to identify this case; never publish or resume it. */
    memcpy(record, live + offsets[partialBank], sizeof(record));
    assert(Hash(record, 4060, record + 4060) == 0);
    memcpy(record + 4092, "CMIT", 4);
    assert(WtJournal_Decode(record, Hash, &candidate) == 0);
    assert(complete->phase == WT_JOURNAL_IDLE && complete->nextCounter == 1 && complete->eventCursor == 0);
    assert(candidate.generation == complete->generation + 1 && candidate.eventCursor == 1);
    candidate.generation = complete->generation; candidate.eventCursor = 0;
    assert(memcmp(&candidate, complete, sizeof(candidate)) == 0);
}

static void Visit(int slot, unsigned action)
{
    WtEngine engine;
    CpVisit visit;
    OpenEngine(&engine);
    assert(CpVisit_Run(&engine, &game, 7, slot, action, &transport, &visit) == 0);
    assert(visit.canLeave);
    if (scenario == OFFER) assert(visit.phase == WT_JOURNAL_QUEUED);
    else assert(visit.phase == WT_JOURNAL_IDLE);
    WtEngine_Close(&engine);
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
    } else {
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
            CheckCursorFence(&available[bank], (unsigned)(bank ^ 1));
            if (strict) fprintf(stderr, "JOURNAL FENCED %s event=%u model=%s step=%s offset=%u size=%zu error=%d\n",
                names[scenario], stopEvent, stopModel, steps[stopStep], stopOffset, stopSize, result);
            CloseFile(); _exit(strict ? 1 : 11);
        }
        assert(WtJournal_Read(&journal, &entry) == 0);
        WtJournal_Close(&journal);
    }
    Conservation(live, &entry, digests);
    if (load == CP_SAVE_NEEDS_GAME_SAVE) {
        /* The companion deliberately exposes no writable image in this case.
         * The retail oracle above still proves conservation in the selected
         * blocks. Never fabricate a loaded image to make recovery pass. */
        assert(!image.loaded && CpSaveImage_Block(&image, 0) == NULL);
        assert(!WtGame_CanLeave(&game, &entry));
        if (strict) {
            fprintf(stderr, "RELOAD FENCED %s event=%u model=%s step=%s offset=%u size=%zu\n",
                names[scenario], stopEvent, stopModel, steps[stopStep], stopOffset, stopSize);
            CloseFile(); _exit(1);
        }
        CloseFile(); _exit(10);
    }
    assert(load == CP_SAVE_OK);
    OpenEngine(&engine);
    /* This is the local boot path used by WtBoot_Check, without DS glue/TLS. */
    assert(WtEngine_Settle(&engine) == 0);
    settled = engine.entry; memcpy(after, live, sizeof(after));
    Conservation(live, &settled, digests);
    WtEngine_Close(&engine);
    OpenEngine(&engine);
    assert(WtEngine_Settle(&engine) == 0);
    assert(memcmp(&settled, &engine.entry, sizeof(settled)) == 0);
    assert(memcmp(after, live, sizeof(after)) == 0);
    Conservation(live, &engine.entry, digests);
    if ((scenario == OFFER || scenario == REJECT_RESTORE)
        && engine.entry.phase <= WT_JOURNAL_IDLE && server.eventCount <= 1) slot = 1;
    WtEngine_Close(&engine);
    if (scenario == CHECKPOINT) assert(WtGame_Checkpoint(&game) == 0);
    Visit(slot, scenario == CANCEL_RETURN ? WT_OP_CANCEL : 0);
    CheckFinal(0);
    CloseFile(); _exit(0);
}

static void Setup(void)
{
    FxSpec specs[4];
    unsigned i;
    inject = counting = 0; step = 0; randomCounter = 0;
    memset(&server, 0, sizeof(server)); replayCount = 0;
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
    eventCount = saveWrites = journalWrites = syncs = resumed = fenced = journalFenced = stepMask = 0;
    counting = 1;
}

static void Run(void)
{
    if (scenario == PROVISION) Provision();
    if (scenario == CHECKPOINT) assert(WtGame_Checkpoint(&game) == 0);
    if (scenario == PROVISION || scenario == CHECKPOINT) counting = 0;
    Visit(scenario == OFFER || scenario == REJECT_RESTORE ? 1 : -1,
        scenario == CANCEL_RETURN ? WT_OP_CANCEL : 0);
}

int main(void)
{
    const char *only = getenv("CP_FAULT_SCENARIO");
    unsigned ran = 0;
    full = getenv("CP_FAULT_FULL") != NULL;
    strict = getenv("CP_FAULT_REQUIRE_RESUME") != NULL;
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
        assert(resumed + fenced + journalFenced == (full ? 3 * checkpoints : checkpoints));
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
        printf("%s: checkpoints=%u save-writes=%u journal-writes=%u syncs=%u resumed=%u save-fenced=%u journal-fenced=%u\n",
            names[scenario], checkpoints, saveWrites, journalWrites, syncs, resumed, fenced, journalFenced);
        CloseFile();
    }
    assert(ran != 0);
    puts("PASS safety matrix: retail selection and exact ownership at every cut; settle twice and final party/PC for resumed cuts");
    puts("LIMIT: save-fenced/journal-fenced cuts retain ownership but cannot resume with current production recovery; CP_FAULT_REQUIRE_RESUME=1 fails on these");
    return 0;
}
