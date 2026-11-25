/* LUNE-880: the adapter on Diamond/Pearl (FX_GAME 1) and HeartGold/SoulSilver
 * (FX_GAME 2) saves, as platinum_test.c and platinum_receive_test.c check it
 * on Platinum's: every inventory step, the game's own refusals and the receive
 * effects of client/src/wonder_trade/dp/ and hgss/ game_save.c. Expected
 * values are worked out here from the decomps, not by calling the code under
 * test. */
#include "save_fixture.h"

#include <stdio.h>
#include <stdlib.h>

#include "game/text.h"

#if FX_GAME != 1 && FX_GAME != 2
#error "FX_GAME must be 1 (Diamond/Pearl) or 2 (HeartGold/SoulSilver)"
#endif
#define DP (FX_GAME == 1)
#define HGSS (FX_GAME == 2)

static FxFile file;
static CpSaveImage image, expected;
static CpFile cpFile = { &file, FxRead, FxWrite, FxSync, FxSize };
static WtGame game;
static unsigned char fileBefore[CP_SAVE_FILE_SIZE];

/* ---- The fake game data (as platinum_receive_test.c) ---------------------- */

typedef struct FakeSpecies {
    uint16_t species;
    uint8_t form, base[6], ratio, growth, abilities[2];
} FakeSpecies;

static const FakeSpecies kSpecies[] = {
    { 64, 0, { 40, 35, 30, 105, 120, 70 }, 63, 3, { 28, 39 } },    /* Kadabra */
    { 65, 0, { 55, 50, 45, 120, 135, 85 }, 63, 3, { 28, 39 } },    /* Alakazam */
    { 93, 0, { 45, 50, 45, 95, 115, 55 }, 127, 3, { 26, 0 } },     /* Haunter */
    { 94, 0, { 60, 65, 60, 110, 130, 75 }, 127, 3, { 26, 0 } },    /* Gengar */
    { 95, 0, { 35, 45, 160, 70, 30, 45 }, 127, 0, { 69, 5 } },     /* Onix */
    { 208, 0, { 75, 85, 200, 30, 55, 65 }, 127, 0, { 69, 5 } },    /* Steelix */
    { 172, 0, { 20, 40, 15, 60, 35, 35 }, 127, 0, { 9, 0 } },      /* Pichu */
    { 385, 0, { 100, 100, 100, 100, 100, 100 }, 255, 5, { 32, 0 } }, /* Jirachi */
    { 492, 0, { 100, 100, 100, 100, 100, 100 }, 255, 3, { 30, 0 } }, /* Shaymin Land */
    { 492, 1, { 100, 103, 75, 127, 120, 75 }, 255, 3, { 32, 0 } }, /* Shaymin Sky */
};

static int FakePersonal(void *arg, uint16_t species, uint8_t form, CpPersonal *out)
{
    unsigned i, fallback = sizeof(kSpecies);
    (void)arg;
    for (i = 0; i < sizeof(kSpecies) / sizeof(kSpecies[0]); ++i) {
        if (kSpecies[i].species != species) continue;
        if (kSpecies[i].form == form) { fallback = i; break; }
        if (kSpecies[i].form == 0) fallback = i;
    }
    if (fallback < sizeof(kSpecies) / sizeof(kSpecies[0])) {
        const FakeSpecies *s = &kSpecies[fallback];
        memcpy(out->baseStats, s->base, 6);
        out->genderRatio = s->ratio; out->growthRate = s->growth; out->baseFriendship = 0;
        out->abilities[0] = s->abilities[0]; out->abilities[1] = s->abilities[1];
        return CP_OK;
    }
    if (species == 0 || species > 493) return CP_ERR_NOT_FOUND;
    memset(out->baseStats, 50, 6);
    out->genderRatio = 127; out->growthRate = 3; out->baseFriendship = 0;
    out->abilities[0] = (uint8_t)(species & 0x7F); out->abilities[1] = (uint8_t)((species & 0x7F) + 1);
    return CP_OK;
}

static int FakeExp(void *arg, uint8_t growth, uint32_t out[CP_MAX_LEVEL + 1])
{
    unsigned level;
    (void)arg;
    if (growth > 5) return CP_ERR_NOT_FOUND;
    out[0] = 0; out[1] = 0;
    for (level = 2; level <= CP_MAX_LEVEL; ++level) out[level] = level * level * level;
    return CP_OK;
}

static int FakeEvolution(void *arg, uint16_t species, uint16_t held, CpTradeEvolution *out)
{
    (void)arg;
    if (species == 64) { out->target = 65; out->item = 0; return CP_OK; }
    if (species == 93) { out->target = 94; out->item = 0; return CP_OK; }
    if (species == 95 && held == 233) { out->target = 208; out->item = 233; return CP_OK; }
    return CP_ERR_NOT_FOUND;
}

