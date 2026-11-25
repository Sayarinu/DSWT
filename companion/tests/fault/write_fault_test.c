#define _POSIX_C_SOURCE 200809L
#include "../adapter/save_fixture.h"
/* LUNE-881: FX_GEN5 runs the matrix over Black/White (FX5_GAME 0) and Black
 * 2/White 2 (1) saves, with their journal in the musical distribution save. */
#ifdef FX_GEN5
#include "../adapter/gen5_fixture.h"
#include "core/journal_family.h"
#include "save/journal_save.h"
#endif

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
#ifdef FX_GEN5
#include "retail_load5.h"
#define LOAD(image, file) Fx5Load(image, file)
typedef Fx5File Fixture;
enum { PARTY_BYTES = 0x534, PC_BYTES = 0x530 };
#else
#include "retail_load.h"
#define LOAD(image, file) FxLoad(image, file)
typedef FxFile Fixture;
enum { PARTY_BYTES = CP_ADAPTER_PARTY_SIZE, PC_BYTES = FX_PC_SPAN };
#endif

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
/* LUNE-881: Generation V adds "musical": the journal banks hold a musical's
 * leftovers and the save a marker the game no longer honours; the release
 * clears the marker in both copies, erases the banks, then the journal starts. */
enum Scenario { PROVISION, OFFER, CANCEL_RETURN, REJECT_RESTORE, CHECKPOINT, MUSICAL, SCENARIOS };
static const char *names[] = { "provision", "offer", "cancel-return", "reject-restore", "checkpoint", "musical" };
static const char *steps[] = { "checkpoint/network", "prepare-remove", "remove", "verify-removed",
    "prepare-receive", "receive", "verify-received", "restore", "prepare-restore" };
enum CutKind { BEFORE_WRITE, LOST_WRITE, TORN_PREFIX, TAIL_FIRST, AFTER_WRITE, BEFORE_SYNC, AFTER_SYNC, CUT_KINDS };
static const char *cutNames[] = { "before-write", "lost-write", "torn-prefix", "tail-first", "after-write",
    "before-sync-lost-pending", "after-sync" };
static unsigned sampled[sizeof(steps) / sizeof(*steps)], cutMask;
static int journalWrite;
/* LUNE-880: a HeartGold/SoulSilver PC box write between the first and the
 * last (boxes 1..16), and the sync after it. Their cuts leave the same states
 * as the first and last box's, so CI samples them out; CP_FAULT_FULL keeps them. */
static int interiorBox;
static enum Scenario scenario;
static jmp_buf reboot;
static CpSaveImage image;
static WtGame game;
static CpPosixFile posix;
static CpFile disk, file;
static CpJournalHostStore host;
static WtJournalIO journalIO;
static Fixture fixture;
static unsigned char live[CP_SAVE_FILE_SIZE], durable[CP_SAVE_FILE_SIZE];
static unsigned char digests[4][32];
static unsigned char expectedParty[PARTY_BYTES], expectedPC[PC_BYTES];
/* LUNE-906: LOST_IN_WINDOW is the one loss the games accept (client
 * wonder_trade/delivery.h): a return in flight (RESTORING in place, or
 * APPLYING) that no complete retail copy held when the stock game loaded and
 * saved the card. Resume decides it from the cut itself, before recovering;
 * any other loss is LOST. NOT_APPLICABLE: a released/traded-away run's cut
 * with no return in flight in the loaded save (its base run covers it). */
enum Outcome { RECOVERED, LOST_IN_WINDOW, NOT_APPLICABLE, JOURNAL_BLOCKED, SAVE_BLOCKED, STUCK, DUPLICATE, LOST,
    OUTCOMES };
static const char *outcomes[] = { "recovered", "lost-in-window", "not-applicable", "journal-blocked", "save-blocked",
    "stuck", "duplicate", "lost" };
