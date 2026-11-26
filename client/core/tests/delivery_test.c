/* The adapters' delivery decisions (wonder_trade/delivery.h) driven through the
 * real engine and journal, over a synthetic save with two retail copies on a
 * simulated flash. Built once per game family, so Generation V's Battle Box
 * is scanned and recorded. The unmodified game is simulated too: after a power
 * cut it loads the newest copy, may change or move the Pokémon (friendship,
 * the Day Care, the Battle Box) and saves over the other copy.
 *
 * The game adapters run this same code: each one supplies the WtDeliveryOps
 * this file's Ops supplies, over its own save. Synthetic data only. */
#include "wonder_trade/delivery.h"
#include "wonder_trade/wire_layout.h"
#include "journal_fixture.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <psa/crypto.h>

#define CHECK(test) do { if (!(test)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #test); exit(1); } } while (0)

enum { PARTY = WT_GAME_PARTY_SIZE, BOX = WT_POKEMON_SIZE, BATTLE = WT_GAME_BATTLE_BOX_SLOTS ? WT_GAME_BATTLE_BOX_SLOTS : 1 };
/* The Battle Box's slots, 0 where the game has none. */
static unsigned kBattleBox = WT_GAME_BATTLE_BOX_SLOTS;

/* Everything the save holds; the counter is the retail footer, outside the digest. */
typedef struct Save {
    uint32_t count;
    unsigned char party[6][236], boxes[WT_GAME_BOX_COUNT][WT_GAME_BOX_SLOTS][BOX], battle[BATTLE][BOX];
    /* Where no locator reaches: the Day Care. */
    unsigned char daycare[BOX];
    uint32_t playTime;
    uint32_t counter;
} Save;

typedef struct Bank {
    Save save;
    int valid;
} Bank;

static Bank flash[2];
static Save live, scratch;
static int scratchUsed;
static unsigned char journal[2][WT_JOURNAL_RECORD_SIZE];

/* Power cuts: every write to a retail copy or a journal bank is an event. A cut
 * at event `cutAt` skips that write (mode 0), finishes it (mode 1) or leaves a
 * retail copy torn (mode 2), and the console stops. */
static unsigned events, cutAt, cutMode;
static jmp_buf power;
static unsigned saveWrites, journalWrites;

static int Sha(const unsigned char *bytes, size_t size, unsigned char digest[32])
{
    size_t length = 0;
    return psa_hash_compute(PSA_ALG_SHA_256, bytes, size, digest, 32, &length) == PSA_SUCCESS && length == 32 ? 0 : -1;
}

/* An event; sets torn when the write must leave its copy torn. */
static void Event(int *torn)
{
    *torn = 0;
    if (++events != cutAt) return;
    if (cutMode == 0) longjmp(power, 1);
    *torn = cutMode == 2;
}

static void AfterEvent(void)
{
    if (cutAt && events == cutAt) longjmp(power, 1);
}

static int JournalRead(void *arg, int bank, unsigned char record[WT_JOURNAL_RECORD_SIZE])
{
    (void)arg;
    memcpy(record, journal[bank], WT_JOURNAL_RECORD_SIZE);
    return 0;
}

static int JournalWrite(void *arg, int bank, const unsigned char record[WT_JOURNAL_RECORD_SIZE])
{
    int torn;
    (void)arg;
    Event(&torn);
    /* The flash order (wonder_trade/flash.c): the tag cleared, the body, the tag. */
    if (torn) { memset(journal[bank] + WT_JOURNAL_RECORD_SIZE - 4, 0, 4); memcpy(journal[bank], record, 2048); }
    else memcpy(journal[bank], record, WT_JOURNAL_RECORD_SIZE);
    ++journalWrites;
    AfterEvent();
    return 0;
}

/* ---- The save, as an adapter sees it ---- */

static int Digest(const Save *save, unsigned char digest[32])
{
    return Sha((const unsigned char *)save, offsetof(Save, counter), digest);
}

/* The newest valid copy, as every game loads it. */
static int Load(Save *save)
{
    int bank = flash[0].valid && (!flash[1].valid || flash[0].save.counter - flash[1].save.counter < 0x80000000u) ? 0 : 1;
    if (!flash[bank].valid) return -1;
    *save = flash[bank].save;
    return bank;
}

/* A game's save: the counter advances and the older or torn copy is written. */
static void Write(Save *save)
{
    int newest = flash[0].valid && (!flash[1].valid || flash[0].save.counter - flash[1].save.counter < 0x80000000u) ? 0 : 1;
    int target = flash[newest].valid ? !newest : newest, torn;
    ++save->counter;
    Event(&torn);
    flash[target].save = *save; flash[target].valid = !torn;
    ++saveWrites;
    AfterEvent();
}

static int Verify(void *arg, const unsigned char expected[32])
{
    unsigned char digest[32];
    int bank;
    (void)arg;
    for (bank = 0; bank < 2; ++bank) {
        if (!flash[bank].valid || Digest(&flash[bank].save, digest) != 0 || memcmp(digest, expected, 32) != 0) return -1;
    }
    return 0;
}

/* SaveBoth: two full saves unless both copies already hold it. */
static int SaveBoth(const unsigned char expected[32])
{
    unsigned char digest[32];
    if (Digest(&live, digest) != 0 || memcmp(digest, expected, 32) != 0) return -1;
    if (Verify(NULL, expected) == 0) return 0;
    Write(&live); Write(&live);
    return Verify(NULL, expected);
}

static int Checkpoint(void *arg)
{
    unsigned char digest[32];
    (void)arg;
    return Digest(&live, digest) == 0 ? SaveBoth(digest) : -1;
}

static int PartyCount(void *arg, void *save)
{
    (void)arg;
    return (int)((Save *)save)->count;
}

static unsigned char *Slot(void *arg, void *data, WtJournalLocator where)
{
    Save *save = data;
    (void)arg;
    if (where.kind == 1 && where.box == 0 && where.slot < 6 && where.length == PARTY) return save->party[where.slot];
    if (where.kind == 2 && where.box < WT_GAME_BOX_COUNT && where.slot < WT_GAME_BOX_SLOTS && where.length == BOX)
        return save->boxes[where.box][where.slot];
    if (kBattleBox && where.kind == 3 && where.box == 0 && where.slot < kBattleBox
        && where.length == BOX) return save->battle[where.slot];
    return NULL;
}