/* The Everstone's hold effect in each game's item data: 63 in Diamond/Pearl
 * (pokediamond constants/items.h HOLD_EFFECT_NO_EVOLVE), 64 later. Item 77's
 * effect is the other game's value. */
enum { EVERSTONE = 229, OTHER_STONE = 77, METAL_COAT = 233, NO_EVOLVE = DP ? 63 : 64 };

static int FakeHoldEffect(void *arg, uint16_t item, uint8_t *out)
{
    (void)arg;
    if (item == EVERSTONE) { *out = NO_EVOLVE; return CP_OK; }
    if (item == OTHER_STONE) { *out = DP ? 64 : 63; return CP_OK; }
    if (item == METAL_COAT || item == 1) { *out = 3; return CP_OK; }
    return CP_ERR_NOT_FOUND;
}

static int FakeMovePp(void *arg, uint16_t move, uint8_t *out)
{
    (void)arg;
    *out = (uint8_t)(move % 40 + 5);
    return CP_OK;
}

static const uint16_t kAlakazam[] = { 0x12B, 0x136, 0x12B, 0x135, 0x12B, 0x144, 0x12B, 0x137, CP_CHAR_EOS };

static int FakeName(void *arg, uint16_t species, uint16_t *out, size_t capacity, size_t *length)
{
    static const uint16_t other[] = { 0x130, CP_CHAR_EOS };
    const uint16_t *name = species == 65 ? kAlakazam : other;
    size_t n = 0;
    (void)arg;
    while (name[n] != CP_CHAR_EOS) ++n;
    if (n + 1 > capacity) return CP_ERR_RANGE;
    memcpy(out, name, (n + 1) * 2);
    *length = n;
    return CP_OK;
}

/* An English cartridge. */
static CpAdapterData data = { NULL, FakePersonal, FakeExp, FakeEvolution, FakeHoldEffect, FakeMovePp, FakeName, 2 };

static int FakeHoldable(void *arg, unsigned item)
{
    (void)arg;
    return item != 300;
}

/* ---- The general block's receive state ----------------------------------- */

#if DP
enum { DEX = CP_RECEIVE_DP_POKEDEX_OFFSET, STATS = CP_RECEIVE_DP_STATS_OFFSET,
       HISTORY = CP_RECEIVE_DP_POKETCH_OFFSET + 0xB6, VERSION = 10 };
#else
enum { DEX = CP_RECEIVE_HGSS_POKEDEX_OFFSET, STATS = CP_RECEIVE_HGSS_STATS_OFFSET,
       WALKER = CP_RECEIVE_HGSS_WALKER_OFFSET, VERSION = 7 };
#endif

static unsigned R16(const unsigned char *p) { return p[0] | ((unsigned)p[1] << 8); }

/* Save_Pokedex_Init, GameStats init and (HGSS) a sealed GameStats block. */
static void FreshGeneral(unsigned char *general)
{
    unsigned char *dex = general + DEX;
#if DP
    memset(dex, 0, 0x13C);
    FxPut32(dex, 0xBEEFCAFE);
    memset(dex + 0x10C, 0xFF, 28);
    dex[0x108] = dex[0x109] = dex[0x10A] = dex[0x10B] = 0xFF;
    dex[0x43] = 0xFF; dex[0x83] = 0xFF;
    memset(general + STATS, 0, 0x14C);
    FxPut32(general + STATS, 77); /* a trainer score */
    memset(general + HISTORY, 0, 48);
#else
    unsigned char *stats = general + STATS;
    unsigned sum = 0, i;
    memset(dex, 0, 0x340);
    FxPut32(dex, 0xBEEFCAFE);
    memset(dex + 0x10C, 0xFF, 56);
    dex[0x108] = dex[0x109] = dex[0x10A] = dex[0x10B] = 0xFF;
    FxPut32(dex + 0x338, 0xFFFFFFFF); dex[0x33C] = dex[0x33D] = dex[0x33E] = 0xFF;
    dex[0x43] = 0xFF; dex[0x83] = 0xFF;
    dex[0x336] = 1; /* dexEnabled */
    memset(stats, 0, 0x1C0);
    FxPut16(stats + 0x1BE, 0x4321);
    FxPut32(stats + 8, 77);       /* the score */
    FxPut32(stats + 4 * 25, 3);   /* Wi-Fi trades */
    for (i = 8; i < 0x1BC; ++i) sum += stats[i];
    FxPut16(stats + 0x1BC, sum & 65535);
    FxCrypt(stats + 8, 0x1BC - 8, (sum & 65535) + (0x4321u << 16));
    memset(general + WALKER, 0, 0x134);
    FxPut32(general + WALKER + 0x130, 3); /* Pokewalker_Init's two courses */
#endif
}