static unsigned unexpected;
static unsigned randomCounter, eventCount, saveWrites, journalWrites, syncs;
static unsigned attempts, duplicates, distinct, tally[OUTCOMES];
static unsigned prefixAttempts, tailAttempts, prefixStates, tailStates, stockCases, stockRecovered;
static unsigned journalByteTears, expectedJournalByteTears;
static unsigned musicalErases;
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
/* LUNE-880: a resume without stock play that still asks Cut for the next plan. */
static int stockNext;
static char path[sizeof(CP_FAULT_BINARY_DIR) + 32];

/* LUNE-913: OFFER, CANCEL_RETURN and REJECT_RESTORE run again with `held`
 * set: after every cut the stock game moves the offered Pokémon, wherever the
 * save it loaded holds it in the party, to the Day Care (or, HeartGold/
 * SoulSilver, on a Pokéwalker walk) and saves. The companion must refuse
 * whatever step would decide by it, writing nothing; once the player takes it
 * back (one more stock save) everything recovers: never sent or added twice,
 * never lost. heldMoves picks the place in retail_load*.h's table. */
#ifdef FX_GEN5
static const unsigned heldMoves[] = { 0 };
#elif FX_GAME == 2
static const unsigned heldMoves[] = { 0, 3 };
#else
static const unsigned heldMoves[] = { 0 };
#endif
static const char *const heldNames[] = { "daycare", "pokewalker" };
enum { HELD_MOVES = sizeof(heldMoves) / sizeof(heldMoves[0]) };
static unsigned held;
static unsigned char offerCanonical[136], heldParty[PARTY_BYTES];

/* LUNE-906: CANCEL_RETURN and REJECT_RESTORE run again with `letGo` set:
 * after every cut that leaves a return in flight (the journal RESTORING in
 * place or APPLYING) and the offer in the save the stock game loads, the
 * player releases it (1) or trades it away (2) there and saves. The companion
 * must never add it again: the save ends with no copy of it, nothing else
 * lost or doubled, the visit finished. */
static unsigned letGo;
static const char *const letGoNames[] = { "", "released", "traded" };

static const char *Name(void)
{
    static char name[48];
    snprintf(name, sizeof(name), "%s%s%s%s%s", names[scenario], held ? "+" : "", held ? heldNames[held - 1] : "",
        letGo ? "+" : "", letGoNames[letGo]);
    return name;
}

/* Every distinct cut must recover, or lose a return only in the games'
 * window (LOST_IN_WINDOW, decided by rule in Resume); there are no known
 * failures and no allowlist. */