static int Occupied(void *arg, void *save, WtJournalLocator where)
{
    unsigned char canonical[BOX];
    const unsigned char *stored = Slot(arg, save, where);
    if (!stored || WtPokemon_Decode(stored, canonical) != 0) return -1;
    return canonical[8] || canonical[9];
}

static int Hash(void *arg, void *save, unsigned char digest[32])
{
    (void)arg;
    return Digest(save, digest);
}

static void *Copy(void *arg)
{
    (void)arg;
    CHECK(!scratchUsed);
    scratchUsed = 1; scratch = live;
    return &scratch;
}

static void Discard(void *arg, void *copy)
{
    (void)arg;
    CHECK(copy == &scratch && scratchUsed);
    memset(&scratch, 0, sizeof(scratch)); scratchUsed = 0;
}

static int Commit(void *arg, void *copy, const unsigned char digest[32])
{
    (void)arg;
    live = *(Save *)copy;
    return SaveBoth(digest);
}

static void Counters(void *arg, uint32_t counters[3])
{
    (void)arg;
    counters[0] = counters[1] = counters[2] = live.counter;
}

static void Empty(void *arg, unsigned char *image, unsigned length)
{
    unsigned char canonical[BOX];
    (void)arg;
    memset(canonical, 0, sizeof(canonical)); memset(image, 0, length);
    CHECK(WtPokemon_Encode(canonical, image) == 0);
}

static void Checksum(unsigned char canonical[BOX])
{
    unsigned sum = 0, i;
    for (i = 8; i < BOX; i += 2) sum += canonical[i] | ((unsigned)canonical[i + 1] << 8);
    canonical[6] = (unsigned char)sum; canonical[7] = (unsigned char)(sum >> 8);
}

/* A party image: the encrypted box image, then battle stats. */
static void PartyImage(const unsigned char canonical[BOX], unsigned char image[236])
{
    memset(image, 0, 236);
    CHECK(WtPokemon_Encode(canonical, image) == 0);
    memset(image + BOX, canonical[0x14] | 1, PARTY - BOX);
}

/* The game's receive: a matched Pokémon's friendship resets, as the adapters'
 * receive effects do; a returned offer is its original image. */
static int Build(void *arg, void *data, WtJournalEntry *entry, int prepare)
{
    Save *save = data;
    unsigned char built[236], canonical[BOX], *destination;
    unsigned box, slot;
    int found = 0;
    if (prepare) {
        memset(&entry->destination, 0, sizeof(entry->destination));
        if (save->count < 6) {
            entry->destination.kind = 1; entry->destination.slot = (unsigned char)save->count;
            entry->destination.length = PARTY;
        } else {
            for (box = 0; box < WT_GAME_BOX_COUNT && !found; ++box) for (slot = 0; slot < WT_GAME_BOX_SLOTS; ++slot) {
                WtJournalLocator where = { 2, (unsigned char)box, (unsigned char)slot, BOX };
                if (Occupied(arg, save, where) != 0) continue;
                found = 1; break;
            }
            if (!found) return 1;
            --box;
            entry->destination.kind = 2; entry->destination.box = (unsigned char)box;
            entry->destination.slot = (unsigned char)slot; entry->destination.length = BOX;
        }
        destination = Slot(arg, save, entry->destination);
        if (Digest(save, entry->inventoryBefore) != 0) return -1;
        memset(entry->destinationBefore, 0, 236); memcpy(entry->destinationBefore, destination, entry->destination.length);
    }
    destination = Slot(arg, save, entry->destination);
    if (!destination || memcmp(destination, entry->destinationBefore, entry->destination.length) != 0
        || (entry->destination.kind == 1 && save->count != entry->destination.slot)) return -1;
    if (entry->deliveryKind == WT_DELIVERY_RETURN) {
        memcpy(built, entry->original, 236);
    } else {
        memcpy(canonical, entry->incoming, BOX); canonical[0x14] = 70; Checksum(canonical);
        PartyImage(canonical, built);
    }
    if (WtPokemon_Decode(built, canonical) != 0) return -1;
    if (prepare) {
        memset(entry->destinationAfter, 0, 236); memcpy(entry->destinationAfter, built, entry->destination.length);
        if (Sha(canonical, BOX, entry->appliedDigest) != 0) return -1;
    } else if (memcmp(entry->destinationAfter, built, entry->destination.length) != 0) return -1;
    memcpy(destination, built, entry->destination.length);
    if (entry->destination.kind == 1) ++save->count;
    return 0;
}

/* LUNE-902: the DS Companion's removal moves the counter 2^28 ahead
 * (wonder_trade/engine.h WT_ABANDON_STEP, a companion-only constant). */
static const uint32_t kStep = UINT32_C(0x10000000);

static const WtDeliveryOps sOps = {
    NULL, &live, Sha, PartyCount, Slot, Occupied, Hash, Verify, Checkpoint, Copy, Discard, Commit, Counters, Build, Empty
};

/* The adapters' party steps (platinum/game_save.c RESTORE), over the same save. */
static void RemoveSlot(Save *save, unsigned slot)
{
    unsigned i;
    for (i = slot; i + 1 < save->count; ++i) memcpy(save->party[i], save->party[i + 1], 236);
    memset(save->party[save->count - 1], 0, 236); Empty(NULL, save->party[save->count - 1], PARTY);
    --save->count;
}

static void InsertSlot(Save *save, unsigned slot, const unsigned char image[236])
{
    unsigned i;
    for (i = save->count; i > slot; --i) memcpy(save->party[i], save->party[i - 1], 236);
    memcpy(save->party[slot], image, 236); ++save->count;
}

/* The adapters' party steps PREPARE_REMOVE and REMOVE (game_save.c, the same
 * in all five games): REMOVE runs only on a save that is exactly the recorded
 * inventoryBefore (it removes the offer) or inventoryAfter (removed already);
 * anything else fails before any write. */