/* A game stat as the game reads it. */
static uint32_t Stat(const unsigned char *general, unsigned id)
{
#if DP
    return Fx32(general + STATS + 4 * id);
#else
    unsigned char plain[0x1C0];
    unsigned sum = 0, i;
    memcpy(plain, general + STATS, sizeof(plain));
    FxCrypt(plain + 8, 0x1BC - 8, R16(plain + 0x1BC) + ((uint32_t)R16(plain + 0x1BE) << 16));
    for (i = 8; i < 0x1BC; ++i) sum += plain[i];
    assert((sum & 65535) == R16(plain + 0x1BC)); /* the checksum the game checks */
    return Fx32(plain + 4 * id);
#endif
}

enum { SCORE = DP ? 0 : 2 };

static int DexBit(const unsigned char *general, unsigned offset, unsigned species)
{
    return (general[DEX + offset + (species - 1) / 8] >> ((species - 1) % 8)) & 1;
}

/* ---- Saves ----------------------------------------------------------------- */

static void Setup(const FxSpec *specs, unsigned count)
{
    unsigned half;
    FxBuild(&file, specs, count);
    for (half = 0; half < 2; ++half) {
        FreshGeneral(FxGeneral(&file, half));
        FxSeal(&file, half, 0, 10 + half, 20 + half);
    }
    assert(FxLoad(&image, &cpFile) == CP_SAVE_OK);
    assert(image.layout == FX_LAYOUT && image.currentHalf[0] == 1 && image.currentHalf[1] == 1);
    memset(&game, 0, sizeof(game));
    game.image = &image; game.file = &cpFile; game.holdable = FakeHoldable; game.data = &data;
    game.version = VERSION;
}

static void Defaults(FxSpec *specs, unsigned count)
{
    unsigned i;
    for (i = 0; i < count; ++i) specs[i] = FxDefault(i);
}

static int Step(enum WtInventoryStep step, WtJournalEntry *entry, unsigned slot)
{
    return WtGame_Inventory(&game, step, entry, slot);
}

static void Unchanged(void)
{
    assert(memcmp(image.bytes, expected.bytes, sizeof(image.bytes)) == 0);
    assert(memcmp(file.bytes, fileBefore, sizeof(file.bytes)) == 0);
}

static void Keep(void)
{
    expected = image;
    memcpy(fileBefore, file.bytes, sizeof(fileBefore));
}

/* Both retail copies hold the image, as the game would load it. */
static void BothCopies(void)
{
    unsigned char digest[32];
    assert(CpSaveImage_Hash(&image, digest) == CP_SAVE_OK);
    assert(CpSaveImage_Verify(&image, &cpFile, digest) == CP_SAVE_OK);
    assert(FxLoad(&expected, &cpFile) == CP_SAVE_OK);
    assert(memcmp(expected.bytes, file.bytes, sizeof(file.bytes)) == 0);
}

#if HGSS
static unsigned char *Aprijuice(unsigned slot)
{
    return CpSaveImage_Block(&image, 0) + FX_APRIJUICE + 5 * slot;
}
#endif

/* ---- Offers ------------------------------------------------------------------ */

static void Refused(const char *why, unsigned slot)
{
    WtJournalEntry entry;
    memset(&entry, 0, sizeof(entry));
    Keep();
    assert(!CpAdapter_Eligible(&image, slot));
    assert(Step(WT_INVENTORY_PREPARE_REMOVE, &entry, slot) == -1);
    Unchanged();
    printf("PASS %s refuse: %s\n", FX_GAME_NAME, why);
}

static void Eligibility(void)
{
    FxSpec specs[6];
    WtJournalEntry entry;
    Defaults(specs, 3); Setup(specs, 3);
    assert(CpAdapter_Eligible(&image, 0) && CpAdapter_Eligible(&image, 2) && !CpAdapter_Eligible(&image, 3));
    assert(Step(WT_INVENTORY_PREPARE_REMOVE, &entry, 0) == 0);
    Defaults(specs, 3); specs[0].egg = 1; Setup(specs, 3); Refused("an Egg", 0);
    Defaults(specs, 3); specs[0].capsule = 2; Setup(specs, 3); Refused("a ball capsule", 0);
    Defaults(specs, 3); specs[0].item = 140; Setup(specs, 3); Refused("mail", 0);
    Defaults(specs, 3); specs[1].hp = 0; specs[2].egg = 1; Setup(specs, 3); Refused("the last usable party member", 0);
    /* Pichu: HeartGold/SoulSilver never offer the Spiky-eared one. */
    Defaults(specs, 3); specs[0].species = 172; specs[0].form = 1; Setup(specs, 3);
    if (HGSS) Refused("Spiky-eared Pichu", 0);
    else assert(CpAdapter_Eligible(&image, 0));
    Defaults(specs, 3); specs[0].species = 172; Setup(specs, 3);
    assert(CpAdapter_Eligible(&image, 0));
#if HGSS
    /* Aprijuice modifiers in the slot, which a restore could not bring back. */
    Defaults(specs, 3); Setup(specs, 3);
    Aprijuice(1)[4] = 9;
    Refused("a Pokémon with Aprijuice modifiers", 1);
    assert(CpAdapter_Eligible(&image, 0) && CpAdapter_Eligible(&image, 2));
#endif
    printf("PASS %s eligibility\n", FX_GAME_NAME);
}