static void CheckOutcome(const char *kind, size_t tear, unsigned stock, unsigned outcome)
{
    if (outcome == RECOVERED || outcome == LOST_IN_WINDOW || outcome == NOT_APPLICABLE) return;
    ++unexpected;
    printf("FAIL unexpected-recovery %s event=%u kind=%s tear=%zu stock-saves=%u outcome=%s\n",
        Name(), eventCount, kind, tear, stock, outcomes[outcome]);
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
    _exit((int)outcome + (stockPlayed || stockNext ? OUTCOMES : 0));
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
    if (!inject || !counting || interiorBox) return;
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
            inject = 0; stockPlayed = 0; stockNext = 0; stockSaves = plan;
            memcpy(live, crashBytes, sizeof(live));
            longjmp(reboot, 1);
        }
        assert(waitpid(child, &status, 0) == child);
        if (!WIFEXITED(status) || WEXITSTATUS(status) >= 2 * OUTCOMES) {
            fprintf(stderr, "FAIL %s event=%u model=%s step=%s offset=%u size=%zu tear=%zu stock-saves=%u status=%d\n",
                Name(), eventCount, model, steps[step], stopOffset, stopSize, tear, plan, status);
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

#ifdef FX_GEN5
static const CpJournalArea *Area(void) { return CpSaveJournal_Area(FX5_LAYOUT); }
#else
static const CpJournalArea *Area(void) { return &CpJournalArea_Gen4; }
#endif

static int JournalOffset(uint32_t offset)
{
    const CpJournalArea *area = Area();
    return (offset >= area->bank[0] && offset < area->bank[0] + area->reserved)
        || (offset >= area->bank[1] && offset < area->bank[1] + area->reserved);
}

static int WriteAt(void *arg, uint32_t offset, const void *bytes, size_t size)
{
    int result;
    (void)arg;
    journalWrite = JournalOffset(offset); /* Also classifies the following sync. */
#ifdef FX_GEN5
    /* A musical's leftovers are erased only once neither copy marks it saved. */
    if (journalWrite && size == 4096 && ((const unsigned char *)bytes)[0] == 0xFF) {
        unsigned copy;
        ++musicalErases;
        for (copy = 0; copy < 2; ++copy)
            assert(Fx5R16(durable + (copy ? FX5_BACKUP : 0) + Fx5Offset(FX5_TRAINER) + 0x3C) == 0);
    }
#endif
#if FX_SHARED
    {
        uint32_t at = offset % CP_SAVE_HALF_SIZE;
        interiorBox = !full && !journalWrite && at >= FX_BOXES + FX_BOX_STRIDE && at < FX_BOXES + 17 * FX_BOX_STRIDE;
    }
#endif
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
    journalIO = CpJournalHostIOArea(&host, &file, Area());
}

static void Provision(void)
{
    unsigned char seed[32];
    WtJournal journal;
    memset(seed, 4, sizeof(seed)); /* Synthetic entropy, not player credentials. */
    assert(CpSaveJournal_ProvisionArea(&file, Area(), &journal, journalIO, seed) == 0);
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
/* LUNE-913: the player's moves with the offered Pokémon, on the RAM the stock
 * game loaded. */
typedef void (*StockAction)(unsigned char *ram);
#ifdef FX_GEN5
enum { MON = FX5_MON };
static unsigned char *RamParty(unsigned char *ram) { return ram + Rl5Offset(FX5_PARTY); }
static unsigned char *RamPlace(unsigned char *ram, unsigned place) { return ram + Rl5Offset(kFx5Held[place].block); }
#else
enum { MON = FX_MON };
static unsigned char *RamParty(unsigned char *ram) { return ram + FX_GENERAL + FX_PARTY; }
#endif

static int IsOffer(const unsigned char *stored)
{
    unsigned char canonical[136];
    return WtPokemon_Decode(stored, canonical) == 0 && memcmp(canonical, offerCanonical, 4) == 0
        && memcmp(canonical + 12, offerCanonical + 12, 4) == 0;
}

/* From the party to the place, as the game moves it (adapter tests'
 * gen4_held.h and gen5_stock_test.c), the party closing up behind it. */
static void HoldOffered(unsigned char *ram)
{
    unsigned char *party = RamParty(ram);
    unsigned count = Fx32(party + 4), slot, i, place = heldMoves[held - 1];
    for (slot = 0; slot < count && !IsOffer(party + 8 + slot * MON); ++slot) {}
    if (slot == count) return;
    memcpy(heldParty, party + 8 + slot * MON, MON);
#ifdef FX_GEN5
    memcpy(RamPlace(ram, place) + kFx5Held[place].mon, heldParty, MON);
    Fx5W32(RamPlace(ram, place) + kFx5Held[place].flag, 1);
#else
    memcpy(ram + FX_GENERAL + kFxHeld[place].mon, heldParty, kFxHeld[place].size);
#endif
    for (i = slot; i + 1 < count; ++i) {
        memcpy(party + 8 + i * MON, party + 8 + (i + 1) * MON, MON);
#if !defined(FX_GEN5) && FX_GAME == 2
        memcpy(ram + FX_GENERAL + FX_APRIJUICE + 5 * i, ram + FX_GENERAL + FX_APRIJUICE + 5 * (i + 1), 5);
#endif
    }
#ifdef FX_GEN5
    Fx5Empty(party + 8 + (count - 1) * MON, MON);
#else
    FxEmpty(party + 8 + (count - 1) * MON, MON);
#if FX_GAME == 2
    memset(ram + FX_GENERAL + FX_APRIJUICE + 5 * (count - 1), 0, 5);
#endif
#endif
    FxPut32(party + 4, count - 1);
}

/* Taken back to the end of the party unchanged (no steps walked). */
static void WithdrawOffered(unsigned char *ram)
{
    unsigned char *party = RamParty(ram);
    unsigned count = Fx32(party + 4), place = heldMoves[held - 1];
    assert(count < 6);
    memcpy(party + 8 + count * MON, heldParty, MON);
    FxPut32(party + 4, count + 1);
#ifdef FX_GEN5
    Fx5W32(RamPlace(ram, place) + kFx5Held[place].flag, 0); /* the image stays */
#else
    if (kFxHeld[place].flag) FxPut16(ram + FX_GENERAL + kFxHeld[place].flag, 0);
    else if (place == 3) memset(ram + FX_GENERAL + kFxHeld[place].mon, 0, FX_BOX_MON); /* the Pokéwalker */
    else FxEmpty(ram + FX_GENERAL + kFxHeld[place].mon, FX_BOX_MON);
#endif
}

/* Whether the save the stock game loads holds the offer in the place. */
static int Holding(const unsigned char *bytes)
{
    unsigned place = heldMoves[held - 1];
#ifdef FX_GEN5
    static unsigned char ram[0x26000];
    RetailLoad5(bytes, ram);
    return Fx5HeldOccupied(ram, place) && IsOffer(RamPlace(ram, place) + kFx5Held[place].mon);
#else
    unsigned selected[2];
    const unsigned char *general;
    RetailLoad(bytes, selected);
    general = bytes + selected[0] * CP_SAVE_HALF_SIZE + FX_GENERAL;
    return FxHeldOccupied(general, place) && IsOffer(general + kFxHeld[place].mon);
#endif
}

/* LUNE-906: Pokémon with the offer's identity in the party and the PC (and
 * the Battle Box) of the save the stock game loads. */
static unsigned OfferLoaded(const unsigned char *bytes)
{
    unsigned copies = 0, count, mon;
#ifdef FX_GEN5
    static unsigned char ram[0x26000];
    const unsigned char *party;
    RetailLoad5(bytes, ram);
    party = ram + Rl5Offset(FX5_PARTY);
    count = Fx32(party + 4);
    for (mon = 0; mon < count + 720 + 6; ++mon)
        copies += IsOffer(mon < count ? party + 8 + mon * MON
            : mon < count + 720 ? ram + Rl5Offset(1 + (mon - count) / 30) + (mon - count) % 30 * FX5_BOX_MON
            : ram + Rl5Offset(FX5_BATTLE_BOX) + (mon - count - 720) * FX5_BOX_MON);
#else
    unsigned selected[2];
    const unsigned char *general, *boxes;
    RetailLoad(bytes, selected);
    general = bytes + selected[0] * CP_SAVE_HALF_SIZE + FX_GENERAL;
    boxes = bytes + selected[1] * CP_SAVE_HALF_SIZE + FX_BOXES;
    count = Fx32(general + FX_PARTY + 4);
    for (mon = 0; mon < count + 540; ++mon)
        copies += IsOffer(mon < count ? general + FX_PARTY + 8 + mon * MON : FxPcSlotConst(boxes, mon - count));
#endif
    return copies;
}

/* The player releases the offer, or trades it away, on the RAM the stock
 * game loaded. */
static void LetGoOffered(unsigned char *ram)
{
#ifdef FX_GEN5
    assert(Fx5Gone(ram, offerCanonical, letGo == 2));
#else
    assert(FxGone(ram + FX_GENERAL, ram + FX_BOXES, offerCanonical, letGo == 2));
#endif
}

#ifdef FX_GEN5
/* Black/White and Black 2/White 2: the stock load builds RAM block by block
 * (retail_load5.h), the player plays, and each stock save writes it to both
 * copies with a new count. */
static void StockGameSave(StockAction action)
{
    static unsigned char ram[0x26000];
    unsigned pass;
    RetailLoad5(live, ram);
    if (action) action(ram);
    for (pass = 0; pass < stockSaves; ++pass) StockSave5(live, ram);
    stockPlayed = 1;
    CloseFile(); OpenBytes(live); Bind();
}
#else
static void StockGameSave(StockAction action)
{
    unsigned selected[2], block, pass;
    uint32_t global, counters[2];
    unsigned char ram[CP_SAVE_HALF_SIZE];
    RetailLoad(live, selected);
    for (block = 0; block < 2; ++block) {
        const unsigned char *body = live + selected[block] * CP_SAVE_HALF_SIZE + blockOffsets[block];
        const unsigned char *footer = body + blockSizes[block] - FX_FOOTER;
        memcpy(ram + blockOffsets[block], body, blockSizes[block]);
        counters[block] = Fx32(footer + (FX_SHARED ? 0 : 4));
    }
    global = Fx32(ram + FX_GENERAL_SIZE - FX_FOOTER);
    ram[0x10] ^= 0x5a; /* The player plays: play time changes. */
    if (action) action(ram);
    for (pass = 0; pass < stockSaves; ++pass) {
        ++global;
        for (block = 0; block < 2; ++block) {
            unsigned char *body, *footer;
            unsigned crc;
            selected[block] ^= 1;
            body = live + selected[block] * CP_SAVE_HALF_SIZE + blockOffsets[block];
            memcpy(body, ram + blockOffsets[block], blockSizes[block]);
            footer = body + blockSizes[block] - FX_FOOTER;
            crc = FxCrc(body, blockSizes[block] - FX_FOOTER);
#if FX_SHARED
            /* HeartGold/SoulSilver: one counter, the CRC last. */
            FxPut32(footer, global); FxPut16(footer + 14, crc);
            (void)counters;
#else
            FxPut32(footer, global); FxPut32(footer + 4, ++counters[block]);
            FxPut16(footer + 18, crc);
#endif
        }
    }
    stockPlayed = 1;
    CloseFile(); OpenBytes(live); Bind();
}
#endif

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
    assert(LOAD(&image, &file) == CP_SAVE_OK);
    Conservation(live, &entry, digests);
#ifdef FX_GEN5
    /* The party, and the first PC tray, where a delivery to a full party goes. */
    party = CpSaveImage_Block(&image, FX5_PARTY);
    pc = CpSaveImage_Block(&image, 1);
#else
    party = CpSaveImage_Block(&image, 0) + FX_PARTY;
    pc = CpSaveImage_Block(&image, 1) + FX_PC;
#endif
    if (capture) {
        memcpy(expectedParty, party, sizeof(expectedParty)); memcpy(expectedPC, pc, sizeof(expectedPC));
    } else if (!stockPlayed) {
        /* Playing can trigger the restore fallback and change party order. */
        assert(memcmp(expectedParty, party, sizeof(expectedParty)) == 0);
        assert(memcmp(expectedPC, pc, sizeof(expectedPC)) == 0);
    }
    assert(entry.phase == (scenario == OFFER ? WT_JOURNAL_QUEUED : WT_JOURNAL_IDLE));
}

/* The offered Pokémon's party slot in the loaded save: 1 unless the stock
 * game moved it (LUNE-913's held runs take it back to the party's end). */
static int OfferSlot(void)
{
#ifdef FX_GEN5
    unsigned char *party = CpSaveImage_Block(&image, FX5_PARTY);
#else
    unsigned char *party = CpSaveImage_Block(&image, 0) + FX_PARTY;
#endif
    unsigned count = Fx32(party + 4), slot;
    for (slot = 0; slot < count && !IsOffer(party + 8 + slot * MON); ++slot) {}
    /* LUNE-906: released, traded away or lost in the window: none to offer. */
    return slot < count ? (int)slot : -1;
}

static void Resume(void)
{
    WtEngine engine;
    WtJournalEntry entry, settled;
    unsigned char after[CP_SAVE_FILE_SIZE];
    CpSaveResult load;
    int slot = -1, inFlight;
    unsigned loaded;
    /* The child inherits the server only. Recreate all companion handles. */
    assert(close(posix.fd) == 0);
    OpenBytes(live);
    Bind(); randomCounter += 1000; /* Fresh entropy after reboot. */
    load = LOAD(&image, &file);
#ifdef FX_GEN5
    /* The companion releases the area at every visit, as the game does. */
    if (scenario == MUSICAL && load == CP_SAVE_OK) assert(CpGen5_ReleaseMusical(&game) == 0);
#endif
    if (CpSaveJournal_DetectArea(&file, Area()) == CP_SAVE_JOURNAL_UNUSED) {
        assert(scenario == PROVISION || scenario == MUSICAL);
        Provision();
    }
    {
        WtJournal journal;
        int result = WtJournal_Open(&journal, journalIO);
        if (result != 0) {
            WtJournalEntry available[2];
            int valid[2], bank;
            unsigned offsets[2] = { Area()->bank[0], Area()->bank[1] };
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
    /* LUNE-906: a return in flight, and whether the save the stock game loads
     * holds the offer yet. */
    inFlight = entry.phase == WT_JOURNAL_APPLYING
        || (entry.phase == WT_JOURNAL_RESTORING && entry.deliveryKind == WT_DELIVERY_RETURN);
    loaded = OfferLoaded(live);
    if (letGo) {
        if (!inFlight || !loaded) Finish(NOT_APPLICABLE);
        stockSaves = 1;
        StockGameSave(LetGoOffered);
        load = LOAD(&image, &file);
        fxOffer = FX_OFFER_GONE;
    } else if (held) {
        /* The stock game loads any cut save, moves the offer and saves. */
        StockGameSave(HoldOffered);
        load = LOAD(&image, &file);
    } else if (load == CP_SAVE_NEEDS_GAME_SAVE) {
        assert(!image.loaded && CpSaveImage_Block(&image, 0) == NULL);
        assert(!WtGame_CanLeave(&game, &entry));
        StockGameSave(NULL);
        load = LOAD(&image, &file);
    }
#if FX_SHARED || defined(FX_GEN5)
    /* LUNE-880: HeartGold/SoulSilver load every cut save (their LoadCheck falls
     * back to the older half), so the player may also play and save in the
     * stock game before the companion resumes: the first plan resumes
     * directly, the second after one stock save. */
    else if (load == CP_SAVE_OK && scenario != PROVISION) {
        if (stockSaves == 1) {
            /* CI plays the stock game after every fourth distinct cut. */
            stockNext = full || distinct % 4 == 1;
        } else {
            stockSaves = 1;
            StockGameSave(NULL);
            load = LOAD(&image, &file);
        }
    }
#endif
    /* The games' window: never put back once the stock game saved over it. */
    if (stockPlayed && inFlight && !loaded && !letGo) fxOffer = FX_OFFER_WINDOW;
    if (load != CP_SAVE_OK) Finish(SAVE_BLOCKED);
    OpenEngine(&engine);
    /* This is the local boot path used by WtBoot_Check, without DS glue/TLS. */
    {
        static unsigned char opened[CP_SAVE_FILE_SIZE];
        int result, ownership;
        memcpy(opened, live, sizeof(opened));
        result = WtEngine_Settle(&engine);
        ownership = Ownership(live, &engine.entry, digests, stockPlayed);
        if (ownership > 0) Finish(DUPLICATE);
        if (ownership < 0) Finish(LOST);
        if (held && Holding(live)) {
            /* LUNE-913: a refusal while the offer is held there writes
             * nothing; the player takes it back and the companion settles. */
            if ((result != 0 || engine.entry.phase == WT_JOURNAL_ESCROW || engine.entry.phase == WT_JOURNAL_RESTORING
                    || engine.entry.phase == WT_JOURNAL_APPLYING) && memcmp(opened, live, sizeof(opened)) != 0)
                Finish(STUCK);
            WtEngine_Close(&engine);
            stockSaves = 1;
            StockGameSave(WithdrawOffered);
            assert(!Holding(live));
            if (LOAD(&image, &file) != CP_SAVE_OK) Finish(SAVE_BLOCKED);
            OpenEngine(&engine);
            result = WtEngine_Settle(&engine);
            ownership = Ownership(live, &engine.entry, digests, stockPlayed);
            if (ownership > 0) Finish(DUPLICATE);
            if (ownership < 0) Finish(LOST);
        }
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
        && engine.entry.phase <= WT_JOURNAL_IDLE && server.eventCount <= 1) slot = OfferSlot();
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
    if (fxOffer == FX_OFFER_GONE) assert(OfferLoaded(live) == 0);
    Finish(fxOffer == FX_OFFER_WINDOW && !OfferLoaded(live) ? LOST_IN_WINDOW : RECOVERED);
}

static void Setup(void)
{
#ifdef FX_GEN5
    Fx5Spec specs[4];
#else
    FxSpec specs[4];
#endif
    unsigned i;
    inject = counting = 0; step = 0; randomCounter = 0;
    memset(&server, 0, sizeof(server)); replayCount = 0;
    memset(replay, 0, sizeof(replay));
    for (i = 0; i < 4; ++i) {
        unsigned char canonical[136];
#ifdef FX_GEN5
        specs[i] = Fx5Default(i); Fx5Canonical(&specs[i], canonical);
#else
        specs[i] = FxDefault(i); FxCanonical(&specs[i], canonical);
#endif
        FxSha(canonical, sizeof(canonical), digests[i]);
        if (i == 1) memcpy(offerCanonical, canonical, sizeof(canonical));
    }
#ifdef FX_GEN5
    Fx5Build(&fixture, specs, 4, 40);
    /* The last stock save was cut in its backup pass: the backup's copy of
     * the last PC tray is the one before, which a checkpoint rewrites. */
    {
        unsigned char *tray = Fx5Block(&fixture, 1, 24);
        tray[0x7F0] ^= 0x33;
        Fx5W16(tray + Fx5Size_(24) + 2, Fx5Crc(tray, Fx5Size_(24)));
        Fx5W16(tray + Fx5Size_(24), Fx5R16(tray + Fx5Size_(24)) - 1);
    }
#else
    FxBuild(&fixture, specs, 4);
#endif
#ifdef FX_GEN5
    if (scenario == MUSICAL) {
        /* A marked musical whose save count no longer matches: the game's
         * load drops it, and the banks hold what it left. */
        unsigned char *trainer = Fx5Block(&fixture, 0, FX5_TRAINER);
        Fx5W16(trainer + 0x3C, 0xC21E);
        Fx5W32(trainer + FX5_COUNTS + 24, 77);
        Fx5Block(&fixture, 0, FX5_MUSICAL_SAVE)[0x29E] = 1;
        Fx5Seal(&fixture, 0, 40);
        memcpy(fixture.bytes + FX5_BACKUP, fixture.bytes, FX5_USED);
        for (i = 0; i < 0x2000; ++i) fixture.bytes[FX5_JOURNAL + i] = (unsigned char)(i * 3 + 7);
    }
#endif
    OpenBytes(fixture.bytes); Bind();
    assert(LOAD(&image, &file) == CP_SAVE_OK);
    if (scenario != PROVISION && scenario != MUSICAL) Provision();
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
#ifdef FX_GEN5
    if (scenario == MUSICAL) {
        assert(CpGen5_ReleaseMusical(&game) == 0);
        Provision();
    }
#endif
    if (scenario == PROVISION) Provision();
    if (scenario == CHECKPOINT) assert(WtGame_Checkpoint(&game) == 0);
    if (scenario == PROVISION || scenario == CHECKPOINT) counting = 0;
    assert(Visit(scenario == OFFER || scenario == REJECT_RESTORE ? 1 : -1,
        scenario == CANCEL_RETURN ? WT_OP_CANCEL : 0) == 0);
}

int main(void)
{
    const char *only = getenv("CP_FAULT_SCENARIO");
    volatile unsigned ran = 0, failures = 0, windowLosses = 0;
    full = getenv("CP_FAULT_FULL") != NULL;
#ifdef FX_GEN5
    CpCore_SelectFamily(CP_CORE_GEN5);
#endif
    if (setjmp(reboot)) Resume();
    for (scenario = PROVISION; scenario < SCENARIOS; ++scenario) for (held = 0; held <= HELD_MOVES; ++held)
    for (letGo = 0; letGo <= 2; ++letGo) {
        unsigned checkpoints;
        if (held && scenario != OFFER && scenario != CANCEL_RETURN && scenario != REJECT_RESTORE) continue;
        if (letGo && (held || (scenario != CANCEL_RETURN && scenario != REJECT_RESTORE))) continue;
        if (only && strcmp(only, Name()) != 0) continue;
#ifndef FX_GEN5
        if (scenario == MUSICAL) continue;
#endif
        ran += !held && !letGo;
        Setup(); Run(); CheckFinal(1); checkpoints = eventCount; CloseFile();
        Setup(); inject = 1; Run(); inject = 0; CheckFinal(0);
        assert(eventCount == checkpoints && eventCount == saveWrites + journalWrites + syncs);
        assert(saveWrites > 0 || scenario == PROVISION);
        assert(journalWrites > 0 || scenario == CHECKPOINT);
        assert(syncs == saveWrites + journalWrites);
        assert(attempts == distinct + duplicates);
        assert(distinct == tally[RECOVERED] + tally[LOST_IN_WINDOW] + tally[NOT_APPLICABLE] + tally[JOURNAL_BLOCKED]
            + tally[SAVE_BLOCKED] + tally[STUCK] + tally[DUPLICATE] + tally[LOST]);
        /* A released/traded-away run must reach the stock game with a return in flight. */
        assert(!letGo || tally[RECOVERED] > 0);
        assert(prefixStates > 0 && tailStates > 0);
        assert(cutMask == (1u << CUT_KINDS) - 1);
        assert(stockCases > 0 || scenario == PROVISION || scenario == MUSICAL);
        assert(scenario != MUSICAL || musicalErases > 0);
        assert(!full || journalByteTears == expectedJournalByteTears);
        failures += distinct - tally[RECOVERED] - tally[LOST_IN_WINDOW] - tally[NOT_APPLICABLE];
        windowLosses += tally[LOST_IN_WINDOW];
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
            Name(), checkpoints, saveWrites, journalWrites, syncs, attempts, duplicates, distinct,
            prefixAttempts, tailAttempts, prefixStates, tailStates, stockCases, stockRecovered, journalByteTears, cutMask);
        printf("  recovered=%u lost-in-window=%u not-applicable=%u journal-blocked=%u save-blocked=%u stuck=%u "
            "duplicate=%u lost=%u\n", tally[RECOVERED], tally[LOST_IN_WINDOW], tally[NOT_APPLICABLE],
            tally[JOURNAL_BLOCKED], tally[SAVE_BLOCKED], tally[STUCK], tally[DUPLICATE], tally[LOST]);
        CloseFile();
    }
    assert(ran != 0 || only);
#ifdef FX_GEN5
    assert(only || ran == SCENARIOS);
#else
    assert(only || ran == SCENARIOS - 1);
#endif
    free(seen);
    printf("%s recovery: %u unexpected outcomes, %u distinct cuts not recovered, %u returns lost in the games' window\n",
        unexpected ? "FAIL" : "PASS", unexpected, failures, windowLosses);
    return unexpected ? 1 : 0;
}
