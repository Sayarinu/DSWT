/* A matched delivery's receive effects (adapter/platinum_receive.c) and its
 * placement through WtGame_Inventory, against a fake game-data table and
 * synthetic saves. Expected values are worked out here by hand from the
 * game's formulas, not by calling the code under test. */
#include "stock_game.h"

#include <stdio.h>
#include <stdlib.h>

#include "game/text.h"
#include "journal_fixture.h"
#include "session/visit.h"
#include "wonder_trade/wire_layout.h"

/* ---- The fake game data ---------------------------------------------------- */

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
    { 292, 0, { 1, 90, 45, 40, 30, 30 }, 255, 1, { 25, 0 } },      /* Shedinja */
    { 81, 0, { 25, 35, 70, 45, 95, 55 }, 255, 0, { 42, 5 } },      /* Magnemite: genderless */
    { 113, 0, { 250, 5, 5, 50, 35, 105 }, 254, 4, { 30, 32 } },    /* Chansey: always female */
    { 32, 0, { 46, 57, 40, 50, 40, 40 }, 0, 3, { 38, 55 } },       /* Nidoran (male) */
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
    /* Any other species: base stats 50, a 50/50 ratio, two abilities. */
    memset(out->baseStats, 50, 6);
    out->genderRatio = 127; out->growthRate = 3; out->baseFriendship = 0;
    out->abilities[0] = (uint8_t)(species & 0x7F); out->abilities[1] = (uint8_t)((species & 0x7F) + 1);
    return CP_OK;
}

/* Every growth rate: level n needs n^3 experience, level 1 none. */
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

enum { EVERSTONE = 229, METAL_COAT = 233, HOLD_EFFECT_NO_EVOLVE = 64 };

static int FakeHoldEffect(void *arg, uint16_t item, uint8_t *out)
{
    (void)arg;
    if (item == EVERSTONE) { *out = HOLD_EFFECT_NO_EVOLVE; return CP_OK; }
    if (item == METAL_COAT || item == 1) { *out = 3; return CP_OK; }
    return CP_ERR_NOT_FOUND;
}

/* Move n has n % 40 + 5 base PP. */
static int FakeMovePp(void *arg, uint16_t move, uint8_t *out)
{
    (void)arg;
    *out = (uint8_t)(move % 40 + 5);
    return CP_OK;
}

static const uint16_t kAlakazam[] = { 0x12B, 0x136, 0x12B, 0x135, 0x12B, 0x144, 0x12B, 0x137, CP_CHAR_EOS };
static const uint16_t kTooLong[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, CP_CHAR_EOS };

static int FakeName(void *arg, uint16_t species, uint16_t *out, size_t capacity, size_t *length)
{
    const uint16_t *name = species == 65 ? kAlakazam : species == 94 ? kTooLong : NULL;
    size_t n = 0;
    (void)arg;
    if (!name) { static const uint16_t other[] = { 0x130, CP_CHAR_EOS }; name = other; }
    while (name[n] != CP_CHAR_EOS) ++n;
    if (n + 1 > capacity) return CP_ERR_RANGE;
    memcpy(out, name, (n + 1) * 2);
    *length = n;
    return CP_OK;
}

static CpAdapterData data = { NULL, FakePersonal, FakeExp, FakeEvolution, FakeHoldEffect, FakeMovePp, FakeName, 2, NULL };

/* ---- Pokémon and saves ----------------------------------------------------- */

enum { DEX = CP_RECEIVE_POKEDEX_OFFSET, RECORDS = CP_RECEIVE_RECORDS_OFFSET,
       HISTORY = CP_RECEIVE_POKETCH_OFFSET + 184, EVENTS = CP_RECEIVE_JOURNAL_OFFSET + 28 };

typedef struct Spec {
    unsigned species, item, form, language, nicknamed, otGender, egg;
    uint32_t pid, exp;
    unsigned ivs[6], evs[6], moves[4], pp[4], ups[4];
} Spec;

static unsigned R16(const unsigned char *p) { return p[0] | ((unsigned)p[1] << 8); }

static Spec Basic(unsigned species, uint32_t pid)
{
    Spec spec;
    memset(&spec, 0, sizeof(spec));
    spec.species = species; spec.pid = pid; spec.exp = 125000; spec.language = 2;
    spec.moves[0] = 33; spec.pp[0] = 10;
    return spec;
}

/* A canonical PK4: nickname "AB", OT "TRAINER", ball 4. */
static void Canonical(const Spec *spec, unsigned char out[136])
{
    unsigned i, sum = 0;
    uint32_t ivs = 0;
    memset(out, 0, 136);
    FxPut32(out, spec->pid);
    FxPut16(out + 0x08, spec->species); FxPut16(out + 0x0A, spec->item); FxPut32(out + 0x0C, 0x00010002);
    FxPut32(out + 0x10, spec->exp); out[0x14] = 5; out[0x15] = 99; out[0x17] = (unsigned char)spec->language;
    for (i = 0; i < 6; ++i) { out[0x18 + i] = (unsigned char)spec->evs[i]; ivs |= (uint32_t)spec->ivs[i] << (5 * i); }
    for (i = 0; i < 4; ++i) {
        FxPut16(out + 0x28 + 2 * i, spec->moves[i]); out[0x30 + i] = (unsigned char)spec->pp[i];
        out[0x34 + i] = (unsigned char)spec->ups[i];
    }
    ivs |= (uint32_t)(spec->egg & 1) << 30 | (uint32_t)(spec->nicknamed & 1) << 31;
    FxPut32(out + 0x38, ivs);
    out[0x40] = (unsigned char)(spec->form << 3);
    FxPut16(out + 0x48, 0x12B); FxPut16(out + 0x4A, 0x12C); FxPut16(out + 0x4C, 0xFFFF);
    for (i = 0; i < 7; ++i) FxPut16(out + 0x68 + 2 * i, 0x13E + i);
    FxPut16(out + 0x76, 0xFFFF);
    out[0x83] = 4; out[0x84] = (unsigned char)(10 | (spec->otGender << 7));
    for (i = 8; i < 136; i += 2) sum += R16(out + i);
    FxPut16(out + 6, sum & 65535);
}

/* Pokedex_Init, an empty Pokétch history and journal page, encoded records. */
static void FreshGeneral(unsigned char *general)
{
    unsigned char *dex = general + DEX, *records = general + RECORDS;
    memset(dex, 0, 804);
    FxPut32(dex, 0xBEEFCAFE);
    memset(dex + 268, 0xFF, 28);
    dex[264] = dex[265] = dex[266] = dex[267] = 0xFF;
    FxPut32(dex + 796, 0xFFFFFFFF); dex[800] = dex[801] = 0xFF;
    dex[67] = 0xFF; dex[131] = 0xFF; /* Deoxys: four unused 15s */
    memset(records, 0, 444);
    FxPut16(records + 442, 0x1234);   /* the seed's modifier */
    FxPut32(records + 4, 77);         /* a trainer score */
    FxPut32(records + 4 * 24, 3);     /* Wi-Fi trades */
    {
        unsigned sum = 0, i;
        for (i = 4; i < 440; ++i) sum += records[i];
        FxPut16(records + 440, sum & 65535);
        FxCrypt(records + 4, 436, (sum & 65535) | (0x1234u << 16));
    }
    memset(general + HISTORY, 0, 96);
    memset(general + CP_RECEIVE_JOURNAL_OFFSET, 0, 112);
    FxPut16(general + CP_RECEIVE_VARS_OFFSET + 2 * 86, 0);
}

static uint32_t Record(const unsigned char *general, unsigned id)
{
    unsigned char plain[444];
    memcpy(plain, general + RECORDS, sizeof(plain));
    FxCrypt(plain + 4, 436, R16(plain + 440) | ((uint32_t)R16(plain + 442) << 16));
    return Fx32(plain + 4 * id);
}

static int DexBit(const unsigned char *general, unsigned offset, unsigned species)
{
    return (general[DEX + offset + (species - 1) / 8] >> ((species - 1) % 8)) & 1;
}

static unsigned char general[FX_GENERAL_SIZE];