/* REMOVE shifts the party down (and its Aprijuice modifiers), both copies are
 * written; RESTORE puts the offer back exactly, as the game's game_save.c. */
static void RemoveAndRestore(void)
{
    FxSpec specs[6];
    WtJournalEntry entry;
    unsigned char before[32], original[FX_MON], next[FX_MON], last[FX_MON];
    Defaults(specs, 4); Setup(specs, 4);
#if HGSS
    Aprijuice(2)[0] = 5; Aprijuice(3)[4] = 6; /* the slots after the offer */
    { unsigned char digest[32]; assert(CpSaveImage_Hash(&image, digest) == 0); assert(CpSaveImage_CommitBoth(&image, &cpFile, digest) == 0); }
#endif
    memcpy(original, FxImageParty(&image, 1), FX_MON);
    memcpy(next, FxImageParty(&image, 2), FX_MON);
    memcpy(last, FxImageParty(&image, 3), FX_MON);
    assert(CpSaveImage_Hash(&image, before) == CP_SAVE_OK);
    memset(&entry, 0, sizeof(entry));
    assert(Step(WT_INVENTORY_PREPARE_REMOVE, &entry, 1) == 0);
    assert(entry.source.kind == 1 && entry.source.slot == 1 && memcmp(entry.original, original, FX_MON) == 0);
    assert(memcmp(entry.inventoryBefore, before, 32) == 0);
    assert(entry.globalCounter == image.globalCounter);
    assert(Step(WT_INVENTORY_REMOVE, &entry, 1) == 0);
    assert(FxImageCount(&image) == 3);
    assert(memcmp(FxImageParty(&image, 1), next, FX_MON) == 0 && memcmp(FxImageParty(&image, 2), last, FX_MON) == 0);
#if HGSS
    assert(Aprijuice(1)[0] == 5 && Aprijuice(2)[4] == 6 && Aprijuice(3)[4] == 0);
#endif
    BothCopies();
    assert(Step(WT_INVENTORY_VERIFY_REMOVED, &entry, 1) == 0);
    /* The restore. */
    assert(Step(WT_INVENTORY_PREPARE_RESTORE, &entry, 1) == 0);
    assert(Step(WT_INVENTORY_RESTORE, &entry, 1) == 0);
    assert(FxImageCount(&image) == 4 && memcmp(FxImageParty(&image, 1), original, FX_MON) == 0);
    {
        unsigned char after[32];
        assert(CpSaveImage_Hash(&image, after) == CP_SAVE_OK && memcmp(after, before, 32) == 0);
    }
#if HGSS
    assert(Aprijuice(1)[0] == 0 && Aprijuice(2)[0] == 5 && Aprijuice(3)[4] == 6);
#endif
    BothCopies();
    printf("PASS %s remove and restore: party shift, both copies, the exact image back\n", FX_GAME_NAME);
}

/* The game's Behind: Diamond/Pearl's journal may record a box counter its
 * flash does not hold yet (the RAM counter after general-only saves). */
static void Counters(void)
{
    FxSpec specs[6];
    WtJournalEntry entry;
    Defaults(specs, 3); Setup(specs, 3);
    memset(&entry, 0, sizeof(entry));
    assert(Step(WT_INVENTORY_PREPARE_REMOVE, &entry, 1) == 0);
    entry.boxCounter += 3;
    assert(Step(WT_INVENTORY_REMOVE, &entry, 1) == 0);
    entry.boxCounter += 3;
    assert(Step(WT_INVENTORY_VERIFY_REMOVED, &entry, 1) == 0);
    /* A save older than the journal's global counter is refused. */
    entry.globalCounter = image.globalCounter + 1;
    Keep();
    assert(Step(WT_INVENTORY_VERIFY_REMOVED, &entry, 1) == -1);
    Unchanged();
#if DP
    /* Diamond/Pearl also judge the general counter. */
    entry.globalCounter = image.globalCounter;
    entry.generalCounter = image.blockCounters[0] + 1;
    assert(Step(WT_INVENTORY_VERIFY_REMOVED, &entry, 1) == -1);
    Unchanged();
#endif
    printf("PASS %s counters: older saves refused by the game's own counters\n", FX_GAME_NAME);
}