static int GameRemove(enum WtInventoryStep step, WtJournalEntry *entry, unsigned slot, int jump)
{
    unsigned char digest[32], canonical[BOX];
    if (Digest(&live, digest) != 0) return -1;
    scratch = live;
    if (step == WT_INVENTORY_PREPARE_REMOVE) {
        if (slot >= scratch.count || WtPokemon_Decode(scratch.party[slot], canonical) != 0) return -1;
        entry->source.kind = 1; entry->source.box = 0; entry->source.slot = (unsigned char)slot; entry->source.length = PARTY;
        memset(entry->original, 0, sizeof(entry->original)); memcpy(entry->original, scratch.party[slot], PARTY);
        memcpy(entry->outgoing, canonical, BOX);
        if (Sha(entry->original, PARTY, entry->originalDigest) != 0) return -1;
        memcpy(entry->inventoryBefore, digest, 32);
        RemoveSlot(&scratch, slot);
        if (WtDelivery_Unique(&sOps, &scratch, entry->outgoing) != 0 || Digest(&scratch, entry->inventoryAfter) != 0) return -1;
        WtDelivery_Store(&sOps, entry);
        return 0;
    }
    if (memcmp(digest, entry->inventoryBefore, 32) == 0) {
        if (slot >= scratch.count || memcmp(scratch.party[slot], entry->original, PARTY) != 0) return -1;
        RemoveSlot(&scratch, slot);
        if (Digest(&scratch, digest) != 0) return -1;
    }
    if (memcmp(digest, entry->inventoryAfter, 32) != 0) return -1;
    live = scratch;
    /* LUNE-902: the companion's removal moves the counter far ahead. */
    if (jump && Verify(NULL, digest) != 0 && live.counter - entry->globalCounter < kStep)
        live.counter = entry->globalCounter + kStep - 1;
    if (SaveBoth(digest) != 0) return -1;
    WtDelivery_Store(&sOps, entry);
    return 0;
}

static int Inventory(void *arg, enum WtInventoryStep step, WtJournalEntry *entry, unsigned slot)
{
    unsigned char digest[32];
    (void)arg;
    if (step == WT_INVENTORY_PREPARE_REMOVE || step == WT_INVENTORY_REMOVE) return GameRemove(step, entry, slot, 0);
    if (step != WT_INVENTORY_RESTORE) return WtDelivery_Step(&sOps, step, entry);
    if (!WtDelivery_Bound(entry) || Digest(&live, digest) != 0) return -1;
    scratch = live;
    if (memcmp(digest, entry->inventoryAfter, 32) == 0) {
        if (scratch.count >= 6 || slot > scratch.count) return -1;
        InsertSlot(&scratch, slot, entry->original);
        if (Digest(&scratch, digest) != 0) return -1;
    }
    if (memcmp(digest, entry->inventoryBefore, 32) != 0) return -1;
    live = scratch;
    if (SaveBoth(digest) != 0) return -1;
    WtDelivery_Store(&sOps, entry);
    return 0;
}

static int CanHold(void *arg, const unsigned char pokemon[WT_POKEMON_SIZE])
{
    (void)arg; (void)pokemon;
    return 1;
}

static unsigned randomCounter;

static int Random(void *arg, unsigned char *bytes, size_t size)
{
    unsigned char input[4], digest[32];
    (void)arg;
    ++randomCounter; memcpy(input, &randomCounter, 4);
    CHECK(size <= 32 && Sha(input, 4, digest) == 0);
    memcpy(bytes, digest, size);
    return 0;
}

static int NoRandom(void *arg, unsigned char *bytes, size_t size)
{
    (void)arg; (void)bytes; (void)size;
    return -1;
}

/* ---- Pokémon and the scene ---- */

static void Mon(unsigned char canonical[BOX], uint32_t pid, uint32_t ot, unsigned species, unsigned friendship)
{
    memset(canonical, 0, BOX);
    memcpy(canonical, &pid, 4); memcpy(canonical + 12, &ot, 4);
    canonical[8] = (unsigned char)species; canonical[9] = (unsigned char)(species >> 8);
    canonical[0x14] = (unsigned char)friendship; canonical[0x40] = 8;
    Checksum(canonical);
}

static unsigned char offered[BOX], matched[BOX];

/* Where the save holds Pokémon with this identity, the Day Care included. */
static unsigned Copies(const Save *save, const unsigned char target[BOX])
{
    unsigned char canonical[BOX];
    const unsigned char *images[6 + WT_GAME_BOX_COUNT * WT_GAME_BOX_SLOTS + BATTLE + 1];
    unsigned n = 0, i, copies = 0;
    for (i = 0; i < save->count; ++i) images[n++] = save->party[i];
    for (i = 0; i < WT_GAME_BOX_COUNT * WT_GAME_BOX_SLOTS; ++i) images[n++] = save->boxes[i / WT_GAME_BOX_SLOTS][i % WT_GAME_BOX_SLOTS];
    for (i = 0; i < kBattleBox; ++i) images[n++] = save->battle[i];
    images[n++] = save->daycare;
    for (i = 0; i < n; ++i) {
        CHECK(WtPokemon_Decode(images[i], canonical) == 0);
        if ((canonical[8] || canonical[9]) && memcmp(canonical, target, 4) == 0 && memcmp(canonical + 12, target + 12, 4) == 0)
            ++copies;
    }
    return copies;
}

/* A save with three party Pokémon (the offer in slot 1) and two in the PC. */
static void Scene(Save *save)
{
    unsigned char canonical[BOX];
    unsigned box, slot;
    memset(save, 0, sizeof(*save));
    for (box = 0; box < WT_GAME_BOX_COUNT; ++box) for (slot = 0; slot < WT_GAME_BOX_SLOTS; ++slot)
        Empty(NULL, save->boxes[box][slot], BOX);
    for (slot = 0; slot < (unsigned)BATTLE; ++slot) Empty(NULL, save->battle[slot], BOX);
    Empty(NULL, save->daycare, BOX);
    for (slot = 0; slot < 6; ++slot) Empty(NULL, save->party[slot], PARTY);
    Mon(canonical, 0x11111111, 0x01020304, 25, 90); PartyImage(canonical, save->party[0]);
    PartyImage(offered, save->party[1]);
    Mon(canonical, 0x33333333, 0x01020304, 133, 90); PartyImage(canonical, save->party[2]);
    save->count = 3;
    Mon(canonical, 0x44444444, 0x01020304, 1, 70); CHECK(WtPokemon_Encode(canonical, save->boxes[0][0]) == 0);
    Mon(canonical, 0x55555555, 0x01020304, 4, 70); CHECK(WtPokemon_Encode(canonical, save->boxes[0][2]) == 0);
    save->counter = 100;
}