/* Apply to `general` and decode the result: canonical and plain party extension. */
static int Receive(const Spec *spec, int destination, const uint16_t *kept, unsigned char box[136], unsigned char party[100])
{
    unsigned char incoming[136], image[236];
    Canonical(spec, incoming);
    if (CpReceive_Apply(&data, general, incoming, destination, kept, image) != 0) return -1;
    assert(WtPokemon_Decode(image, box) == 0);
    if (party) { memcpy(party, image + 136, 100); FxCrypt(party, 100, spec->pid); }
    return 0;
}

/* ---- Tests ------------------------------------------------------------------ */

/* 0x80000011: nature 15 (Modest: +Sp. Atk, -Attack), odd, low byte 17. */
#define MODEST_ODD_LOW 0x80000011u

static void Stats(void)
{
    Spec spec = Basic(64, MODEST_ODD_LOW);
    unsigned char box[136], party[100];
    unsigned i;
    static const unsigned ivs[6] = { 31, 0, 15, 20, 31, 10 }, evs[6] = { 4, 0, 0, 252, 252, 0 };
    memcpy(spec.ivs, ivs, sizeof(ivs)); memcpy(spec.evs, evs, sizeof(evs));
    spec.item = EVERSTONE; /* Kadabra evolves anyway: see Evolution */
    Canonical(&spec, box);
    memset(party, 0, sizeof(party));
    assert(CpReceive_LevelAndStats(&data, box, party) == 0);
    /* Level 50 (125000 = 50^3). HP (2*40+31+4/4)*50/100+50+10; Attack (70+0)*50/100+5 = 40,
     * Modest's 90% is 36; Sp. Atk (240+31+63)*50/100+5 = 172, 110% is 189. */
    assert(party[4] == 50 && R16(party + 8) == 116 && R16(party + 6) == 116);
    assert(R16(party + 10) == 36 && R16(party + 12) == 42 && R16(party + 14) == 151);
    assert(R16(party + 16) == 189 && R16(party + 18) == 80);
    /* A wounded Pokémon keeps its damage when its maximum HP changes. */
    FxPut16(party + 6, 50); FxPut16(party + 8, 60);
    assert(CpReceive_LevelAndStats(&data, box, party) == 0 && R16(party + 6) == 50 + 116 - 60);
    /* A fainted one stays fainted. */
    FxPut16(party + 6, 0);
    assert(CpReceive_LevelAndStats(&data, box, party) == 0 && R16(party + 6) == 0);
    /* Every neutral nature leaves the stats; the others move exactly two. */
    for (i = 0; i < 25; ++i) {
        unsigned up = i / 5, down = i % 5, base[5] = { 40, 42, 151, 172, 80 }, k;
        spec.pid = (0x80000000u / 25) * 25 + i; /* nature i */
        Canonical(&spec, box); memset(party, 0, sizeof(party));
        assert(CpReceive_LevelAndStats(&data, box, party) == 0);
        for (k = 0; k < 5; ++k) {
            unsigned want = base[k];
            if (up != down && k == up) want = (uint16_t)(want * 110) / 100;
            if (up != down && k == down) want = (uint16_t)(want * 90) / 100;
            assert(R16(party + 10 + 2 * k) == want);
        }
    }
    /* Shedinja: always 1 HP. */
    spec = Basic(292, 2); Canonical(&spec, box); memset(party, 0, sizeof(party));
    assert(CpReceive_LevelAndStats(&data, box, party) == 0 && R16(party + 8) == 1 && R16(party + 6) == 1);
    FxPut16(party + 6, 0);
    assert(CpReceive_LevelAndStats(&data, box, party) == 0 && R16(party + 6) == 0);
    puts("PASS stats: formula, natures, HP carried over, Shedinja");
}

static void Levels(void)
{
    static const struct { uint32_t exp; unsigned level; } cases[] = {
        { 0, 1 }, { 7, 1 }, { 8, 2 }, { 26, 2 }, { 27, 3 }, { 124999, 49 }, { 125000, 50 },
        { 999999, 99 }, { 1000000, 100 }, { 0xFFFFFFFFu, 100 }
    };
    unsigned i;
    unsigned char box[136], party[100];
    Spec spec = Basic(25, 4);
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        spec.exp = cases[i].exp; Canonical(&spec, box); memset(party, 0, sizeof(party));
        assert(CpReceive_LevelAndStats(&data, box, party) == 0 && party[4] == cases[i].level);
    }
    puts("PASS levels: boundaries from the experience table");
}

static void Genders(void)
{
    assert(CpReceive_Gender(0, 0) == 0 && CpReceive_Gender(0, 0xFF) == 0);
    assert(CpReceive_Gender(254, 0xFF) == 1 && CpReceive_Gender(254, 0) == 1);
    assert(CpReceive_Gender(255, 0) == 2 && CpReceive_Gender(255, 0x7F) == 2);
    assert(CpReceive_Gender(127, 126) == 1 && CpReceive_Gender(127, 127) == 0);
    assert(CpReceive_Gender(31, 0x1234561E) == 1 && CpReceive_Gender(31, 0x1F) == 0);
    assert(CpReceive_Gender(191, 190) == 1 && CpReceive_Gender(191, 0xFFFFFFBF) == 0);
    /* The received Pokémon stores the computed gender in block B. */
    {
        Spec spec = Basic(113, 0xFF); unsigned char box[136];
        FreshGeneral(general);
        assert(Receive(&spec, CP_RECEIVE_PARTY, NULL, box, NULL) == 0 && ((box[0x40] >> 1) & 3) == 1);
        spec = Basic(81, 0); assert(Receive(&spec, CP_RECEIVE_PARTY, NULL, box, NULL) == 0 && ((box[0x40] >> 1) & 3) == 2);
        spec = Basic(32, 0); assert(Receive(&spec, CP_RECEIVE_PARTY, NULL, box, NULL) == 0 && ((box[0x40] >> 1) & 3) == 0);
    }
    puts("PASS genders: thresholds, always male, always female, genderless");
}

static void Evolution(void)
{
    unsigned char box[136], party[100];
    Spec spec;
    static const unsigned ivs[6] = { 31, 0, 15, 20, 31, 10 }, evs[6] = { 4, 0, 0, 252, 252, 0 };

    /* Kadabra evolves even holding an Everstone, and keeps the Everstone. */
    FreshGeneral(general);
    spec = Basic(64, MODEST_ODD_LOW); spec.item = EVERSTONE;
    memcpy(spec.ivs, ivs, sizeof(ivs)); memcpy(spec.evs, evs, sizeof(evs));
    assert(Receive(&spec, CP_RECEIVE_PARTY, NULL, box, party) == 0);
    assert(R16(box + 0x08) == 65 && R16(box + 0x0A) == EVERSTONE);
    /* Alakazam's stats; the HP rose with the maximum (116 -> 131). */
    assert(party[4] == 50 && R16(party + 8) == 131 && R16(party + 6) == 131);
    assert(R16(party + 10) == 49 && R16(party + 12) == 57 && R16(party + 14) == 166);
    assert(R16(party + 16) == 205 && R16(party + 18) == 95);
    assert(box[0x15] == 39);  /* odd personality: the second ability */
    assert(box[0x14] == 70);  /* BASE_FRIENDSHIP_VALUE */
    /* Not nicknamed: the species name in the cartridge's language, terminated;
     * the rest of the nickname stays as it was. */
    assert(memcmp(box + 0x48, kAlakazam, sizeof(kAlakazam)) == 0);
    /* Both species are caught; records and score: 77 + 20 (new species) + 20 (evolution) + 40. */
    assert(DexBit(general, 4, 64) && DexBit(general, 4, 65) && DexBit(general, 68, 65));
    assert(Record(general, 12) == 1 && Record(general, 24) == 4 && Record(general, 1) == 77 + 20 + 20 + 40);
    /* Pokétch history: Kadabra then Alakazam. */
    assert(R16(general + HISTORY) == 64 && R16(general + HISTORY + 8) == 65 && R16(general + HISTORY + 16) == 0);

    /* Kadabra without an item, even personality, nicknamed: first ability, nickname kept. */
    FreshGeneral(general);
    spec = Basic(64, 0x40000042u); spec.nicknamed = 1;
    assert(Receive(&spec, CP_RECEIVE_PARTY, NULL, box, party) == 0);
    assert(R16(box + 0x08) == 65 && box[0x15] == 28 && R16(box + 0x48) == 0x12B && R16(box + 0x4A) == 0x12C);

    /* Haunter holding an Everstone stays a Haunter, with its ability untouched. */
    FreshGeneral(general);
    spec = Basic(93, 1); spec.item = EVERSTONE;
    assert(Receive(&spec, CP_RECEIVE_PARTY, NULL, box, party) == 0);
    assert(R16(box + 0x08) == 93 && box[0x15] == 99 && R16(box + 0x0A) == EVERSTONE);
    assert(Record(general, 12) == 0 && Record(general, 1) == 77 + 20 + 40 && !DexBit(general, 4, 94));
    /* Haunter without one becomes a Gengar; with one ability, odd personalities get it too.
     * Its name would not fit, so the nickname is left (the game asserts). */
    FreshGeneral(general);
    spec = Basic(93, 1);
    assert(Receive(&spec, CP_RECEIVE_PARTY, NULL, box, party) == 0);
    assert(R16(box + 0x08) == 94 && box[0x15] == 26 && R16(box + 0x48) == 0x12B);

    /* Onix with a Metal Coat becomes Steelix and uses up the coat; without it, nothing. */
    FreshGeneral(general);
    spec = Basic(95, 6); spec.item = METAL_COAT; spec.exp = 1000;
    assert(Receive(&spec, CP_RECEIVE_PARTY, NULL, box, party) == 0);
    assert(R16(box + 0x08) == 208 && R16(box + 0x0A) == 0 && box[0x15] == 69);
    spec.item = 1;
    assert(Receive(&spec, CP_RECEIVE_PARTY, NULL, box, party) == 0);
    assert(R16(box + 0x08) == 95 && R16(box + 0x0A) == 1);
    puts("PASS evolution: Everstone except Kadabra, held items used up, ability, nickname, records");
}