/* ---- Deliveries -------------------------------------------------------------- */

typedef struct Spec {
    unsigned species, item, form, language;
    uint32_t pid;
} Spec;

static void Canonical(const Spec *spec, unsigned char out[136])
{
    unsigned i, sum = 0;
    memset(out, 0, 136);
    FxPut32(out, spec->pid);
    FxPut16(out + 0x08, spec->species); FxPut16(out + 0x0A, spec->item); FxPut32(out + 0x0C, 0x00010002);
    FxPut32(out + 0x10, 125000); out[0x14] = 5; out[0x15] = 32; out[0x17] = (unsigned char)spec->language;
    FxPut16(out + 0x28, 33); out[0x30] = 10; out[0x34] = 2;
    FxPut32(out + 0x38, 0x3FFFFFFF & 0x1F);
    out[0x40] = (unsigned char)(spec->form << 3);
    FxPut16(out + 0x48, 0x12B); FxPut16(out + 0x4A, 0xFFFF);
    for (i = 0; i < 7; ++i) FxPut16(out + 0x68 + 2 * i, 0x13E + i);
    FxPut16(out + 0x76, 0xFFFF);
    out[0x83] = 4; out[0x84] = 10;
    for (i = 8; i < 136; i += 2) sum += R16(out + i);
    FxPut16(out + 6, sum & 65535);
}

static void Match(WtJournalEntry *entry, const Spec *spec)
{
    FxSpec offer = FxDefault(1);
    memset(entry, 0, sizeof(*entry));
    entry->deliveryKind = WT_DELIVERY_MATCH;
    entry->source.kind = 1; entry->source.length = FX_MON;
    FxPartyMon(&offer, entry->original);
    assert(WtPokemon_Decode(entry->original, entry->outgoing) == 0);
    Canonical(spec, entry->incoming);
}

/* PREPARE_RECEIVE, RECEIVE and VERIFY_RECEIVED, as the engine drives them. */
static void Deliver(WtJournalEntry *entry)
{
    assert(Step(WT_INVENTORY_PREPARE_RECEIVE, entry, 0) == 0);
    assert(Step(WT_INVENTORY_RECEIVE, entry, 0) == 0);
    assert(Step(WT_INVENTORY_VERIFY_RECEIVED, entry, 0) == 0);
    BothCopies();
}

static void PartyDelivery(void)
{
    FxSpec specs[6];
    WtJournalEntry entry;
    Spec spec = { 25, 0, 0, 5, 0x1234 }; /* Pikachu, German */
    unsigned char box[136], party[100], *general;
    WtReceivedPokemon received;
    unsigned i;
    Defaults(specs, 3); Setup(specs, 3);
#if HGSS
    Aprijuice(3)[2] = 7; /* left over in the empty slot: the delivery clears it */
    { unsigned char digest[32]; assert(CpSaveImage_Hash(&image, digest) == 0); assert(CpSaveImage_CommitBoth(&image, &cpFile, digest) == 0); }
#endif
    Match(&entry, &spec);
    Deliver(&entry);
    general = CpSaveImage_Block(&image, 0);
    assert(entry.destination.kind == 1 && entry.destination.slot == 3 && FxImageCount(&image) == 4);
    assert(WtPokemon_Decode(FxImageParty(&image, 3), box) == 0);
    memcpy(party, FxImageParty(&image, 3) + 136, 100); FxCrypt(party, 100, spec.pid);
    assert(box[0x14] == 70);                       /* friendship */
    assert(((box[0x40] >> 1) & 3) == (0x34 < 127)); /* gender from the species' ratio */
    assert(party[4] == 50);                       /* level: 125000 = 50^3 */
    /* Mail_Init: the cartridge's language and version, no sentences. */
    assert(party[20 + 5] == 2 && party[20 + 6] == VERSION && party[20 + 7] == 0xFF);
    for (i = 0; i < 3; ++i) assert(R16(party + 20 + 32 + 8 * i) == 0xFFFF && R16(party + 20 + 34 + 8 * i) == 0);
    /* The Pokédex: caught and seen, and the language where each game keeps it. */
    assert(DexBit(general, 4, 25) && DexBit(general, 0x44, 25));
#if DP
    assert(general[DEX + 0x129 + 1] == 1u << 3);   /* Pikachu's Meister flag, German */
    assert(Stat(general, SCORE) == 77 + 20);        /* the catch only: no GTS score in DP */
    assert(R16(general + HISTORY) == 25 && R16(general + HISTORY + 2) == 0 && R16(general + HISTORY + 4) == 0);
#else
    assert(general[DEX + 0x144 + 25] == 1u << 3);   /* caught languages, German */
    assert(general[DEX + 0x335] == 1);              /* another language than the cartridge's */
    assert(Stat(general, SCORE) == 77 + 20 + 40 && Stat(general, 25) == 4);
    assert(Aprijuice(3)[2] == 0);
#endif
    assert(WtGame_Received(&entry, &received) == 0 && received.species == 25);
    printf("PASS %s party delivery: placement, Pokémon, mail, Pokédex and records\n", FX_GAME_NAME);
}