static void Flash(const Save *save)
{
    flash[0].save = *save; flash[0].save.counter = save->counter - 1; flash[0].valid = 1;
    flash[1].save = *save; flash[1].valid = 1;
}

enum Scenario { IN_PLACE, APPEND, SERVER_RETURN, MATCHED, SCENARIOS };
static const char *const kScenarios[] = { "restore in place", "restore after play", "server return", "matched delivery" };

/* The journal and save the scenario starts from: an offer escrowed from slot 1
 * and removed; then rejected (RESTORING) or collected (RECEIVED). */
static void Start(enum Scenario scenario)
{
    WtJournalEntry entry;
    Save before;
    WtJournal store;
    WtJournalIO io = { NULL, JournalRead, JournalWrite, Sha };
    unsigned phase = scenario == SERVER_RETURN || scenario == MATCHED ? WT_JOURNAL_RECEIVED : WT_JOURNAL_RESTORING;
    Mon(offered, 0x22222222, 0x01020304, 150, 120);
    Mon(matched, 0x66666666, 0x0A0B0C0D, 448, 255);
    Scene(&before);
    CHECK(WtTest_JournalEntry(phase, Sha, &entry) == 0);
    memcpy(entry.original, before.party[1], 236); memset(entry.original + PARTY, 0, 236 - PARTY);
    entry.source.length = PARTY; entry.source.slot = 1;
    memcpy(entry.outgoing, offered, BOX);
    CHECK(Sha(entry.original, PARTY, entry.originalDigest) == 0);
    CHECK(Digest(&before, entry.inventoryBefore) == 0);
    live = before; RemoveSlot(&live, 1); live.counter += 2;
    CHECK(Digest(&live, entry.inventoryAfter) == 0);
    entry.globalCounter = entry.generalCounter = entry.boxCounter = live.counter;
    if (scenario != IN_PLACE) { ++live.playTime; live.counter += 3; } /* Played and saved since. */
    if (scenario == SERVER_RETURN) {
        entry.deliveryKind = WT_DELIVERY_RETURN; memcpy(entry.incoming, offered, BOX);
        CHECK(Sha(entry.incoming, BOX, entry.incomingDigest) == 0);
    } else if (scenario == MATCHED) {
        memcpy(entry.incoming, matched, BOX);
        CHECK(Sha(entry.incoming, BOX, entry.incomingDigest) == 0);
    }
    Flash(&live);
    memset(journal, 0, sizeof(journal));
    CHECK(WtJournal_Encode(&entry, Sha, journal[0]) == 0);
    memcpy(journal[1], journal[0], sizeof(journal[0]));
    CHECK(WtJournal_Open(&store, io) == 0);
    WtJournal_Close(&store);
}

static void Open(WtEngine *engine, int random)
{
    WtJournalIO io = { NULL, JournalRead, JournalWrite, Sha };
    WtEngineIO game = { NULL, random ? Random : NoRandom, Inventory, CanHold };
    CHECK(WtEngine_Open(engine, io, game) == 0);
}

/* The boot: the newest retail copy, then the engine's local steps. */
static int Boot(WtEngine *engine)
{
    int result;
    CHECK(Load(&live) >= 0);
    Open(engine, 0);
    result = WtEngine_Settle(engine);
    return result;
}

static const unsigned char *Target(enum Scenario scenario)
{
    return scenario == MATCHED ? matched : offered;
}

enum Play { PLAY_NONE, PLAY_FRIENDSHIP, PLAY_DAYCARE, PLAY_BATTLE_BOX, PLAY_ACTIONS };
static const char *const kPlays[] = { "saves", "walks it (friendship +1)", "leaves it at the Day Care", "puts it in the Battle Box" };

/* The unmodified game: load, play, save over the other copy. Nonzero when
 * the save it loaded held the target. */
static int Stock(enum Scenario scenario, enum Play play, unsigned saves)
{
    Save save;
    unsigned char canonical[BOX], *image = NULL;
    const unsigned char *target = Target(scenario);
    unsigned i, slot = 0, box = 0, kind = 0, held, n;
    CHECK(Load(&save) >= 0);
    held = Copies(&save, target) != 0;
    for (n = 0; n < saves; ++n) {
        ++save.playTime;
        for (i = 0; i < save.count && !kind; ++i) {
            CHECK(WtPokemon_Decode(save.party[i], canonical) == 0);
            if (memcmp(canonical, target, 4) == 0) { kind = 1; slot = i; image = save.party[i]; }
        }
        for (i = 0; i < WT_GAME_BOX_COUNT * WT_GAME_BOX_SLOTS && !kind; ++i) {
            CHECK(WtPokemon_Decode(save.boxes[i / WT_GAME_BOX_SLOTS][i % WT_GAME_BOX_SLOTS], canonical) == 0);
            if (memcmp(canonical, target, 4) == 0) {
                kind = 2; box = i / WT_GAME_BOX_SLOTS; slot = i % WT_GAME_BOX_SLOTS; image = save.boxes[box][slot];
            }
        }
        if (kind && play == PLAY_FRIENDSHIP) {
            CHECK(WtPokemon_Decode(image, canonical) == 0);
            ++canonical[0x14]; Checksum(canonical);
            CHECK(WtPokemon_Encode(canonical, image) == 0);
        } else if (kind && (play == PLAY_DAYCARE || (play == PLAY_BATTLE_BOX && kBattleBox))) {
            unsigned char *to = play == PLAY_DAYCARE ? save.daycare : save.battle[BATTLE - 1];
            memcpy(to, image, BOX);
            if (kind == 1) RemoveSlot(&save, slot); else Empty(NULL, image, BOX);
        }
        Write(&save);
        kind = 0;
    }
    return (int)held;
}

static void Seed(void)
{
    memset(flash, 0, sizeof(flash)); scratchUsed = 0;
    events = cutAt = cutMode = 0; saveWrites = journalWrites = 0;
}

/* ---- Scan, uniqueness and the journal's Battle Box ---- */