static void Pokedex(void)
{
    unsigned char *dex = general + DEX, box[136];
    Spec spec;

    /* First catch of a female: both gender entries, the language, caught and seen. */
    FreshGeneral(general);
    spec = Basic(64 + 100, 0x10); /* species 164, ratio 127, low byte 16: female */
    spec.language = 5;            /* German: language index 3 */
    assert(Receive(&spec, CP_RECEIVE_BOX, NULL, box, NULL) == 0);
    assert(DexBit(general, 4, 164) && DexBit(general, 68, 164) && DexBit(general, 132, 164) && DexBit(general, 196, 164));
    assert(dex[296 + 164] == 1u << 3);
    /* Already seen as female: a male sets only the second entry. */
    spec.pid = 0x80; spec.language = 8; /* Korean: not recorded */
    assert(Receive(&spec, CP_RECEIVE_BOX, NULL, box, NULL) == 0);
    assert(DexBit(general, 132, 164) == 1 && DexBit(general, 196, 164) == 0 && dex[296 + 164] == 1u << 3);
    /* A score for a new species only. */
    assert(Record(general, 1) == 77 + 20 + 40 + 40);

    /* Genderless records as male. */
    FreshGeneral(general);
    spec = Basic(81, 0xFFFF); spec.language = 2;
    assert(Receive(&spec, CP_RECEIVE_BOX, NULL, box, NULL) == 0);
    assert(!DexBit(general, 132, 81) && !DexBit(general, 196, 81) && dex[296 + 81] == 2);

    /* Spinda: its personality on first sight only. */
    FreshGeneral(general);
    spec = Basic(327, 0x11223344);
    assert(Receive(&spec, CP_RECEIVE_BOX, NULL, box, NULL) == 0 && Fx32(dex + 260) == 0x11223344);
    spec.pid = 0x55667788;
    assert(Receive(&spec, CP_RECEIVE_BOX, NULL, box, NULL) == 0 && Fx32(dex + 260) == 0x11223344);

    /* Unown: forms in the order first seen. */
    FreshGeneral(general);
    spec = Basic(201, 2); spec.form = 7;
    assert(Receive(&spec, CP_RECEIVE_BOX, NULL, box, NULL) == 0);
    spec.form = 3; assert(Receive(&spec, CP_RECEIVE_BOX, NULL, box, NULL) == 0);
    spec.form = 7; assert(Receive(&spec, CP_RECEIVE_BOX, NULL, box, NULL) == 0);
    assert(dex[268] == 7 && dex[269] == 3 && dex[270] == 0xFF);

    /* Shellos: the first form fills both entries; a second fills the other. */
    FreshGeneral(general);
    spec = Basic(422, 2); spec.form = 0;
    assert(Receive(&spec, CP_RECEIVE_BOX, NULL, box, NULL) == 0 && dex[264] == 0xFC);
    spec.form = 1; assert(Receive(&spec, CP_RECEIVE_BOX, NULL, box, NULL) == 0 && dex[264] == 0xFE);
    spec.form = 0; assert(Receive(&spec, CP_RECEIVE_BOX, NULL, box, NULL) == 0 && dex[264] == 0xFE);

    /* Burmy: three 2-bit entries. */
    FreshGeneral(general);
    spec = Basic(412, 2); spec.form = 2;
    assert(Receive(&spec, CP_RECEIVE_BOX, NULL, box, NULL) == 0 && dex[266] == 0xFE);
    spec.form = 1; assert(Receive(&spec, CP_RECEIVE_BOX, NULL, box, NULL) == 0 && dex[266] == 0xF6);
    spec.form = 2; assert(Receive(&spec, CP_RECEIVE_BOX, NULL, box, NULL) == 0 && dex[266] == 0xF6);

    /* Deoxys: nibbles in the top bytes of the last caught and seen words. */
    FreshGeneral(general);
    spec = Basic(386, 2); spec.form = 3;
    assert(Receive(&spec, CP_RECEIVE_BOX, NULL, box, NULL) == 0 && dex[67] == 0xF3 && dex[131] == 0xFF);
    spec.form = 1; assert(Receive(&spec, CP_RECEIVE_BOX, NULL, box, NULL) == 0 && dex[67] == 0x13);
    spec.form = 2; assert(Receive(&spec, CP_RECEIVE_BOX, NULL, box, NULL) == 0 && dex[131] == 0xF2);
    spec.form = 0; assert(Receive(&spec, CP_RECEIVE_BOX, NULL, box, NULL) == 0 && dex[131] == 0x02);
    spec.form = 1; assert(Receive(&spec, CP_RECEIVE_BOX, NULL, box, NULL) == 0 && dex[67] == 0x13 && dex[131] == 0x02);
    /* Deoxys is also caught: bit 385 of the caught words, below the forms. */
    assert(DexBit(general, 4, 386));

    /* Rotom: 3-bit entries. */
    FreshGeneral(general);
    spec = Basic(479, 2); spec.form = 5;
    assert(Receive(&spec, CP_RECEIVE_BOX, NULL, box, NULL) == 0 && Fx32(dex + 796) == 0xFFFFFFFDu);
    spec.form = 5; assert(Receive(&spec, CP_RECEIVE_BOX, NULL, box, NULL) == 0 && Fx32(dex + 796) == 0xFFFFFFFDu);
    spec.form = 2; assert(Receive(&spec, CP_RECEIVE_BOX, NULL, box, NULL) == 0 && Fx32(dex + 796) == 0xFFFFFFD5u);

    /* Giratina's Origin Forme, first seen: both entries. */
    FreshGeneral(general);
    spec = Basic(487, 2); spec.form = 1;
    assert(Receive(&spec, CP_RECEIVE_BOX, NULL, box, NULL) == 0 && dex[801] == 0xFF);

    /* A Pokédex the game never initialised refuses the delivery. */
    FreshGeneral(general); FxPut32(dex, 0);
    spec = Basic(25, 2);
    assert(Receive(&spec, CP_RECEIVE_BOX, NULL, box, NULL) == -1);
    puts("PASS pokedex: first catch, seen before, genders, languages, Spinda, Unown, two/three forms, Deoxys, Rotom");
}