/* A trade evolution, and the Everstone's hold effect of this game. */
static void Evolution(void)
{
    FxSpec specs[6];
    WtJournalEntry entry;
    Spec spec = { 64, 0, 0, 2, 0x2001 }, stone = { 93, EVERSTONE, 0, 2, 0x2002 }, other = { 93, OTHER_STONE, 0, 2, 0x2003 };
    unsigned char box[136], *general;
    Defaults(specs, 2); Setup(specs, 2);
    Match(&entry, &spec);
    Deliver(&entry);
    general = CpSaveImage_Block(&image, 0);
    assert(WtPokemon_Decode(FxImageParty(&image, 2), box) == 0 && R16(box + 8) == 65);
    assert(R16(box + 0x48) == kAlakazam[0] && R16(box + 0x48 + 16) == CP_CHAR_EOS);
    assert(DexBit(general, 4, 64) && DexBit(general, 4, 65));
#if DP
    /* Two catches, two Pokétch entries; no evolution record. */
    assert(Stat(general, SCORE) == 77 + 40);
    assert(R16(general + HISTORY) == 64 && R16(general + HISTORY + 4) == 65);
#else
    assert(Stat(general, SCORE) == 77 + 20 + 20 + 40);
#endif
    /* The Everstone stops Haunter; the other game's value does not. */
    Match(&entry, &stone); Deliver(&entry);
    assert(WtPokemon_Decode(FxImageParty(&image, 3), box) == 0 && R16(box + 8) == 93);
    Match(&entry, &other); Deliver(&entry);
    assert(WtPokemon_Decode(FxImageParty(&image, 4), box) == 0 && R16(box + 8) == 94);
    printf("PASS %s evolution: trade evolution, catches and the game's own Everstone\n", FX_GAME_NAME);
}

/* A full party sends the delivery to the PC: PP restored, and Shaymin back
 * to its Land Forme in HeartGold/SoulSilver only. */
static void PcDelivery(void)
{
    FxSpec specs[6];
    WtJournalEntry entry;
    Spec shaymin = { 492, 0, 1, 2, 0x3001 };
    unsigned char box[136];
    Defaults(specs, 6); Setup(specs, 6);
    Match(&entry, &shaymin);
    Deliver(&entry);
    assert(entry.destination.kind == 2 && entry.destination.box == 0 && entry.destination.slot == 0);
    assert(WtPokemon_Decode(FxImageBox(&image, 0, 0), box) == 0 && R16(box + 8) == 492);
    assert(box[0x30] == 33 % 40 + 5 + (33 % 40 + 5) * 20 * 2 / 100); /* PP with two PP Ups */
    assert((box[0x40] >> 3) == (DP ? 1u : 0u));
    assert(box[0x15] == (DP ? 32 : 30)); /* the ability as given, or the Land Forme's */
    printf("PASS %s PC delivery: first empty slot, PP, Shaymin's form\n", FX_GAME_NAME);
}

#if HGSS
/* Pichu's Pokédex forms, Jirachi's Pokéwalker course, the Unown caught order. */
static void HeartGoldEffects(void)
{
    FxSpec specs[6];
    WtJournalEntry entry;
    Spec jirachi = { 385, 0, 0, 2, 0x4001 }, pichu = { 172, 0, 1, 2, 0x4002 }, unown = { 201, 0, 5, 2, 0x4003 };
    unsigned char *general;
    Defaults(specs, 2); Setup(specs, 2);
    Match(&entry, &jirachi); Deliver(&entry);
    Match(&entry, &pichu); Deliver(&entry);
    Match(&entry, &unown); Deliver(&entry);
    general = CpSaveImage_Block(&image, 0);
    assert(Fx32(general + WALKER + 0x130) == (3u | 1u << 21));
    assert(general[DEX + 0x33E] == (0xFC | 2));   /* Spiky-eared: form entry 2 */
    assert(general[DEX + 0x10C] == 5 && general[DEX + 0x128] == 5 && general[DEX + 0x129] == 0xFF);
    assert(general[DEX + 0x335] == 0);           /* all English, as the cartridge */
    printf("PASS HeartGold/SoulSilver effects: Pokéwalker course, Pichu and Unown Pokédex forms\n");
}
#endif

/* The Pokédex's form history of every form species a delivery registers:
 * HeartGold/SoulSilver's Pokedex_TryAppendSeenForm records Deoxys, Rotom,
 * Shaymin and Giratina (at their own offsets); Diamond/Pearl's
 * Pokedex_SetMonSeenForm records Deoxys and keeps no others. */