static void ScanCases(void)
{
    WtDeliveryScan scan;
    unsigned char canonical[BOX], twin[BOX];
    Seed(); Start(IN_PLACE);
    /* The removed offer is absent and unique in the escrow's save. */
    CHECK(WtDelivery_Absent(&sOps, &live, offered) == 0 && WtDelivery_Unique(&sOps, &live, offered) == 0);
    /* A twin (same identity, other image) is not the offer, but is not unique. */
    memcpy(twin, offered, BOX); ++twin[0x14]; Checksum(twin);
    CHECK(WtPokemon_Encode(twin, live.boxes[3][4]) == 0);
    CHECK(WtDelivery_Absent(&sOps, &live, offered) == 0 && WtDelivery_Unique(&sOps, &live, offered) != 0);
    CHECK(WtDelivery_Scan(&sOps, &live, offered, &scan) == 0 && scan.identities == 1 && scan.exact == 0);
    CHECK(scan.where.kind == 2 && scan.where.box == 3 && scan.where.slot == 4 && scan.where.length == BOX);
    CHECK(memcmp(scan.canonical, twin, BOX) == 0);
    /* The exact image is present. */
    CHECK(WtPokemon_Encode(offered, live.boxes[5][6]) == 0);
    CHECK(WtDelivery_Absent(&sOps, &live, offered) != 0);
    CHECK(WtDelivery_Scan(&sOps, &live, offered, &scan) == 0 && scan.identities == 2 && scan.exact == 1);
    /* A different personality, or trainer, is another Pokémon. */
    Empty(NULL, live.boxes[3][4], BOX); Empty(NULL, live.boxes[5][6], BOX);
    memcpy(canonical, offered, BOX); ++canonical[0]; Checksum(canonical);
    CHECK(WtPokemon_Encode(canonical, live.boxes[1][1]) == 0);
    memcpy(canonical, offered, BOX); ++canonical[13]; Checksum(canonical);
    CHECK(WtPokemon_Encode(canonical, live.boxes[1][2]) == 0);
    CHECK(WtDelivery_Unique(&sOps, &live, offered) == 0);
    /* An unreadable slot makes neither check provable. */
    live.boxes[17][29][7] ^= 1;
    CHECK(WtDelivery_Absent(&sOps, &live, offered) != 0 && WtDelivery_Unique(&sOps, &live, offered) != 0);
    CHECK(WtDelivery_Scan(&sOps, &live, offered, &scan) == 0 && scan.unreadable == 1);
    live.boxes[17][29][7] ^= 1;
    /* Party slots past the count are not Pokémon; the count must be one to six. */
    PartyImage(offered, live.party[5]);
    CHECK(WtDelivery_Unique(&sOps, &live, offered) == 0);
    live.count = 0; CHECK(WtDelivery_Scan(&sOps, &live, offered, &scan) != 0);
    live.count = 7; CHECK(WtDelivery_Scan(&sOps, &live, offered, &scan) != 0);
    live.count = 2;
    /* The Battle Box is scanned where the game has one. */
    CHECK(WtPokemon_Encode(offered, live.battle[BATTLE - 1]) == 0);
    CHECK(WtDelivery_Scan(&sOps, &live, offered, &scan) == 0);
    CHECK(scan.identities == (WT_GAME_BATTLE_BOX_SLOTS ? 1u : 0u));
    if (WT_GAME_BATTLE_BOX_SLOTS) CHECK(scan.where.kind == 3 && scan.where.slot == WT_GAME_BATTLE_BOX_SLOTS - 1);
    puts("scan: identity, exact image, unreadable slots, party count and Battle Box");
}

/* ---- Flows ---- */

/* The first boot after a cut; with stock play it is the boot after that game saved. */
static void Expect(enum Scenario scenario, unsigned saves, int held, int unwritten, const char *what)
{
    WtEngine engine;
    WtJournalEntry settled;
    unsigned char canonical[BOX], digest[32];
    const unsigned char *target = Target(scenario);
    unsigned copies;
    int result = Boot(&engine);
    if (result != 0 || engine.blocked) { fprintf(stderr, "%s: %s: settle %d blocked %d\n", kScenarios[scenario], what, result, engine.blocked); exit(1); }
    copies = Copies(&live, target);
    /* Never twice. Once only when the delivery cannot have been written, or the
     * stock game played on a save that held it. */
    if (copies > 1 || ((!saves || held || unwritten) && copies != 1) || (saves && !held && !unwritten && copies != 0)) {
        fprintf(stderr, "%s: %s: %u copies (stock saves %u, held %d, unwritten %d)\n",
            kScenarios[scenario], what, copies, saves, held, unwritten);
        exit(1);
    }
    if (scenario == MATCHED) CHECK(Copies(&live, offered) == 0);
    CHECK(engine.entry.phase == (scenario == SERVER_RETURN || scenario == MATCHED ? WT_JOURNAL_APPLIED : WT_JOURNAL_IDLE));
    CHECK(WtDelivery_CanLeave(&sOps, &engine.entry));
    if (engine.entry.phase == WT_JOURNAL_APPLIED && copies == 1 && engine.entry.destination.kind) {
        const unsigned char *stored = Slot(NULL, &live, engine.entry.destination);
        /* Recorded where it is, as it is; the ACK reports that image. */
        /* Otherwise it is where no locator reaches, and the record stays as prepared. */
        if (stored && memcmp(stored, engine.entry.destinationAfter, engine.entry.destination.length) == 0) {
            CHECK(WtPokemon_Decode(stored, canonical) == 0 && Sha(canonical, BOX, digest) == 0);
            CHECK(memcmp(digest, engine.entry.appliedDigest, 32) == 0);
        }
    }
    settled = engine.entry;
    WtEngine_Close(&engine);
    /* Settling again changes nothing. */
    CHECK(Boot(&engine) == 0 && memcmp(&settled, &engine.entry, sizeof(settled)) == 0 && Copies(&live, target) == copies);
    if (engine.entry.phase == WT_JOURNAL_APPLIED) {
        WtEngine_Close(&engine); Open(&engine, 1);
        CHECK(WtEngine_Request(&engine, 0) == 0 && engine.frame[5] == WT_OP_ACKNOWLEDGE);
        CHECK(memcmp(engine.frame + WT_WIRE_REQUEST_HEADER_SIZE + WT_WIRE_ACK_APPLIED_DIGEST,
            settled.appliedDigest, 32) == 0);
    }
    WtEngine_Close(&engine);
}