static void JournalAndArceus(void)
{
    unsigned char box[136], *event;
    Spec spec;
    unsigned i;
    FreshGeneral(general);
    spec = Basic(25, 0x10); spec.otGender = 1; /* female OT, female Pokémon */
    assert(Receive(&spec, CP_RECEIVE_PARTY, NULL, box, NULL) == 0);
    event = general + EVENTS;
    assert(event[0] == 7 && event[1] == ((1 << 2) | 1));
    for (i = 0; i < 7; ++i) assert(R16(event + 2 + 2 * i) == 0x13E + i);
    assert(R16(event + 16) == 0xFFFF);
    /* The nickname "AB", then the terminator filling the rest. */
    assert(R16(event + 18) == 0x12B && R16(event + 20) == 0x12C);
    for (i = 2; i < 12; ++i) assert(R16(event + 18 + 2 * i) == 0xFFFF);
    /* A second event takes the second slot; a third pushes the first out. */
    spec.species = 26; assert(Receive(&spec, CP_RECEIVE_PARTY, NULL, box, NULL) == 0);
    assert(event[42] == 7);
    memset(event + 42 + 18, 0x55, 2);
    spec.species = 27; assert(Receive(&spec, CP_RECEIVE_PARTY, NULL, box, NULL) == 0);
    assert(event[0] == 7 && R16(event + 18) == 0x5555 && event[42] == 7 && R16(event + 42 + 18) == 0x12B);
    assert(R16(general + CP_RECEIVE_VARS_OFFSET + 2 * 86) == 0);
    /* Arceus: the event flag for a fateful encounter or one met at the Hall of Origin. */
    spec = Basic(493, 2);
    assert(Receive(&spec, CP_RECEIVE_PARTY, NULL, box, NULL) == 0 && R16(general + CP_RECEIVE_VARS_OFFSET + 172) == 0);
    {
        unsigned char incoming[136], image[236];
        unsigned sum = 0;
        Canonical(&spec, incoming); FxPut16(incoming + 0x80, 86);
        for (i = 8; i < 136; i += 2) sum += R16(incoming + i);
        FxPut16(incoming + 6, sum & 65535);
        assert(CpReceive_Apply(&data, general, incoming, CP_RECEIVE_PARTY, NULL, image) == 0);
        assert(R16(general + CP_RECEIVE_VARS_OFFSET + 172) == 1);
    }
    puts("PASS journal and Arceus: trade event bytes, slots, the Arceus event flag");
}

static void MailAndPc(void)
{
    unsigned char box[136], party[100], mail[56];
    static const uint16_t kept[3] = { 0x1111, 0x2222, 0x3333 };
    Spec spec;
    unsigned i;
    /* Mail_Reset after zeroing: language and version, type 0xFF, empty names and icons. */
    FreshGeneral(general);
    spec = Basic(25, 2);
    assert(Receive(&spec, CP_RECEIVE_PARTY, NULL, box, party) == 0);
    memset(mail, 0, sizeof(mail));
    mail[5] = 2; mail[6] = 12; mail[7] = 0xFF;
    for (i = 8; i < 30; ++i) mail[i] = 0xFF;
    for (i = 0; i < 3; ++i) { FxPut16(mail + 32 + 8 * i, 0xFFFF); FxPut16(mail + 36 + 8 * i, 0xFFFF); FxPut16(mail + 38 + 8 * i, 0xFFFF); }
    assert(memcmp(party + 20, mail, sizeof(mail)) == 0);
    /* Status, capsule and seals are clear. */
    for (i = 0; i < 4; ++i) assert(party[i] == 0);
    assert(party[5] == 0);
    for (i = 76; i < 100; ++i) assert(party[i] == 0);
    /* When applied, the prepared image's sentence IDs are kept. */
    assert(Receive(&spec, CP_RECEIVE_PARTY, kept, box, party) == 0);
    for (i = 0; i < 3; ++i) FxPut16(mail + 34 + 8 * i, kept[i]);
    assert(memcmp(party + 20, mail, sizeof(mail)) == 0);

    /* The PC restores PP (with PP Ups) and makes Shaymin the Land Forme. */
    spec = Basic(492, 3); spec.form = 1;
    spec.moves[0] = 33; spec.pp[0] = 1; spec.ups[0] = 3;   /* 38 PP, +60% = 60 */
    spec.moves[1] = 10; spec.pp[1] = 0; spec.ups[1] = 1;   /* 15 PP, +20% = 18 */
    spec.moves[2] = 0;  spec.pp[2] = 7;                    /* no move: left */
    spec.moves[3] = 7;  spec.pp[3] = 0; spec.ups[3] = 5;   /* PP Ups past 3 count as 3: 12 -> 19 */
    assert(Receive(&spec, CP_RECEIVE_BOX, NULL, box, NULL) == 0);
    assert(box[0x30] == 60 && box[0x31] == 18 && box[0x32] == 7 && box[0x33] == 19);
    assert((box[0x40] >> 3) == 0 && box[0x15] == 30);
    /* In the party it keeps its PP and its form. */
    assert(Receive(&spec, CP_RECEIVE_PARTY, NULL, box, NULL) == 0);
    assert(box[0x30] == 1 && (box[0x40] >> 3) == 1 && box[0x15] == 99);
    puts("PASS mail and PC: reset mail bytes, kept sentence IDs, PP restored, Shaymin Land Forme");
}

/* ---- Placement through WtGame_Inventory ------------------------------------ */

static FxFile file;
static CpSaveImage image;
static CpFile cpFile = { &file, FxRead, FxWrite, FxSync, FxSize };
static WtGame game;

static void Setup(unsigned count)
{
    FxSpec specs[6];
    unsigned half, i;
    for (i = 0; i < 6; ++i) specs[i] = FxDefault(i);
    FxBuild(&file, specs, count);
    for (half = 0; half < 2; ++half) {
        FreshGeneral(FxGeneral(&file, half));
        FxSeal(&file, half, 0, 10 + half, 20 + half);
    }
    assert(CpSaveImage_Load(&image, &cpFile, CP_SAVE_MAGIC_INTERNATIONAL) == CP_SAVE_OK);
    memset(&game, 0, sizeof(game));
    game.image = &image; game.file = &cpFile; game.data = &data;
}

/* An entry as the engine leaves it after Collect: the escrow of party slot 0 of
 * an earlier save, and a matched Pokémon. */
static void Entry(WtJournalEntry *entry, const Spec *incoming)
{
    FxSpec offered = FxDefault(9);
    memset(entry, 0, sizeof(*entry));
    entry->source.kind = 1; entry->source.length = 236;
    FxPartyMon(&offered, entry->original);
    assert(WtPokemon_Decode(entry->original, entry->outgoing) == 0);
    entry->deliveryKind = WT_DELIVERY_MATCH;
    Canonical(incoming, entry->incoming);
}