static void FormHistory(void)
{
    FxSpec specs[6];
    WtJournalEntry entry;
    static const Spec forms[] = {
        { 386, 0, 2, 2, 0x7001 }, /* Deoxys Defense */
        { 386, 0, 2, 2, 0x7002 }, /* the same form again: no second entry */
        { 479, 0, 1, 2, 0x7003 }, /* Heat Rotom */
        { 492, 0, 0, 2, 0x7004 }, /* Land Shaymin */
        { 487, 0, 0, 2, 0x7005 }, /* Altered Giratina */
    };
    unsigned char *general, *dex, before[0x340];
    unsigned i;
    Defaults(specs, 1); Setup(specs, 1);
    memcpy(before, CpSaveImage_Block(&image, 0) + DEX, sizeof(before));
    for (i = 0; i < sizeof(forms) / sizeof(forms[0]); ++i) {
        Match(&entry, &forms[i]);
        Deliver(&entry);
    }
    general = CpSaveImage_Block(&image, 0);
    dex = general + DEX;
    /* Deoxys: the first nibble of the last caught word's top byte; the rest 15. */
    assert(dex[0x43] == 0xF2 && dex[0x83] == 0xFF);
#if HGSS
    assert(Fx32(dex + 0x338) == (0xFFFFFFFFu & ~7u) + 1); /* Rotom: entry 0 is form 1 */
    assert(dex[0x33C] == 0xFC && dex[0x33D] == 0xFC);    /* both entries form 0 */
    /* Giratina's Origin Forme next: the second entry. */
    {
        Spec origin = { 487, 0, 1, 2, 0x7006 };
        Match(&entry, &origin); Deliver(&entry);
        assert(CpSaveImage_Block(&image, 0)[DEX + 0x33D] == 0xFE);
    }
#else
    /* Nothing after the Meister flags changes, in the Pokédex or past it where
     * Platinum keeps Rotom (0x31C), Shaymin (0x320) and Giratina (0x321). */
    assert(memcmp(dex + 0x137, before + 0x137, sizeof(before) - 0x137) == 0);
#endif
    printf("PASS %s form history: Deoxys%s\n", FX_GAME_NAME, HGSS ? ", Rotom, Shaymin and Giratina" : " only");
}

/* A returned offer already back is never added twice; a full PC keeps it. */
static void Returns(void)
{
    FxSpec specs[6];
    WtJournalEntry entry;
    Defaults(specs, 3); Setup(specs, 3);
    memset(&entry, 0, sizeof(entry));
    assert(Step(WT_INVENTORY_PREPARE_REMOVE, &entry, 1) == 0);
    assert(Step(WT_INVENTORY_REMOVE, &entry, 1) == 0);
    entry.deliveryKind = WT_DELIVERY_RETURN;
    memcpy(entry.incoming, entry.outgoing, 136);
    Deliver(&entry);
    assert(FxImageCount(&image) == 3 && memcmp(FxImageParty(&image, 2), entry.original, FX_MON) == 0);
    Keep();
    assert(Step(WT_INVENTORY_PREPARE_RECEIVE, &entry, 0) == -1);
    Unchanged();
    printf("PASS %s return: back at the end of the party, never twice\n", FX_GAME_NAME);
}

/* ---- The stock game between companion sessions --------------------------- */

/* The unmodified game loads the file, the play time changes and it saves:
 * Diamond/Pearl a general-only save (the global counter stays, both block
 * counters advance in RAM, only the general block is written), HeartGold/
 * SoulSilver both blocks to the other half with the next counter. Written
 * from the decomps' save.c, not from save/save_image.c. */
static void StockSave(void)
{
    unsigned half = image.currentHalf[0], block;
    unsigned char *general;
    uint32_t global = image.globalCounter;
#if DP
    general = FxGeneral(&file, !half);
    memcpy(general, FxGeneral(&file, half), FX_GENERAL_SIZE);
    general[0x60 + 0x10] ^= 0x5a; /* PlayerData: the play time */
    FxSeal(&file, !half, 0, global, image.blockCounters[0] + 1);
    (void)block;
#else
    for (block = 0; block < 2; ++block) {
        unsigned char *from = block ? FxBoxes(&file, half) : FxGeneral(&file, half);
        unsigned char *to = block ? FxBoxes(&file, !half) : FxGeneral(&file, !half);
        memcpy(to, from, block ? FX_BOXES_SIZE : FX_GENERAL_SIZE);
    }
    general = FxGeneral(&file, !half);
    general[0x60 + 0x10] ^= 0x5a;
    FxSeal(&file, !half, 0, global + 1, 0);
    FxSeal(&file, !half, 1, global + 1, 0);
#endif
    assert(FxLoad(&image, &cpFile) == CP_SAVE_OK && image.currentHalf[0] == !half);
}