/* Whether the journal at the cut still holds nothing a write could follow:
 * RECEIVED, or a restore not yet recorded as a return. */
static int Unwritten(void)
{
    WtJournal store;
    WtJournalEntry entry;
    WtJournalIO io = { NULL, JournalRead, JournalWrite, Sha };
    int unwritten;
    CHECK(WtJournal_Open(&store, io) == 0 && WtJournal_Read(&store, &entry) == 0);
    WtJournal_Close(&store);
    unwritten = entry.phase == WT_JOURNAL_RECEIVED
        || (entry.phase == WT_JOURNAL_RESTORING && entry.deliveryKind == WT_DELIVERY_NONE);
    return unwritten;
}

/* Every cut in every scenario, then the patched game at once or after the
 * stock game saved once or twice with each kind of play. */
static void Sweep(void)
{
    enum Scenario scenario;
    unsigned total, cut, mode, play, saves, cases = 0, forfeits = 0;
    char what[160];
    for (scenario = IN_PLACE; scenario < SCENARIOS; ++scenario) {
        WtEngine engine;
        Seed(); Start(scenario);
        CHECK(Boot(&engine) == 0); WtEngine_Close(&engine);
        total = events;
        CHECK(total >= 4);
        for (cut = 1; cut <= total; ++cut) for (mode = 0; mode < 3; ++mode)
        for (play = PLAY_NONE; play < PLAY_ACTIONS; ++play) for (saves = 0; saves <= 2; ++saves) {
            int held = 1, unwritten;
            if ((saves == 0 && play != PLAY_NONE) || (play == PLAY_BATTLE_BOX && !kBattleBox)) continue;
            Seed(); Start(scenario);
            cutAt = cut; cutMode = mode; events = 0;
            if (!setjmp(power)) {
                CHECK(Boot(&engine) == 0);
                fprintf(stderr, "%s: cut %u never reached\n", kScenarios[scenario], cut); exit(1);
            }
            cutAt = 0; scratchUsed = 0;
            unwritten = Unwritten();
            if (saves) held = Stock(scenario, (enum Play)play, saves);
            snprintf(what, sizeof(what), "cut %u/%u mode %u, then the stock game %s x%u", cut, total, mode, kPlays[play], saves);
            Expect(scenario, saves, held, unwritten, what);
            ++cases; forfeits += saves && !held && !unwritten;
        }
    }
    printf("sweep: %u power cuts and stock saves, never duplicated, never blocked; %u settled without a "
        "Pokemon the stock game never had\n", cases, forfeits);
}

/* Finding: ordinary play in the stock game (friendship from walking) after a
 * restore was written. The changed image is accepted, kept and reported. */
static void ChangedByPlay(void)
{
    enum Scenario scenario;
    for (scenario = IN_PLACE; scenario < SCENARIOS; ++scenario) {
        WtEngine engine;
        unsigned char canonical[BOX], digest[32];
        unsigned cut, held = 0;
        /* The first retail write of the delivery or restore. */
        for (cut = 1; !held; ++cut) {
            Seed(); Start(scenario); cutAt = cut; cutMode = 1; events = 0;
            if (!setjmp(power)) { CHECK(Boot(&engine) == 0); CHECK(0); }
            cutAt = 0; scratchUsed = 0;
            if (Load(&live) >= 0 && Copies(&live, Target(scenario)) == 1 && saveWrites) held = 1;
        }
        CHECK(Stock(scenario, PLAY_FRIENDSHIP, 1) == 1);
        CHECK(Boot(&engine) == 0 && Copies(&live, Target(scenario)) == 1);
        if (engine.entry.phase == WT_JOURNAL_APPLIED) {
            const unsigned char *stored = Slot(NULL, &live, engine.entry.destination);
            CHECK(stored && memcmp(stored, engine.entry.destinationAfter, engine.entry.destination.length) == 0);
            CHECK(WtPokemon_Decode(stored, canonical) == 0);
            CHECK(canonical[0x14] == (scenario == MATCHED ? 70 : Target(scenario)[0x14]) + 1);
            CHECK(Sha(canonical, BOX, digest) == 0 && memcmp(digest, engine.entry.appliedDigest, 32) == 0);
        } else CHECK(engine.entry.phase == WT_JOURNAL_IDLE);
        WtEngine_Close(&engine);
    }
    puts("changed by play: the stock game's friendship +1 is accepted, recorded and reported, never added again");
}

/* Finding: a restore in place, cut after its first retail write; the stock
 * game leaves the Pokémon at the Day Care and saves. It is not put back. */
static void MovedToDaycare(void)
{
    WtEngine engine;
    unsigned cut;
    for (cut = 1;; ++cut) {
        Seed(); Start(IN_PLACE); cutAt = cut; cutMode = 1; events = 0;
        if (!setjmp(power)) { CHECK(Boot(&engine) == 0); CHECK(0); }
        cutAt = 0; scratchUsed = 0;
        if (saveWrites) break;
    }
    CHECK(Stock(IN_PLACE, PLAY_DAYCARE, 2) == 1);
    CHECK(Boot(&engine) == 0 && engine.entry.phase == WT_JOURNAL_IDLE);
    CHECK(Copies(&live, offered) == 1 && live.count == 2);
    WtEngine_Close(&engine);
    puts("restore in place cut short, then the Day Care: one copy, at the Day Care");
}

/* A journal an older build left: its restore in place was written, without
 * being recorded first, then the stock game walked the Pokémon and saved. The
 * return finds it back, changed, and adds nothing. */
static void OlderBuildRestore(void)
{
    WtEngine engine;
    WtJournal store;
    WtJournalEntry entry;
    WtJournalIO io = { NULL, JournalRead, JournalWrite, Sha };
    Seed(); Start(IN_PLACE);
    CHECK(WtJournal_Open(&store, io) == 0 && WtJournal_Read(&store, &entry) == 0);
    WtJournal_Close(&store);
    CHECK(Load(&live) >= 0);
    InsertSlot(&live, 1, entry.original);
    Write(&live); Write(&live);
    CHECK(Stock(IN_PLACE, PLAY_FRIENDSHIP, 1) == 1);
    CHECK(Boot(&engine) == 0 && engine.entry.phase == WT_JOURNAL_IDLE);
    CHECK(Copies(&live, offered) == 1 && live.count == 3 && memcmp(live.party[1], entry.original, PARTY) != 0);
    WtEngine_Close(&engine);
    puts("older build's restore in place, then walked in the stock game: back once, as it is");
}