static void Placement(void)
{
    WtJournalEntry entry, copy;
    WtReceivedPokemon received;
    Spec spec = Basic(64, MODEST_ODD_LOW);
    static unsigned char before[CP_SAVE_FILE_SIZE];
    unsigned char digest[32], canonical[136], *destination;
    unsigned box, slot;

    /* Party of five: the sixth slot. PREPARE changes nothing. */
    Setup(5); Entry(&entry, &spec);
    memcpy(before, file.bytes, sizeof(before));
    assert(WtGame_Inventory(&game, WT_INVENTORY_PREPARE_RECEIVE, &entry, 0) == 0);
    assert(memcmp(file.bytes, before, sizeof(before)) == 0 && FxImageCount(&image) == 5);
    assert(entry.destination.kind == 1 && entry.destination.slot == 5 && entry.destination.length == 236);
    assert(memcmp(entry.destinationBefore, FxImageParty(&image, 5), 236) == 0);
    assert(CpSaveImage_Hash(&image, digest) == CP_SAVE_OK && memcmp(digest, entry.inventoryBefore, 32) == 0);
    assert(WtPokemon_Decode(entry.destinationAfter, canonical) == 0);
    FxSha(canonical, 136, digest);
    assert(memcmp(digest, entry.appliedDigest, 32) == 0);
    copy = entry;
    assert(WtGame_Inventory(&game, WT_INVENTORY_RECEIVE, &entry, 0) == 0);
    assert(FxImageCount(&image) == 6 && memcmp(FxImageParty(&image, 5), entry.destinationAfter, 236) == 0);
    assert(CpSaveImage_Hash(&image, digest) == CP_SAVE_OK && memcmp(digest, entry.inventoryAfter, 32) == 0);
    /* The effects reached the general block: Kadabra and Alakazam caught. */
    assert(DexBit(CpSaveImage_Block(&image, 0), 4, 64) && DexBit(CpSaveImage_Block(&image, 0), 4, 65));
    assert(CpSaveImage_Load(&image, &cpFile, CP_SAVE_MAGIC_INTERNATIONAL) == CP_SAVE_OK);
    assert(WtGame_Inventory(&game, WT_INVENTORY_VERIFY_RECEIVED, &entry, 0) == 0);
    /* A replayed RECEIVE finds it applied. */
    assert(WtGame_Inventory(&game, WT_INVENTORY_RECEIVE, &copy, 0) == 0);
    assert(WtGame_Received(&entry, &received) == 0 && received.species == 65 && received.gender == 1);
    assert(received.personality == MODEST_ODD_LOW && !received.hasNickname && received.nickname[0] == 0x12B);
    assert(received.nickname[10] == 0xFFFF && received.originalTrainer[0] == 0x13E && received.originalTrainer[7] == 0xFFFF);
    /* 0x0001 ^ 0x0002 ^ 0x8000 ^ 0x0011 is not below 8: not shiny. */
    assert(!received.shiny);
    entry.deliveryKind = WT_DELIVERY_RETURN;
    assert(WtGame_Received(&entry, &received) == -1);
    puts("PASS placement: a matched Pokémon at the end of the party, applied, verified, shown");

    /* Applying the prepared delivery on another device rebuilds the same bytes,
     * keeping the prepared mail sentence IDs. */
    Setup(5); Entry(&entry, &spec);
    assert(WtGame_Inventory(&game, WT_INVENTORY_PREPARE_RECEIVE, &entry, 0) == 0);
    {
        unsigned char extension[100];
        uint32_t pid = Fx32(entry.destinationAfter);
        memcpy(extension, entry.destinationAfter + 136, 100); FxCrypt(extension, 100, pid);
        FxPut16(extension + 54, 0x0BAD); FxCrypt(extension, 100, pid);
        memcpy(entry.destinationAfter + 136, extension, 100);
    }
    assert(WtGame_Inventory(&game, WT_INVENTORY_RECEIVE, &entry, 0) == -1); /* the hash no longer matches */
    puts("PASS placement: the applied image must match the prepared one");

    /* Full party: the first empty PC slot. */
    Setup(6); Entry(&entry, &spec);
    {
        FxSpec stored = FxDefault(50);
        FxCanonical(&stored, canonical);
        assert(WtPokemon_Encode(canonical, FxImageBox(&image, 0, 0)) == 0);
    }
    assert(WtGame_Inventory(&game, WT_INVENTORY_PREPARE_RECEIVE, &entry, 0) == 0);
    assert(entry.destination.kind == 2 && entry.destination.box == 0 && entry.destination.slot == 1 && entry.destination.length == 136);
    assert(WtGame_Inventory(&game, WT_INVENTORY_RECEIVE, &entry, 0) == 0);
    destination = FxImageBox(&image, 0, 1);
    assert(memcmp(destination, entry.destinationAfter, 136) == 0 && FxImageCount(&image) == 6);
    assert(WtPokemon_Decode(destination, canonical) == 0 && Fx32(canonical) == MODEST_ODD_LOW);
    assert(WtGame_Inventory(&game, WT_INVENTORY_VERIFY_RECEIVED, &entry, 0) == 0);
    puts("PASS placement: a full party sends it to the first empty PC slot");

    /* No room: 1, and nothing changes. */
    Setup(6); Entry(&entry, &spec);
    for (box = 0; box < 18; ++box) for (slot = 0; slot < 30; ++slot) {
        FxSpec filler = FxDefault(100 + box * 30 + slot);
        FxCanonical(&filler, canonical);
        assert(WtPokemon_Encode(canonical, FxImageBox(&image, box, slot)) == 0);
    }
    memcpy(before, image.bytes, sizeof(before));
    assert(WtGame_Inventory(&game, WT_INVENTORY_PREPARE_RECEIVE, &entry, 0) == 1);
    assert(memcmp(before, image.bytes, sizeof(before)) == 0);
    puts("PASS placement: no room keeps the delivery pending");

    /* The offer still in the save: a matched delivery is refused and nothing
     * changes, then prepared once the offer is gone. */
    Setup(5); Entry(&entry, &spec);
    assert(WtPokemon_Encode(entry.outgoing, FxImageBox(&image, 4, 4)) == 0);
    memcpy(before, image.bytes, sizeof(before));
    assert(WtGame_Inventory(&game, WT_INVENTORY_PREPARE_RECEIVE, &entry, 0) == -1);
    assert(memcmp(before, image.bytes, sizeof(before)) == 0);
    FxEmpty(FxImageBox(&image, 4, 4), 136);
    assert(WtGame_Inventory(&game, WT_INVENTORY_PREPARE_RECEIVE, &entry, 0) == 0);
    puts("PASS placement: a matched delivery waits while its offer is still in the save");

    /* Without game data a matched delivery is refused and nothing changes. */
    Setup(5); Entry(&entry, &spec); game.data = NULL;
    memcpy(before, image.bytes, sizeof(before));
    assert(WtGame_Inventory(&game, WT_INVENTORY_PREPARE_RECEIVE, &entry, 0) == -1);
    assert(memcmp(before, image.bytes, sizeof(before)) == 0);
    puts("PASS placement: no game data, no matched delivery");
}

/* ---- LUNE-899: the stock game saved before an applying delivery resumed ---- */

static void AddPartyMember(unsigned char *general, unsigned char *boxes)
{
    FxSpec extra = FxDefault(20);
    unsigned count = Fx32(general + FX_PARTY + 4);
    (void)boxes;
    FxPartyMon(&extra, general + FX_PARTY + 8 + count * FX_MON);
    FxPut32(general + FX_PARTY + 4, count + 1);
}

static void FillPc(unsigned char *general, unsigned char *boxes)
{
    unsigned i;
    (void)general;
    for (i = 0; i < 18 * 30; ++i) {
        FxSpec filler = FxDefault(100 + i);
        unsigned char canonical[136];
        FxCanonical(&filler, canonical);
        assert(WtPokemon_Encode(canonical, boxes + FX_PC + i * FX_BOX_MON) == 0);
    }
}

/* A matched delivery prepared and recorded APPLYING, the RECEIVE cut at its
 * `cut`th write (13: finished, the journal not yet APPLIED). */
static void Applying(unsigned party, const Spec *spec, WtJournalEntry *journal, unsigned cut)
{
    WtJournalEntry entry;
    Setup(party); Entry(journal, spec);
    journal->globalCounter = image.globalCounter;
    journal->generalCounter = image.blockCounters[0]; journal->boxCounter = image.blockCounters[1];
    assert(WtGame_Inventory(&game, WT_INVENTORY_PREPARE_RECEIVE, journal, 0) == 0);
    entry = *journal;
    file.failWrite = file.writes + cut;
    assert(WtGame_Inventory(&game, WT_INVENTORY_RECEIVE, &entry, 0) == (cut <= 12 ? -1 : 0));
    file.failWrite = 0;
}

/* LUNE-906: what the stock-game player does with the delivery, when the save
 * the game loaded holds it, before saving: keep it, release it, or trade it away. */
static unsigned char letGo[136];
static int letGoTrade;
static void LetGo(unsigned char *general, unsigned char *boxes) { (void)FxGone(general, boxes, letGo, letGoTrade); }

/* RECEIVE cut at every write, then the stock game: the delivery is never
 * added again once the stock game saved over it (client wonder_trade/
 * delivery.h, the games' never-duplicate rule). Kept, it is there once when
 * the save the game loaded held it; released or traded away, it stays gone;
 * a cut before the first retail copy held it loses it (the games' window). */