/* LUNE-899's rules on these games: the stock game saved over both recorded
 * inventories, and REMOVE and RECEIVE decide by the Pokémon itself. */
static void StockGame(void)
{
    FxSpec specs[6];
    WtJournalEntry entry;
    Spec spec = { 25, 0, 0, 2, 0x6001 };
    unsigned char digest[32], canonical[136];
    /* An offer prepared, then the stock game saved: removed from this save. */
    Defaults(specs, 4); Setup(specs, 4);
    memset(&entry, 0, sizeof(entry));
    assert(Step(WT_INVENTORY_PREPARE_REMOVE, &entry, 2) == 0);
    StockSave();
    assert(Step(WT_INVENTORY_REMOVE, &entry, 2) == 0);
    assert(FxImageCount(&image) == 3);
    assert(CpSaveImage_Hash(&image, digest) == CP_SAVE_OK && memcmp(digest, entry.inventoryAfter, 32) == 0);
    assert(entry.globalCounter == image.globalCounter);
    BothCopies();
    assert(Step(WT_INVENTORY_VERIFY_REMOVED, &entry, 2) == 0);
    /* A delivery prepared, then the stock game saved: received into this save, once. */
    Match(&entry, &spec);
    assert(Step(WT_INVENTORY_PREPARE_RECEIVE, &entry, 0) == 0);
    StockSave();
    assert(Step(WT_INVENTORY_RECEIVE, &entry, 0) == 0);
    assert(Step(WT_INVENTORY_VERIFY_RECEIVED, &entry, 0) == 0);
    assert(FxImageCount(&image) == 4);
    BothCopies();
    /* Applied, then the stock game saved before RECEIVE was recorded: only a
     * checkpoint, the record rebuilt from where the save holds it. */
    {
        WtJournalEntry recorded = entry;
        Spec second = { 26, 0, 0, 2, 0x6002 };
        Match(&entry, &second);
        assert(Step(WT_INVENTORY_PREPARE_RECEIVE, &entry, 0) == 0);
        recorded = entry;
        assert(Step(WT_INVENTORY_RECEIVE, &entry, 0) == 0);
        StockSave();
        assert(Step(WT_INVENTORY_RECEIVE, &recorded, 0) == 0);
        assert(FxImageCount(&image) == 5 && recorded.destination.kind == 1 && recorded.destination.slot == 4);
        assert(WtPokemon_Decode(FxImageParty(&image, 4), canonical) == 0 && R16(canonical + 8) == 26);
        assert(Step(WT_INVENTORY_VERIFY_RECEIVED, &recorded, 0) == 0);
    }
    printf("PASS %s stock game: removal and both receive recoveries decide by the Pokémon, once\n", FX_GAME_NAME);
}

static void Resum(unsigned char pk[136])
{
    unsigned i, sum = 0;
    for (i = 8; i < 136; i += 2) sum += R16(pk + i);
    FxPut16(pk + 6, sum & 65535);
}

static void CanHold(void)
{
    unsigned char pk[136];
    Spec spec = { 25, 0, 0, 2, 0x5001 };
    FxSpec specs[6];
    Defaults(specs, 2); Setup(specs, 2);
    Canonical(&spec, pk);
    assert(WtGame_CanHold(&game, pk));
    spec.species = 479; spec.form = 1; Canonical(&spec, pk);
    assert(WtGame_CanHold(&game, pk) == HGSS);   /* Heat Rotom is Platinum's */
    spec.species = 172; spec.form = 1; Canonical(&spec, pk);
    assert(WtGame_CanHold(&game, pk) == HGSS);   /* Spiky-eared Pichu */
    spec.species = 25; spec.form = 0; spec.item = DP ? 464 : 536; Canonical(&spec, pk);
    assert(WtGame_CanHold(&game, pk));
    spec.item = DP ? 465 : 537; Canonical(&spec, pk);
    assert(!WtGame_CanHold(&game, pk));
    spec.item = 0; Canonical(&spec, pk);
    pk[0x86] = 25; Resum(pk);
    assert(WtGame_CanHold(&game, pk));
    pk[0x86] = 26; Resum(pk);
    assert(WtGame_CanHold(&game, pk) == DP);     /* past the Park Ball */
    printf("PASS %s can hold: forms, last item, HeartGold/SoulSilver's ball\n", FX_GAME_NAME);
}

int main(void)
{
    Eligibility();
    RemoveAndRestore();
    Counters();
    PartyDelivery();
    Evolution();
    PcDelivery();
#if HGSS
    HeartGoldEffects();
#endif
    FormHistory();
    Returns();
    StockGame();
    CanHold();
    printf("PASS %s adapter\n", FX_GAME_NAME);
    return 0;
}