/* Battle Box: a returned offer the stock game moved there is recorded with
 * journal locator kind 3, and its journal is valid. */
static void BattleBoxRecord(void)
{
    WtEngine engine;
    unsigned cut;
    if (!WT_GAME_BATTLE_BOX_SLOTS) return;
    for (cut = 1;; ++cut) {
        Seed(); Start(SERVER_RETURN); cutAt = cut; cutMode = 1; events = 0;
        if (!setjmp(power)) { CHECK(Boot(&engine) == 0); CHECK(0); }
        cutAt = 0; scratchUsed = 0;
        if (saveWrites) break;
    }
    CHECK(Stock(SERVER_RETURN, PLAY_BATTLE_BOX, 1) == 1);
    CHECK(Boot(&engine) == 0 && engine.entry.phase == WT_JOURNAL_APPLIED);
    CHECK(engine.entry.destination.kind == 3 && engine.entry.destination.box == 0);
    CHECK(engine.entry.destination.slot == WT_GAME_BATTLE_BOX_SLOTS - 1 && engine.entry.destination.length == BOX);
    CHECK(WtDelivery_Verify(&sOps, &engine.entry) == 0 && Copies(&live, offered) == 1);
    WtEngine_Close(&engine);
    puts("Battle Box: the returned offer is recorded at locator kind 3");
}

/* What still blocks, without writing anything: an older save, a save changed
 * with the recorded counters, and a matched delivery whose offer is back. */
static void Refusals(void)
{
    enum { OLDER, SAME_COUNTERS, OFFER_BACK_PREPARE, OFFER_BACK_RECOVER, CASES };
    unsigned which;
    for (which = OLDER; which < CASES; ++which) {
        WtEngine engine;
        unsigned char banks[2][WT_JOURNAL_RECORD_SIZE];
        Bank before[2];
        unsigned cut;
        enum Scenario scenario = which >= OFFER_BACK_PREPARE ? MATCHED : SERVER_RETURN;
        if (which == OFFER_BACK_PREPARE) {
            Seed(); Start(scenario);
            CHECK(WtPokemon_Encode(offered, flash[0].save.boxes[2][2]) == 0);
            CHECK(WtPokemon_Encode(offered, flash[1].save.boxes[2][2]) == 0);
        } else {
            for (cut = 1;; ++cut) {
                Seed(); Start(scenario); cutAt = cut; cutMode = 0; events = 0;
                if (!setjmp(power)) { CHECK(Boot(&engine) == 0); CHECK(0); }
                cutAt = 0; scratchUsed = 0;
                if (!Unwritten()) break;
            }
            /* The boot that follows repairs the journal's mirror first. */
            Open(&engine, 0); WtEngine_Close(&engine);
            CHECK(Load(&live) >= 0);
            if (which == OLDER) { live.counter -= 10; ++live.playTime; }
            if (which == SAME_COUNTERS) ++live.playTime;
            if (which == OFFER_BACK_RECOVER) { ++live.counter; CHECK(WtPokemon_Encode(offered, live.boxes[2][2]) == 0); }
            flash[0].save = live; flash[0].valid = 1; flash[1].valid = 0;
        }
        memcpy(banks, journal, sizeof(banks)); memcpy(before, flash, sizeof(before));
        saveWrites = journalWrites = 0;
        CHECK(Boot(&engine) != 0 && engine.blocked && saveWrites == 0);
        CHECK(memcmp(banks, journal, sizeof(banks)) == 0 && memcmp(before, flash, sizeof(before)) == 0);
        WtEngine_Close(&engine);
    }
    puts("refused without writes: an older save, a changed save with the recorded counters, a matched delivery's offer back");
}

#ifdef WT_COMPANION_ESCROWS
/* ---- LUNE-902: the patched games never advance a DS Companion escrow ---- */

/* The DS Companion's engine, built into this test with WT_ABANDON_UNSENT and
 * its public names prefixed (companion_engine_rename.h). */
int CpEngine_Open(WtEngine *, WtJournalIO, WtEngineIO);
void CpEngine_Close(WtEngine *);
int CpEngine_Deposit(WtEngine *, unsigned slot);
int CpEngine_Settle(WtEngine *);
int CpEngine_EscrowInventories(const WtJournalEntry *, WtJournalHash, unsigned char before[32], unsigned char after[32]);
/* The companion engine's WT_INVENTORY_HISTORY and its answers (engine.h). */
enum { HISTORY_STEP = WT_INVENTORY_PREPARE_RESTORE + 1, HISTORY_UNPROVEN = -1, HISTORY_UNCHANGED = 0,
       HISTORY_LEFT = 1, HISTORY_REMOVED = 2, HISTORY_AFTER = 3 };
static const uint32_t kSaves = UINT32_C(0x8000000);

/* The companion adapter's steps, as adapter/platinum.c and gen5.c take them. */
static int CompanionInventory(void *arg, enum WtInventoryStep step, WtJournalEntry *entry, unsigned slot)
{
    unsigned char digest[32];
    uint32_t past;
    (void)arg;
    if ((unsigned)step == HISTORY_STEP) {
        if (Digest(&live, digest) != 0) return HISTORY_UNPROVEN;
        if (memcmp(digest, entry->inventoryBefore, 32) == 0) return HISTORY_UNCHANGED;
        if (memcmp(digest, entry->inventoryAfter, 32) == 0) return HISTORY_AFTER;
        past = live.counter - entry->globalCounter;
        if (past >= kStep && past - kStep < kSaves) return HISTORY_REMOVED;
        if (past == 0 || past >= kSaves) return HISTORY_UNPROVEN;
        return Copies(&live, entry->outgoing) ? HISTORY_UNCHANGED : HISTORY_LEFT;
    }
    if (step == WT_INVENTORY_PREPARE_REMOVE || step == WT_INVENTORY_REMOVE) return GameRemove(step, entry, slot, 1);
    return Inventory(arg, step, entry, slot);
}