static void StockSave(void)
{
    static const char *const fates[] = { "kept", "released", "traded away" };
    static unsigned char resumed[CP_SAVE_FILE_SIZE];
    WtJournalEntry journal, entry;
    Spec spec = Basic(64, MODEST_ODD_LOW);
    unsigned char incoming[136];
    unsigned party, cut, saves, fate, held, cases, lost;
    Canonical(&spec, incoming);
    memcpy(letGo, incoming, sizeof(letGo));
    for (fate = 0; fate < 3; ++fate) {
        cases = lost = 0;
        letGoTrade = fate == 2;
        for (party = 5; party <= 6; ++party) for (cut = 1; cut <= 13; ++cut)
        for (saves = 1; saves <= 2; ++saves) {
            unsigned full = saves == 2;
            Applying(party, &spec, &journal, cut);
            held = FxStockCopies(&file, incoming);
            /* Held once both blocks of the first retail copy are written. */
            assert(held == (cut > 6));
            assert(FxStockPlay(&file, saves, (int)full, fate ? LetGo : NULL));
            assert(CpSaveImage_Load(&image, &cpFile, CP_SAVE_MAGIC_INTERNATIONAL) == CP_SAVE_OK);
            entry = journal;
            assert(WtGame_Inventory(&game, WT_INVENTORY_RECEIVE, &entry, 0) == 0);
            assert(WtGame_Inventory(&game, WT_INVENTORY_VERIFY_RECEIVED, &entry, 0) == 0);
            assert(FxStockCopies(&file, incoming) == (held && !fate));
            lost += !held;
            entry.phase = WT_JOURNAL_APPLIED;
            assert(WtGame_CanLeave(&game, &entry));
            /* Cut again before the journal recorded the recovery: done already. */
            memcpy(resumed, file.bytes, sizeof(resumed));
            assert(CpSaveImage_Load(&image, &cpFile, CP_SAVE_MAGIC_INTERNATIONAL) == CP_SAVE_OK);
            entry = journal;
            assert(WtGame_Inventory(&game, WT_INVENTORY_RECEIVE, &entry, 0) == 0);
            assert(WtGame_Inventory(&game, WT_INVENTORY_VERIFY_RECEIVED, &entry, 0) == 0);
            assert(memcmp(resumed, file.bytes, sizeof(resumed)) == 0 && FxStockCopies(&file, incoming) == (held && !fate));
            ++cases;
        }
        printf("PASS stock save while applying, delivery %s: %u cuts of RECEIVE into the party and the PC, "
            "0 duplicates, %u lost (cut before write 7, the first retail copy holding it)\n", fates[fate], cases, lost);
    }

    /* Not applied, and the player filled the party: never added, not even to
     * the PC (the save moved on: the games' window). */
    Applying(5, &spec, &journal, 1);
    assert(journal.destination.kind == 1);
    assert(FxStockPlay(&file, 1, 0, AddPartyMember));
    assert(CpSaveImage_Load(&image, &cpFile, CP_SAVE_MAGIC_INTERNATIONAL) == CP_SAVE_OK);
    entry = journal;
    assert(WtGame_Inventory(&game, WT_INVENTORY_RECEIVE, &entry, 0) == 0);
    assert(WtGame_Inventory(&game, WT_INVENTORY_VERIFY_RECEIVED, &entry, 0) == 0);
    assert(FxStockCopies(&file, incoming) == 0 && FxImageCount(&image) == 6);
    puts("PASS stock save while applying: not written before the stock game saved, never added");

    /* No room left at all: the same, accepted as it is. */
    Applying(6, &spec, &journal, 1);
    assert(FxStockPlay(&file, 1, 0, FillPc));
    assert(CpSaveImage_Load(&image, &cpFile, CP_SAVE_MAGIC_INTERNATIONAL) == CP_SAVE_OK);
    entry = journal;
    assert(WtGame_Inventory(&game, WT_INVENTORY_RECEIVE, &entry, 0) == 0);
    assert(FxStockCopies(&file, incoming) == 0);
    puts("PASS stock save while applying: a full save is accepted as it is");
}

static unsigned char impostor[136];

static void PlaceImpostor(unsigned char *general, unsigned char *boxes)
{
    (void)general;
    assert(WtPokemon_Encode(impostor, boxes + FX_PC + 3 * FX_BOX_MON) == 0);
}

/* LUNE-906: the one Pokémon with the delivery's identity, in any state (an
 * evolution changes the species), is recorded as the delivery where it is,
 * as the games record it (delivery.h). Two are never told apart (LUNE-914:
 * Pokémon with a fixed personality and trainer ID): refused, nothing written. */
static void StockSaveCollision(void)
{
    static unsigned char before[CP_SAVE_FILE_SIZE];
    WtJournalEntry journal, entry;
    Spec spec = Basic(64, MODEST_ODD_LOW), other = Basic(25, MODEST_ODD_LOW);
    unsigned char canonical[136], digest[32];
    Canonical(&other, impostor);
    /* Not written, one other Pokémon with its identity: taken as it is. */
    Applying(5, &spec, &journal, 1);
    assert(FxStockPlay(&file, 1, 0, PlaceImpostor));
    assert(CpSaveImage_Load(&image, &cpFile, CP_SAVE_MAGIC_INTERNATIONAL) == CP_SAVE_OK);
    entry = journal;
    assert(WtGame_Inventory(&game, WT_INVENTORY_RECEIVE, &entry, 0) == 0);
    assert(WtGame_Inventory(&game, WT_INVENTORY_VERIFY_RECEIVED, &entry, 0) == 0);
    assert(entry.destination.kind == 2 && entry.destination.box == 0 && entry.destination.slot == 3);
    assert(WtPokemon_Decode(FxImageBox(&image, 0, 3), canonical) == 0 && FxSha(canonical, 136, digest) == 0);
    assert(memcmp(digest, entry.appliedDigest, 32) == 0 && FxImageCount(&image) == 5);
    /* Written, and another with its identity: two, refused untouched. */
    Applying(5, &spec, &journal, 13);
    assert(FxStockPlay(&file, 1, 0, PlaceImpostor));
    assert(CpSaveImage_Load(&image, &cpFile, CP_SAVE_MAGIC_INTERNATIONAL) == CP_SAVE_OK);
    memcpy(before, file.bytes, sizeof(before));
    entry = journal;
    assert(WtGame_Inventory(&game, WT_INVENTORY_RECEIVE, &entry, 0) == -1);
    assert(memcmp(before, file.bytes, sizeof(before)) == 0);
    puts("PASS stock save while applying: one Pokemon sharing the identity is recorded as the delivery, two are refused");
}

/* The stock game: the delivered Shaymin, in the party, deposited in PC box 0
 * slot 0, where it is Land Forme again. */
static void DepositLandForme(unsigned char *general, unsigned char *boxes)
{
    unsigned char canonical[136], *party = general + FX_PARTY;
    unsigned count = Fx32(party + 4), slot, i, sum = 0;
    for (slot = 0; slot < count; ++slot) {
        assert(WtPokemon_Decode(party + 8 + slot * FX_MON, canonical) == 0);
        if ((canonical[8] | (canonical[9] << 8)) == 492) break;
    }
    if (slot == count) return;
    canonical[0x40] &= 7; /* form 0 */
    for (i = 8; i < 136; i += 2) sum += canonical[i] | (canonical[i + 1] << 8);
    FxPut16(canonical + 6, sum & 65535);
    assert(WtPokemon_Encode(canonical, FxPcSlot(boxes, 0)) == 0);
    for (i = slot; i + 1 < count; ++i) memcpy(party + 8 + i * FX_MON, party + 8 + (i + 1) * FX_MON, FX_MON);
    FxEmpty(party + 8 + (count - 1) * FX_MON, FX_MON);
    FxPut32(party + 4, count - 1);
}