/* The journal's current entry. */
static WtJournalEntry Current(void)
{
    WtJournal store;
    WtJournalEntry entry;
    WtJournalIO io = { NULL, JournalRead, JournalWrite, Sha };
    CHECK(WtJournal_Open(&store, io) == 0 && WtJournal_Read(&store, &entry) == 0);
    WtJournal_Close(&store);
    return entry;
}

/* An enrolled, idle save with the offer in party slot 1. */
static void Idle(void)
{
    WtJournalEntry entry;
    Save save;
    Seed();
    Mon(offered, 0x22222222, 0x01020304, 150, 120);
    Scene(&save);
    Flash(&save); live = save;
    CHECK(WtTest_JournalEntry(WT_JOURNAL_IDLE, Sha, &entry) == 0);
    CHECK(WtJournal_Encode(&entry, Sha, journal[0]) == 0);
    memcpy(journal[1], journal[0], sizeof(journal[0]));
}

static void Deposit(void)
{
    WtEngine engine;
    WtJournalIO io = { NULL, JournalRead, JournalWrite, Sha };
    WtEngineIO companion = { NULL, Random, CompanionInventory, CanHold };
    CHECK(Load(&live) >= 0);
    CHECK(CpEngine_Open(&engine, io, companion) == 0);
    engine.synchronized = 1;
    CHECK(CpEngine_Deposit(&engine, 1) == 0);
    CpEngine_Close(&engine);
}

/* The Submit was rejected: the journal as Reconcile leaves it (RESTORING). */
static void Rejected(void)
{
    WtJournal store;
    WtJournalEntry entry = Current(), next;
    WtJournalIO io = { NULL, JournalRead, JournalWrite, Sha };
    CHECK(entry.phase == WT_JOURNAL_SUBMIT_READY);
    next = entry;
    memcpy(next.lastOperation, next.operation, 16); next.lastOperationCounter = next.operationCounter;
    next.nextCounter = next.operationCounter + 1; next.phase = WT_JOURNAL_RESTORING;
    memset(next.request, 0, sizeof(next.request)); next.requestLength = 0;
    next.generation = entry.generation + 1;
    CHECK(WtJournal_Open(&store, io) == 0 && WtJournal_Commit(&store, &next) == 0);
    WtJournal_Close(&store);
}

/* Every cut of the companion's deposit (its ESCROW, its removal moving the
 * counter, its SUBMIT_READY), each with and without a stock-game save after
 * it, then the patched game's boot with the game adapters' own decisions:
 * in ESCROW it blocks before writing anything (both inventory hashes are
 * masked, so REMOVE matches neither); from SUBMIT_READY, which records the
 * true hashes, it settles, and a rejected offer is restored exactly once. */
static void CompanionEscrows(void)
{
    WtEngine engine;
    unsigned total, cut, mode, stock, escrows = 0, restored = 0;
    Idle(); Deposit(); total = events;
    CHECK(Current().phase == WT_JOURNAL_SUBMIT_READY && Copies(&live, offered) == 0);
    {
        /* The companion's ESCROW stores both hashes masked; SUBMIT_READY true ones. */
        WtJournalEntry entry = Current();
        unsigned char before[32], after[32];
        CHECK(CpEngine_EscrowInventories(&entry, Sha, before, after) == 0);
    }
    for (cut = 1; cut <= total + 1; ++cut) for (mode = 0; mode < 3; ++mode) for (stock = 0; stock < 2; ++stock) {
        static unsigned char flashBefore[sizeof(flash)];
        WtJournalEntry entry;
        unsigned writes;
        int result;
        if (cut > total && mode) continue;
        Idle();
        cutAt = cut <= total ? cut : 0; cutMode = mode; events = 0;
        if (!setjmp(power)) Deposit();
        cutAt = 0; scratchUsed = 0;
        if (stock) { CHECK(Load(&live) >= 0); ++live.playTime; Write(&live); }
        entry = Current();
        memcpy(flashBefore, flash, sizeof(flash));
        writes = saveWrites;
        result = Boot(&engine);
        if (entry.phase == WT_JOURNAL_ESCROW) {
            unsigned char before[32], after[32];
            CHECK(CpEngine_EscrowInventories(&entry, Sha, before, after) == 1);
            CHECK(memcmp(before, entry.inventoryBefore, 32) != 0 && memcmp(after, entry.inventoryAfter, 32) != 0);
            /* Blocked; the save untouched; the journal, which the engine's open
             * may only mirror between its banks, still this escrow. */
            if (result != -1 || !engine.blocked || saveWrites != writes || memcmp(flashBefore, flash, sizeof(flash)) != 0
                || Current().generation != entry.generation || Current().phase != WT_JOURNAL_ESCROW
                || memcmp(Current().inventoryBefore, entry.inventoryBefore, 32) != 0) {
                fprintf(stderr, "companion escrow cut %u mode %u stock %u: the game advanced it\n", cut, mode, stock);
                exit(1);
            }
            CHECK(!WtDelivery_CanLeave(&sOps, &entry));
            ++escrows;
        } else if (entry.phase == WT_JOURNAL_SUBMIT_READY) {
            CHECK(result == 0 && !engine.blocked && Copies(&live, offered) == 0);
            WtEngine_Close(&engine);
            /* Rejected: the patched game restores it, once. */
            Rejected();
            CHECK(Boot(&engine) == 0 && engine.entry.phase == WT_JOURNAL_IDLE && Copies(&live, offered) == 1);
            ++restored;
        } else CHECK(entry.phase == WT_JOURNAL_IDLE && result == 0 && Copies(&live, offered) == 1);
        WtEngine_Close(&engine);
    }
    CHECK(escrows > 0 && restored > 0);
    printf("companion escrows: %u cuts (and stock saves) left in ESCROW, the game blocked without a write every "
        "time; %u from SUBMIT_READY rejected and restored once\n", escrows, restored);
}
#endif

int main(void)
{
    ScanCases();
    ChangedByPlay();
    MovedToDaycare();
    OlderBuildRestore();
    BattleBoxRecord();
    Refusals();
    Sweep();
#ifdef WT_COMPANION_ESCROWS
    CompanionEscrows();
#endif
    return 0;
}