static void StockSaveRedirect(void)
{
    WtJournalEntry journal, entry;
    WtReceivedPokemon received;
    Spec sky = Basic(492, MODEST_ODD_LOW);
    unsigned char canonical[136], digest[32];

    /* Sky Shaymin received into the party, then deposited in the stock game
     * (Land Forme in the PC) before the journal recorded it: the recovery
     * records what the save holds, where it holds it, and adds nothing. */
    sky.form = 1;
    Applying(5, &sky, &journal, 13);
    assert(FxStockPlay(&file, 1, 0, DepositLandForme));
    assert(CpSaveImage_Load(&image, &cpFile, CP_SAVE_MAGIC_INTERNATIONAL) == CP_SAVE_OK);
    entry = journal;
    assert(WtGame_Inventory(&game, WT_INVENTORY_RECEIVE, &entry, 0) == 0);
    assert(WtGame_Inventory(&game, WT_INVENTORY_VERIFY_RECEIVED, &entry, 0) == 0);
    assert(entry.destination.kind == 2 && entry.destination.box == 0 && entry.destination.slot == 0);
    assert(WtPokemon_Decode(FxImageBox(&image, 0, 0), canonical) == 0);
    FxSha(canonical, 136, digest);
    assert(memcmp(digest, entry.appliedDigest, 32) == 0 && FxImageCount(&image) == 5);
    assert(WtGame_Received(&entry, &received) == 0 && received.species == 492 && received.form == 0);
    puts("PASS stock save while applying: a delivery deposited since is recorded as the Land Forme the PC holds");
}

/* ---- Through the engine and its journal ------------------------------------ */

static int Hash(const unsigned char *bytes, size_t size, unsigned char digest[32]) { return FxSha(bytes, size, digest); }

/* A one-trade server whose offer is matched with `pokemon`. */
static struct {
    unsigned char saveID[16], trade[16], asset[16], receipt[16], offered[32], received[32], pokemon[136];
    unsigned char events[8][WT_WIRE_EVENT_SIZE];
    uint64_t nextCounter;
    unsigned eventCount, state;
} match;

static void Put(unsigned char *p, uint64_t value, unsigned size)
{
    while (size--) { *p++ = (unsigned char)value; value >>= 8; }
}

static uint64_t Get(const unsigned char *p, unsigned size)
{
    uint64_t value = 0;
    while (size--) value = (value << 8) | p[size];
    return value;
}

static void MatchEvent(unsigned kind, const unsigned char *operation, const unsigned char *digest, int receipt)
{
    unsigned char *e = match.events[match.eventCount];
    memset(e, 0, WT_WIRE_EVENT_SIZE);
    Put(e + WT_WIRE_EVENT_SEQUENCE, ++match.eventCount, 8); e[WT_WIRE_EVENT_KIND] = (unsigned char)kind;
    if (kind != WT_EVENT_ENROLLED) {
        memcpy(e + WT_WIRE_EVENT_TRADE, match.trade, 16); memcpy(e + WT_WIRE_EVENT_ASSET, match.asset, 16);
        memcpy(e + WT_WIRE_EVENT_DIGEST, digest, 32);
    }
    if (operation) memcpy(e + WT_WIRE_EVENT_OPERATION, operation, 16);
    if (receipt) memcpy(e + WT_WIRE_EVENT_RECEIPT, match.receipt, 16);
    e[WT_WIRE_EVENT_STATE] = (unsigned char)match.state;
}

static void MatchSnapshot(unsigned char *s)
{
    memset(s, 0, WT_WIRE_SNAPSHOT_SIZE);
    memcpy(s + WT_WIRE_SNAPSHOT_TRADE, match.trade, 16); s[WT_WIRE_SNAPSHOT_STATE] = (unsigned char)match.state;
    memcpy(s + WT_WIRE_SNAPSHOT_OFFERED_ASSET, match.asset, 16);
    memcpy(s + WT_WIRE_SNAPSHOT_OFFERED_DIGEST, match.offered, 32);
    if (match.state >= WT_TRADE_MATCHED) {
        s[WT_WIRE_SNAPSHOT_DELIVERY_KIND] = WT_DELIVERY_MATCH;
        memset(s + WT_WIRE_SNAPSHOT_RECEIVED_ASSET, 0x74, 16);
        memcpy(s + WT_WIRE_SNAPSHOT_RECEIVED_DIGEST, match.received, 32);
        memcpy(s + WT_WIRE_SNAPSHOT_RECEIPT, match.receipt, 16);
    }
    Put(s + WT_WIRE_SNAPSHOT_LAST_EVENT, match.eventCount, 8);
}

static int MatchPost(void *arg, const unsigned char *request, size_t size,
    unsigned char reply[WT_WIRE_REPLY_MAX], size_t *replySize)
{
    const unsigned char *body = request + WT_WIRE_REQUEST_HEADER_SIZE;
    unsigned operation = request[WT_WIRE_REQUEST_OPERATION], result = WT_RESULT_OK;
    unsigned char *out = reply + WT_WIRE_REPLY_HEADER_SIZE;
    size_t length = 0;
    (void)arg;
    assert(WtWire_CheckRequest(request, size, Hash) == 0);
    switch (operation) {
    case WT_OP_ENROLL:
        memset(match.saveID, 0x51, 16); match.nextCounter = 1;
        if (!match.eventCount) MatchEvent(WT_EVENT_ENROLLED, NULL, NULL, 0);
        memcpy(out, match.saveID, 16); length = 16;
        break;
    case WT_OP_RECONCILE: {
        uint64_t after = Get(body + WT_WIRE_RECONCILE_AFTER, 8), i;
        unsigned limit = body[WT_WIRE_RECONCILE_LIMIT], count = 0;
        for (i = after; i < match.eventCount && count < limit; ++i, ++count)
            memcpy(out + WT_WIRE_PAGE_EVENTS + count * WT_WIRE_EVENT_SIZE, match.events[i], WT_WIRE_EVENT_SIZE);
        out[WT_WIRE_PAGE_COUNT] = (unsigned char)count; out[WT_WIRE_PAGE_HAS_MORE] = after + count < match.eventCount;
        Put(out + WT_WIRE_PAGE_END, after + count, 8);
        length = WT_WIRE_PAGE_EVENTS + count * WT_WIRE_EVENT_SIZE;
        break;
    }
    case WT_OP_SUBMIT:
        assert(Get(request + WT_WIRE_REQUEST_COUNTER, 8) == match.nextCounter);
        FxSha(body + WT_WIRE_SUBMIT_POKEMON, 136, match.offered);
        ++match.nextCounter;
        memset(match.trade, 0x71, 16); memset(match.asset, 0x72, 16); match.state = WT_TRADE_QUEUED;
        MatchEvent(WT_EVENT_SUBMITTED, request + WT_WIRE_REQUEST_ID, match.offered, 0);
        Put(out, match.eventCount, 8); MatchSnapshot(out + WT_WIRE_OUTCOME_SNAPSHOT); length = WT_WIRE_OUTCOME_SIZE;
        break;
    case WT_OP_STATUS:
        MatchSnapshot(out); length = WT_WIRE_SNAPSHOT_SIZE;
        break;
    case WT_OP_COLLECT:
        if (match.state != WT_TRADE_MATCHED) { result = WT_RESULT_CONFLICT; break; }
        MatchSnapshot(out); memcpy(out + WT_WIRE_COLLECTION_POKEMON, match.pokemon, 136); length = WT_WIRE_COLLECTION_SIZE;
        break;
    case WT_OP_ACKNOWLEDGE:
        assert(Get(request + WT_WIRE_REQUEST_COUNTER, 8) == match.nextCounter);
        ++match.nextCounter; match.state = WT_TRADE_ACKNOWLEDGED;
        MatchEvent(WT_EVENT_ACKNOWLEDGED, request + WT_WIRE_REQUEST_ID, match.received, 1);
        Put(out, match.eventCount, 8); MatchSnapshot(out + WT_WIRE_OUTCOME_SNAPSHOT); length = WT_WIRE_OUTCOME_SIZE;
        break;
    default:
        assert(0);
    }
    if (result != WT_RESULT_OK) length = 0;
    memset(reply, 0, WT_WIRE_REPLY_HEADER_SIZE);
    memcpy(reply, request, WT_WIRE_MAGIC_SIZE); reply[WT_WIRE_REPLY_OPERATION] = (unsigned char)operation;
    Put(reply + WT_WIRE_REPLY_BODY_LENGTH, length, 2);
    memcpy(reply + WT_WIRE_REPLY_REQUEST_ID, request + WT_WIRE_REQUEST_ID, 16);
    Put(reply + WT_WIRE_REPLY_RESULT, result, 2);
    Put(reply + WT_WIRE_REPLY_NEXT_COUNTER, match.nextCounter, 8);
    Put(reply + WT_WIRE_REPLY_HIGHWATER, match.eventCount, 8);
    *replySize = WT_WIRE_REPLY_HEADER_SIZE + length;
    return 0;
}

static const CpTransport matchTransport = { NULL, MatchPost, NULL, NULL, NULL };

/* The journal banks; a commit of `refusePhase` fails once (the power is cut
 * before the journal records it), and once `cutAfter` is committed the next
 * save write fails (cut before the step writes). */
static unsigned char banks[2][WT_JOURNAL_RECORD_SIZE];
static unsigned refusePhase, cutAfter, randomCounter;

static int ReadBank(void *arg, int bank, unsigned char record[WT_JOURNAL_RECORD_SIZE])
{
    (void)arg; memcpy(record, banks[bank], WT_JOURNAL_RECORD_SIZE); return 0;
}

static int WriteBank(void *arg, int bank, const unsigned char record[WT_JOURNAL_RECORD_SIZE])
{
    WtJournalEntry entry;
    (void)arg;
    assert(WtJournal_Decode(record, Hash, &entry) == 0);
    if (refusePhase && entry.phase == refusePhase) { refusePhase = 0; return -1; }
    memcpy(banks[bank], record, WT_JOURNAL_RECORD_SIZE);
    if (cutAfter && entry.phase == cutAfter) { cutAfter = 0; file.failWrite = file.writes + 1; }
    return 0;
}

static int EngineRandom(void *arg, unsigned char *bytes, size_t size)
{
    unsigned char input[4], digest[32];
    (void)arg;
    FxPut32(input, ++randomCounter); FxSha(input, 4, digest);
    memcpy(bytes, digest, size); return 0;
}

static int AnyHoldable(void *arg, unsigned item) { (void)arg; (void)item; return 1; }

static void OpenEngine(WtEngine *engine)
{
    WtJournalIO journal = { NULL, ReadBank, WriteBank, Hash };
    WtEngineIO io = { &game, EngineRandom, WtGame_Inventory, WtGame_CanHold };
    assert(CpSaveImage_Load(&image, &cpFile, CP_SAVE_MAGIC_INTERNATIONAL) == CP_SAVE_OK);
    game.holdable = AnyHoldable;
    assert(WtEngine_Open(engine, journal, io) == 0);
}

static void FillParty(unsigned char *general, unsigned char *boxes)
{
    while (Fx32(general + FX_PARTY + 4) < 6) AddPartyMember(general, boxes);
}

/* A matched Sky Shaymin, cut after APPLYING was recorded; the stock game then
 * fills the party; the recovery puts it in the PC (Land Forme) and is cut
 * before APPLIED is recorded; the next settle must still record APPLIED, and
 * the ACK must leave the journal IDLE. */
/* Through the engine: offer party slot 1, the server matches it, and the
 * visit collects the delivery with `cut` (APPLYING: before RECEIVE writes;
 * APPLIED: after, before the journal records it). */
static void CollectCut(const Spec *spec, unsigned cut)
{
    WtJournalEntry entry;
    WtEngine engine;
    CpVisit visit;
    Setup(5);
    memset(&match, 0, sizeof(match));
    Canonical(spec, match.pokemon); FxSha(match.pokemon, 136, match.received);
    memset(match.receipt, 0x73, 16);
    assert(WtTest_JournalEntry(WT_JOURNAL_EMPTY, Hash, &entry) == 0);
    assert(WtJournal_Encode(&entry, Hash, banks[0]) == 0);
    memcpy(banks[1], banks[0], sizeof(banks[0]));
    refusePhase = cutAfter = randomCounter = 0;
    OpenEngine(&engine);
    assert(CpVisit_Run(&engine, &game, 7, 1, 0, &matchTransport, &visit) == 0 && visit.phase == WT_JOURNAL_QUEUED);
    WtEngine_Close(&engine);
    match.state = WT_TRADE_MATCHED;
    MatchEvent(WT_EVENT_MATCHED, NULL, match.received, 1);
    if (cut == WT_JOURNAL_APPLYING) cutAfter = cut; else refusePhase = cut;
    OpenEngine(&engine);
    assert(CpVisit_Run(&engine, &game, 7, -1, WT_OP_COLLECT, &matchTransport, &visit) != 0);
    WtEngine_Close(&engine);
    file.failWrite = 0;
}

/* LUNE-906 through the engine and its journal: after a stock-game save the
 * delivery is never added again. Written, then deposited (Land Forme in the
 * PC): APPLIED records it there and the ACK carries its digest. Cut before
 * it was written: APPLIED without it, the ACK ends the trade, nothing added
 * (the games' window). */
static void StockSaveRedirectThroughJournal(void)
{
    WtJournalEntry entry;
    WtEngine engine;
    CpVisit visit;
    WtReceivedPokemon received;
    Spec sky = Basic(492, MODEST_ODD_LOW);
    unsigned char canonical[136], digest[32];
    sky.form = 1;
    CollectCut(&sky, WT_JOURNAL_APPLIED);
    assert(FxStockCopies(&file, match.pokemon) == 1);
    assert(FxStockPlay(&file, 1, 0, DepositLandForme));
    OpenEngine(&engine);
    assert(engine.entry.phase == WT_JOURNAL_APPLYING && engine.entry.destination.kind == 1);
    assert(WtEngine_Settle(&engine) == 0 && engine.entry.phase == WT_JOURNAL_APPLIED);
    entry = engine.entry;
    WtEngine_Close(&engine);
    assert(entry.destination.kind == 2 && entry.destination.length == 136);
    assert(WtPokemon_Decode(FxImageBox(&image, 0, 0), canonical) == 0);
    FxSha(canonical, 136, digest);
    assert(memcmp(digest, entry.appliedDigest, 32) == 0);
    assert(WtGame_Received(&entry, &received) == 0 && received.species == 492 && received.form == 0);
    OpenEngine(&engine);
    assert(CpVisit_Run(&engine, &game, 7, -1, 0, &matchTransport, &visit) == 0);
    assert(visit.phase == WT_JOURNAL_IDLE && visit.canLeave && match.state == WT_TRADE_ACKNOWLEDGED);
    WtEngine_Close(&engine);
    assert(FxStockCopies(&file, match.pokemon) == 1);
    puts("PASS stock save while applying: a delivery deposited since commits APPLIED where it is and is acknowledged");

    CollectCut(&sky, WT_JOURNAL_APPLYING);
    assert(FxStockCopies(&file, match.pokemon) == 0);
    assert(FxStockPlay(&file, 1, 0, FillParty));
    OpenEngine(&engine);
    assert(engine.entry.phase == WT_JOURNAL_APPLYING);
    assert(WtEngine_Settle(&engine) == 0 && engine.entry.phase == WT_JOURNAL_APPLIED);
    WtEngine_Close(&engine);
    OpenEngine(&engine);
    assert(CpVisit_Run(&engine, &game, 7, -1, 0, &matchTransport, &visit) == 0);
    assert(visit.phase == WT_JOURNAL_IDLE && visit.canLeave && match.state == WT_TRADE_ACKNOWLEDGED);
    WtEngine_Close(&engine);
    assert(FxStockCopies(&file, match.pokemon) == 0);
    puts("PASS stock save while applying: cut before it was written, APPLIED and acknowledged without adding it");
}

int main(void)
{
    Stats();
    Levels();
    Genders();
    Evolution();
    Pokedex();
    JournalAndArceus();
    MailAndPc();
    Placement();
    StockSave();
    StockSaveCollision();
    StockSaveRedirect();
    StockSaveRedirectThroughJournal();
    puts("PASS platinum receive");
    return 0;
}
