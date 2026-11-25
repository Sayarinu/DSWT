/* LUNE-881: the Generation V adapter on Black/White (FX5_GAME 0) and Black
 * 2/White 2 (FX5_GAME 1) saves: offers, removal and restore, deliveries with
 * the receive effects, older saves, stock-game recovery, the musical
 * distribution area and what the game can hold. Each step's result is read
 * back through a fresh load, as the game would load it. */
#include "gen5_fixture.h"

#include <stdio.h>
#include <stdlib.h>

#include "adapter/gen5_receive.h"
#include "save/gen5_save.h"
#include "save/musical_area.h"
#include "save/journal_save.h"
#include "game/text.h"

#define B2W2 (FX5_GAME == 1)

static Fx5File file;
static CpSaveImage image, check;
static CpFile cpFile = { &file, Fx5Read, Fx5Write, Fx5Sync, Fx5Size };
static WtGame game;

/* ---- Fake game data -------------------------------------------------------- */

typedef struct FakeSpecies {
    uint16_t species;
    uint8_t form, base[6], ratio, growth, friendship, abilities[3];
} FakeSpecies;

static const FakeSpecies kSpecies[] = {
    { 64, 0, { 40, 35, 30, 105, 120, 70 }, 63, 3, 70, { 28, 39, 98 } },     /* Kadabra */
    { 65, 0, { 55, 50, 45, 120, 135, 85 }, 63, 3, 70, { 28, 39, 98 } },     /* Alakazam */
    { 93, 0, { 45, 50, 45, 95, 115, 55 }, 127, 3, 70, { 26, 0, 0 } },       /* Haunter */
    { 94, 0, { 60, 65, 60, 110, 130, 75 }, 127, 3, 70, { 130, 0, 0 } },     /* Gengar */
    { 95, 0, { 35, 45, 160, 70, 30, 45 }, 127, 0, 70, { 69, 5, 133 } },     /* Onix */
    { 208, 0, { 75, 85, 200, 30, 55, 65 }, 127, 0, 70, { 69, 5, 125 } },    /* Steelix */
    { 292, 0, { 1, 90, 45, 40, 30, 30 }, 255, 1, 70, { 25, 0, 0 } },        /* Shedinja */
    { 327, 0, { 60, 60, 60, 60, 60, 60 }, 127, 4, 70, { 20, 86, 126 } },    /* Spinda */
    { 588, 0, { 50, 75, 45, 60, 40, 45 }, 127, 0, 70, { 68, 61, 99 } },     /* Karrablast */
    { 589, 0, { 70, 135, 105, 20, 60, 105 }, 127, 0, 70, { 75, 142, 0 } },  /* Escavalier */
    { 616, 0, { 50, 40, 85, 25, 40, 65 }, 127, 0, 70, { 93, 75, 142 } },    /* Shelmet */
    { 617, 0, { 80, 70, 40, 145, 100, 60 }, 127, 0, 70, { 93, 60, 84 } },   /* Accelgor */
    { 479, 0, { 50, 50, 77, 91, 95, 77 }, 255, 0, 0, { 26, 0, 0 } },        /* Rotom */
    { 479, 1, { 50, 65, 107, 86, 105, 107 }, 255, 0, 0, { 26, 0, 0 } },     /* Heat Rotom */
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
    memset(out, 0, sizeof(*out));
    if (fallback < sizeof(kSpecies) / sizeof(kSpecies[0])) {
        const FakeSpecies *s = &kSpecies[fallback];
        memcpy(out->baseStats, s->base, 6);
        out->genderRatio = s->ratio; out->growthRate = s->growth; out->baseFriendship = s->friendship;
        out->abilities[0] = s->abilities[0]; out->abilities[1] = s->abilities[1]; out->hiddenAbility = s->abilities[2];
        return CP_OK;
    }
    if (species == 0 || species > 649) return CP_ERR_NOT_FOUND;
    memset(out->baseStats, 50, 6);
    out->genderRatio = 127; out->growthRate = 3; out->baseFriendship = 50;
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

static int FakeEvolutions(void *arg, uint16_t species, CpEvolution out[CP_EVOLUTIONS])
{
    (void)arg;
    memset(out, 0, sizeof(CpEvolution) * CP_EVOLUTIONS);
    if (species == 64) { out[0].method = 4; out[0].parameter = 99; out[0].target = 3; out[1].method = 5; out[1].target = 65; }
    if (species == 93) { out[0].method = 5; out[0].target = 94; }
    if (species == 95) { out[0].method = 6; out[0].parameter = 233; out[0].target = 208; }
    if (species == 588) { out[0].method = 7; out[0].target = 589; }
    if (species == 616) { out[0].method = 7; out[0].target = 617; }
    return CP_OK;
}

static int FakeTrade(void *arg, uint16_t species, uint16_t held, CpTradeEvolution *out)
{
    (void)arg; (void)species; (void)held; (void)out;
    return CP_ERR_NOT_FOUND;
}

enum { EVERSTONE = 229, METAL_COAT = 233, MAIL = 140 };

static int FakeHoldEffect(void *arg, uint16_t item, uint8_t *out)
{
    (void)arg;
    if (item == EVERSTONE) { *out = 64; return CP_OK; }
    if (item == METAL_COAT) { *out = 68; return CP_OK; }
    if (item >= 1 && item <= 638) { *out = 0; return CP_OK; }
    return CP_ERR_NOT_FOUND;
}

static int FakeMovePp(void *arg, uint16_t move, uint8_t *out)
{
    (void)arg;
    *out = (uint8_t)(move % 40 + 5);
    return CP_OK;
}

static int FakeName(void *arg, uint16_t species, uint16_t *out, size_t capacity, size_t *length)
{
    const char *name = species == 65 ? "Alakazam" : species == 589 ? "Escavalier" : "Other";
    size_t n = strlen(name), i;
    (void)arg;
    if (n + 1 > capacity) return CP_ERR_RANGE;
    for (i = 0; i < n; ++i) out[i] = (uint16_t)name[i];
    out[n] = CP_CHAR_EOS;
    *length = n;
    return CP_OK;
}

static CpAdapterData data = { NULL, FakePersonal, FakeExp, FakeTrade, FakeHoldEffect, FakeMovePp, FakeName, 2,
                              FakeEvolutions };

static int FakeHoldable(void *arg, unsigned item)
{
    (void)arg;
    return item != 300;
}

/* ---- Saves ----------------------------------------------------------------- */

static void Setup(const Fx5Spec *specs, unsigned count)
{
    Fx5Build(&file, specs, count, 40);
    assert(Fx5Load(&image, &cpFile) == CP_SAVE_OK);
    memset(&game, 0, sizeof(game));
    game.image = &image; game.file = &cpFile;
    game.holdable = FakeHoldable; game.data = &data; game.version = B2W2 ? 23 : 21;
}

static void Defaults(Fx5Spec *specs, unsigned count)
{
    unsigned i;
    for (i = 0; i < count; ++i) specs[i] = Fx5Default(i);
}

static int Step(enum WtInventoryStep step, WtJournalEntry *entry, unsigned slot)
{
    return WtGame_Inventory(&game, step, entry, slot);
}

/* What the game loads now: both copies hold the image. */
static void BothCopies(void)
{
    unsigned char digest[32], loaded[32];
    assert(CpSaveImage_Hash(&image, digest) == CP_SAVE_OK);
    assert(Fx5Load(&check, &cpFile) == CP_SAVE_OK && CpSaveImage_Hash(&check, loaded) == CP_SAVE_OK);
    assert(memcmp(digest, loaded, 32) == 0 && check.globalCounter == image.globalCounter);
    assert(CpSaveImage_Verify(&image, &cpFile, digest) == CP_SAVE_OK);
    assert(memcmp(file.bytes, file.bytes + FX5_BACKUP, FX5_USED) == 0);
}

/* ---- Offers ------------------------------------------------------------------ */

static void Eligibility(void)
{
    Fx5Spec specs[6];
    unsigned char canonical[136];
    Defaults(specs, 6);
    specs[1].egg = 1; specs[2].item = MAIL; specs[4].hp = 0; specs[5].hp = 0;
    Setup(specs, 6);
    assert(CpAdapter_Eligible(&image, 0) && CpGen5_Reason(&image, 0) == CP_GEN5_OK);
    assert(CpGen5_Reason(&image, 1) == CP_GEN5_EGG && !CpAdapter_Eligible(&image, 1));
    assert(CpGen5_Reason(&image, 2) == CP_GEN5_MAIL);
    assert(CpAdapter_Eligible(&image, 3) && !CpAdapter_Eligible(&image, 6));
    /* An item past the game's last reads as none. */
    specs[2].item = B2W2 ? 639 : 627;
    Setup(specs, 6);
    assert(CpAdapter_Eligible(&image, 2));
    /* The last usable Pokémon, and a slot that claims to be decrypted. */
    Defaults(specs, 2); specs[1].hp = 0;
    Setup(specs, 2);
    assert(CpGen5_Reason(&image, 0) == CP_GEN5_LAST_USABLE && CpAdapter_Eligible(&image, 1));
    Fx5ImageParty(&image, 1)[4] = 1;
    assert(!CpAdapter_Eligible(&image, 1) && !CpAdapter_Eligible(&image, 0));
    /* A copy in a PC box, then in the Battle Box, is not unique. */
    Defaults(specs, 3);
    Setup(specs, 3);
    assert(CpAdapter_Unique(&image, 1));
    assert(WtPokemon_Decode(Fx5ImageParty(&image, 1), canonical) == 0);
    assert(WtPokemon_Encode(canonical, Fx5ImageBox(&image, 23, 29)) == 0);
    assert(!CpAdapter_Unique(&image, 1));
    Fx5Empty(Fx5ImageBox(&image, 23, 29), FX5_BOX_MON);
    assert(WtPokemon_Encode(canonical, Fx5ImageBattleBox(&image, 5)) == 0);
    assert(!CpAdapter_Unique(&image, 1) && CpAdapter_Unique(&image, 0));
    printf("PASS %s offers: eggs, mail, the last usable Pokémon and decrypted slots are refused; copies in the PC or Battle Box are found\n",
           FX5_NAME);
}

static void RemoveAndRestore(void)
{
    Fx5Spec specs[4];
    WtJournalEntry entry;
    unsigned char second[FX5_MON], third[FX5_MON], before[32], empty[FX5_MON];
    Defaults(specs, 4);
    Setup(specs, 4);
    memcpy(second, Fx5ImageParty(&image, 1), FX5_MON);
    memcpy(third, Fx5ImageParty(&image, 2), FX5_MON);
    assert(CpSaveImage_Hash(&image, before) == CP_SAVE_OK);
    memset(&entry, 0, sizeof(entry));
    assert(Step(WT_INVENTORY_PREPARE_REMOVE, &entry, 1) == 0);
    /* Preparing changes nothing, records the escrow and both inventories. */
    assert(Fx5ImageCount(&image) == 4 && memcmp(Fx5ImageParty(&image, 1), second, FX5_MON) == 0);
    assert(entry.source.kind == 1 && entry.source.slot == 1 && entry.source.length == FX5_MON);
    assert(memcmp(entry.original, second, FX5_MON) == 0 && memcmp(entry.inventoryBefore, before, 32) == 0);
    assert(entry.globalCounter == 40 && entry.generalCounter == 40 && entry.boxCounter == 40);
    assert(Step(WT_INVENTORY_REMOVE, &entry, 1) == 0);
    Fx5Empty(empty, FX5_MON);
    assert(Fx5ImageCount(&image) == 3 && memcmp(Fx5ImageParty(&image, 1), third, FX5_MON) == 0);
    assert(memcmp(Fx5ImageParty(&image, 3), empty, FX5_MON) == 0);
    /* The removal moved the save count WT_ABANDON_STEP ahead (LUNE-902). */
    assert(image.globalCounter == 40 + WT_ABANDON_STEP && entry.globalCounter == 40 + WT_ABANDON_STEP);
    BothCopies();
    assert(Step(WT_INVENTORY_VERIFY_REMOVED, &entry, 1) == 0 && WtGame_CanLeave(&game, &entry) == 1);
    /* Again on the removed save: nothing more is removed. */
    assert(Step(WT_INVENTORY_REMOVE, &entry, 1) == 0 && Fx5ImageCount(&image) == 3);
    assert(Step(WT_INVENTORY_PREPARE_RESTORE, &entry, 1) == 0);
    assert(Step(WT_INVENTORY_RESTORE, &entry, 1) == 0);
    assert(Fx5ImageCount(&image) == 4 && memcmp(Fx5ImageParty(&image, 1), second, FX5_MON) == 0);
    {
        unsigned char now[32];
        assert(CpSaveImage_Hash(&image, now) == CP_SAVE_OK && memcmp(now, before, 32) == 0);
    }
    BothCopies();
    /* An older save than the journal's is never promoted. */
    entry.globalCounter = image.globalCounter + 1;
    assert(Step(WT_INVENTORY_VERIFY_REMOVED, &entry, 1) != 0);
    printf("PASS %s remove and restore: the party shifts as the game's, both copies hold each step, the save count is the counter\n",
           FX5_NAME);
}

/* ---- Deliveries -------------------------------------------------------------- */

typedef struct Spec {
    unsigned species, item, form, language, hidden, nicknamed, origin;
    uint32_t pid, ot, exp;
    unsigned ivs[6], evs[6], moves[4], pp[4], ups[4], nature;
} Spec;

static void Canonical(const Spec *spec, unsigned char out[136])
{
    unsigned i, sum = 0;
    uint32_t ivs = 0;
    memset(out, 0, 136);
    Fx5W32(out, spec->pid);
    Fx5W16(out + 0x08, spec->species); Fx5W16(out + 0x0A, spec->item);
    Fx5W32(out + 0x0C, spec->ot); Fx5W32(out + 0x10, spec->exp);
    out[0x14] = 3; out[0x15] = 1;
    out[0x17] = (unsigned char)spec->language;
    for (i = 0; i < 6; ++i) { out[0x18 + i] = (unsigned char)spec->evs[i]; ivs |= (uint32_t)spec->ivs[i] << (5 * i); }
    for (i = 0; i < 4; ++i) {
        Fx5W16(out + 0x28 + 2 * i, spec->moves[i]); out[0x30 + i] = (unsigned char)spec->pp[i];
        out[0x34 + i] = (unsigned char)spec->ups[i];
    }
    Fx5W32(out + 0x38, ivs | (uint32_t)(spec->nicknamed & 1) << 31);
    out[0x40] = (unsigned char)(spec->form << 3 | 2);
    out[0x41] = (unsigned char)spec->nature;
    out[0x42] = (unsigned char)(spec->hidden & 1);
    for (i = 0; i < 10; ++i) Fx5W16(out + 0x48 + 2 * i, 'N');
    Fx5W16(out + 0x48 + 20, 0xFFFF);
    out[0x5F] = (unsigned char)spec->origin;
    for (i = 0; i < 7; ++i) Fx5W16(out + 0x68 + 2 * i, 'T');
    Fx5W16(out + 0x68 + 14, 0xFFFF);
    out[0x83] = 4;
    for (i = 8; i < 136; i += 2) sum += Fx5R16(out + i);
    Fx5W16(out + 6, sum & 0xFFFF);
}

static Spec Basic(unsigned species)
{
    Spec spec;
    memset(&spec, 0, sizeof(spec));
    spec.species = species; spec.language = 2; spec.origin = 21;
    spec.pid = 0x12345678; spec.ot = 0x0BADF00D; spec.exp = 27000; /* level 30 */
    spec.ivs[0] = 31; spec.ivs[1] = 20; spec.ivs[2] = 10; spec.ivs[3] = 5; spec.ivs[4] = 25; spec.ivs[5] = 15;
    spec.evs[0] = 100; spec.evs[1] = 4; spec.evs[3] = 252;
    spec.moves[0] = 33; spec.moves[1] = 35; spec.pp[0] = 1; spec.pp[1] = 60; spec.ups[0] = 3; spec.ups[1] = 1;
    spec.nature = 10; /* Timid: +Speed, -Attack */
    return spec;
}

static void Match(WtJournalEntry *entry, const Spec *spec)
{
    entry->deliveryKind = WT_DELIVERY_MATCH;
    Canonical(spec, entry->incoming);
    entry->phase = WT_JOURNAL_RECEIVED;
}

/* PREPARE_RECEIVE, RECEIVE and VERIFY_RECEIVED, as the engine drives them. */
static void Deliver(WtJournalEntry *entry)
{
    assert(Step(WT_INVENTORY_PREPARE_RECEIVE, entry, 0) == 0);
    assert(Step(WT_INVENTORY_RECEIVE, entry, 0) == 0);
    assert(Step(WT_INVENTORY_VERIFY_RECEIVED, entry, 0) == 0);
    BothCopies();
}

/* An offer removed from the party, ready for its delivery. */
static void Offered(WtJournalEntry *entry, unsigned partySize, unsigned partnerSpecies, unsigned partnerItem)
{
    Fx5Spec specs[6];
    Defaults(specs, partySize);
    specs[0].species = partnerSpecies; specs[0].item = partnerItem;
    Setup(specs, partySize);
    memset(entry, 0, sizeof(*entry));
    assert(Step(WT_INVENTORY_PREPARE_REMOVE, entry, 0) == 0 && Step(WT_INVENTORY_REMOVE, entry, 0) == 0);
}

static void Decoded(const unsigned char *image220, unsigned char box[136], unsigned char ext[84])
{
    assert(WtPokemon_Decode(image220, box) == 0);
    if (ext) {
        memcpy(ext, image220 + 136, 84);
        Fx5Crypt(ext, 84, Fx5R32(box));
    }
}

static int Bit(const unsigned char *bits, unsigned bit) { return (bits[bit >> 3] >> (bit & 7)) & 1; }

/* The stats as the game's formulas give them, by a separate route. */
static unsigned Stat(unsigned base, unsigned iv, unsigned ev, unsigned level, int mod)
{
    unsigned value = (2 * base + iv + ev / 4) * level / 100 + 5;
    if (mod > 0) value = (unsigned)(uint16_t)(value * 110) / 100;
    if (mod < 0) value = (unsigned)(uint16_t)(value * 90) / 100;
    return value;
}

static void PartyDelivery(void)
{
    WtJournalEntry entry;
    unsigned char box[136], ext[84], keystream[84], *dex;
    Spec spec = Basic(500);
    unsigned level = 30, max, i;
    Offered(&entry, 3, 501, 0);
    Match(&entry, &spec);
    Deliver(&entry);
    assert(entry.destination.kind == 1 && entry.destination.slot == 2 && entry.destination.length == FX5_MON);
    assert(Fx5ImageCount(&image) == 3);
    Decoded(Fx5ImageParty(&image, 2), box, ext);
    /* Level, stats, heal, friendship; the ability is not touched without an evolution. */
    max = (2 * 50 + 31 + 100 / 4) * level / 100 + level + 10;
    assert(ext[4] == level && Fx5R16(ext + 8) == max && Fx5R16(ext + 6) == max && Fx5R32(ext) == 0);
    assert(Fx5R16(ext + 10) == Stat(50, 20, 4, level, -1) && Fx5R16(ext + 12) == Stat(50, 10, 0, level, 0));
    assert(Fx5R16(ext + 14) == Stat(50, 5, 252, level, 1));
    assert(box[0x14] == 50 && box[0x15] == 1 && Fx5R16(box + 0x08) == 500);
    /* PP below the maximum rises; above it stays (party: HealFull). */
    assert(box[0x30] == (unsigned char)(38 + 38 * 20 * 3 / 100) && box[0x31] == 60);
    /* Fields nothing writes stay zero ciphertext: capsule, mail, padding. */
    memset(keystream, 0, sizeof(keystream));
    for (i = 0; i < 84; ++i) if (i == 5 || i >= 20) assert(Fx5ImageParty(&image, 2)[136 + i] == 0);
    (void)keystream;
    dex = CpSaveImage_Block(&image, FX5_POKEDEX);
    /* Male (pid 0x78 >= ratio 127? no: 0x78 = 120 < 127, female), not shiny. */
    assert(Bit(dex + 0x5C + 0x54, 499) && !Bit(dex + 0x5C, 499));
    assert(Bit(dex + 0x1AC + 0x54, 499));
    /* Caught, as the retail trades register it; languages are kept for the
     * first 493 species only. */
    assert(Bit(dex + 0x008, 499) && !Bit(dex + (B2W2 ? 0x328 : 0x320), (2 - 1) + 499 * 7));
    assert(WtGame_CanLeave(&game, (entry.phase = WT_JOURNAL_APPLIED, &entry)) == 1);
    {
        WtReceivedPokemon received;
        assert(WtGame_Received(&entry, &received) == 0 && received.species == 500 && received.gender == 1);
        assert(received.nickname[0] == 'N' && received.originalTrainer[0] == 'T');
    }
    printf("PASS %s party delivery: level, stats, heal, friendship and the Pokédex (caught, as retail) as the game's\n",
           FX5_NAME);
    /* The offer still in the save: a matched delivery is refused and nothing
     * changes, then prepared once the offer is gone. */
    {
        static unsigned char before[sizeof(file.bytes)];
        unsigned char digest[32], now[32];
        Fx5Spec specs[3];
        Defaults(specs, 3);
        Setup(specs, 3);
        memset(&entry, 0, sizeof(entry));
        assert(Step(WT_INVENTORY_PREPARE_REMOVE, &entry, 1) == 0);
        Match(&entry, &spec);
        memcpy(before, file.bytes, sizeof(before));
        assert(CpSaveImage_Hash(&image, digest) == CP_SAVE_OK);
        assert(Step(WT_INVENTORY_PREPARE_RECEIVE, &entry, 0) == -1);
        assert(memcmp(before, file.bytes, sizeof(before)) == 0);
        assert(CpSaveImage_Hash(&image, now) == CP_SAVE_OK && memcmp(digest, now, 32) == 0);
        assert(Step(WT_INVENTORY_REMOVE, &entry, 1) == 0 && Fx5ImageCount(&image) == 2);
        Deliver(&entry);
        assert(Fx5ImageCount(&image) == 3 && entry.destination.kind == 1 && entry.destination.slot == 2);
    }
    printf("PASS %s party delivery: a matched delivery waits while its offer is still in the save\n", FX5_NAME);
}

static void BoxDelivery(void)
{
    WtJournalEntry entry;
    unsigned char box[136], before[136];
    Spec spec = Basic(500);
    unsigned i;
    Offered(&entry, 6, 501, 0);
    /* A full party; the first box's first slot is taken. */
    memcpy(before, Fx5ImageParty(&image, 0), 136);
    memcpy(Fx5ImageBox(&image, 0, 0), before, 136);
    assert(CpSaveImage_CommitBoth(&image, &cpFile, (CpSaveImage_Hash(&image, before), before)) == CP_SAVE_OK);
    {
        Fx5Spec more = Fx5Default(9);
        Fx5PartyMon(&more, Fx5ImageParty(&image, 5));
        Fx5W32(CpSaveImage_Block(&image, FX5_PARTY) + 4, 6);
        assert(CpSaveImage_Hash(&image, before) == CP_SAVE_OK && CpSaveImage_CommitBoth(&image, &cpFile, before) == CP_SAVE_OK);
    }
    entry.inventoryAfter[0] ^= 0; /* the journal's inventories no longer match: re-prepared */
    memset(&entry.destination, 0, sizeof(entry.destination));
    Match(&entry, &spec);
    assert(Step(WT_INVENTORY_PREPARE_RECEIVE, &entry, 0) == 0);
    assert(entry.destination.kind == 2 && entry.destination.box == 0 && entry.destination.slot == 1);
    assert(entry.destination.length == FX5_BOX_MON);
    for (i = 136; i < 236; ++i) assert(entry.destinationAfter[i] == 0);
    assert(Step(WT_INVENTORY_RECEIVE, &entry, 0) == 0 && Step(WT_INVENTORY_VERIFY_RECEIVED, &entry, 0) == 0);
    BothCopies();
    Decoded(Fx5ImageBox(&image, 0, 1), box, NULL);
    /* The PC restores every move's PP to its maximum, lowering one above it. */
    assert(box[0x30] == (unsigned char)(38 + 38 * 20 * 3 / 100) && box[0x31] == (unsigned char)(40 + 40 * 20 / 100));
    printf("PASS %s PC delivery: the first empty slot, every move's PP at its maximum, 136 bytes recorded\n", FX5_NAME);
}

static void Evolutions(void)
{
    WtJournalEntry entry;
    unsigned char box[136], ext[84];
    Spec spec;
    /* Kadabra evolves by trade, even holding an Everstone; its name follows. */
    spec = Basic(64); spec.item = EVERSTONE;
    Offered(&entry, 3, 501, 0);
    Match(&entry, &spec);
    Deliver(&entry);
    Decoded(Fx5ImageParty(&image, 2), box, ext);
    assert(Fx5R16(box + 8) == 65 && Fx5R16(box + 0x0A) == EVERSTONE);
    assert(Fx5R16(box + 0x48) == 'A' && Fx5R16(box + 0x48 + 16) == 0xFFFF && Fx5R16(box + 0x48 + 18) == 'N'
        && Fx5R16(box + 0x48 + 20) == 0xFFFF && !(Fx5R32(box + 0x38) >> 31));
    /* Ability: PID bit 0 (origin before White) picks the second ability. */
    assert(box[0x15] == ((0x12345678u & 1) ? 39 : 28));
    /* The trade registers Kadabra, the evolution Alakazam: both caught, with
     * the language, as the retail trade and evolution scenes register them. */
    {
        const unsigned char *dex = CpSaveImage_Block(&image, FX5_POKEDEX);
        unsigned languages = B2W2 ? 0x328 : 0x320;
        assert(Bit(dex + 0x008, 63) && Bit(dex + 0x008, 64));
        assert(Bit(dex + languages, 1 + 63 * 7) && Bit(dex + languages, 1 + 64 * 7));
        assert((Bit(dex + 0x5C, 63) || Bit(dex + 0x5C + 0x54, 63)) && (Bit(dex + 0x5C, 64) || Bit(dex + 0x5C + 0x54, 64)));
    }
    assert(Fx5R16(ext + 8) == Fx5R16(ext + 6) && box[0x14] == 70);
    /* Haunter holding an Everstone stays; Onix drops its Metal Coat as Steelix
     * and keeps its nickname. */
    spec = Basic(93); spec.item = EVERSTONE;
    Offered(&entry, 3, 501, 0);
    Match(&entry, &spec);
    Deliver(&entry);
    Decoded(Fx5ImageParty(&image, 2), box, NULL);
    assert(Fx5R16(box + 8) == 93);
    spec = Basic(95); spec.item = METAL_COAT; spec.nicknamed = 1; spec.hidden = 1;
    Offered(&entry, 3, 501, 0);
    Match(&entry, &spec);
    Deliver(&entry);
    Decoded(Fx5ImageParty(&image, 2), box, NULL);
    assert(Fx5R16(box + 8) == 208 && Fx5R16(box + 0x0A) == 0 && Fx5R16(box + 0x48) == 'N' && box[0x15] == 125);
    /* Karrablast for Shelmet evolves; for another, or with the partner
     * holding an Everstone, it does not. Shelmet for Karrablast too. */
    spec = Basic(588);
    Offered(&entry, 3, 616, 0);
    Match(&entry, &spec);
    Deliver(&entry);
    Decoded(Fx5ImageParty(&image, 2), box, NULL);
    assert(Fx5R16(box + 8) == 589);
    Offered(&entry, 3, 616, EVERSTONE);
    Match(&entry, &spec);
    Deliver(&entry);
    Decoded(Fx5ImageParty(&image, 2), box, NULL);
    assert(Fx5R16(box + 8) == 588);
    Offered(&entry, 3, 501, 0);
    Match(&entry, &spec);
    Deliver(&entry);
    Decoded(Fx5ImageParty(&image, 2), box, NULL);
    assert(Fx5R16(box + 8) == 588);
    spec = Basic(616);
    Offered(&entry, 3, 588, 0);
    Match(&entry, &spec);
    Deliver(&entry);
    Decoded(Fx5ImageParty(&image, 2), box, NULL);
    assert(Fx5R16(box + 8) == 617);
    printf("PASS %s trade evolutions: Kadabra despite an Everstone, Everstones stop others, Metal Coat is used, Karrablast and Shelmet need each other\n",
           FX5_NAME);
}

static void Pokedex(void)
{
    unsigned char dex[FX5_POKEDEX_SIZE], box[136];
    Spec spec;
    CpReceive5Game which = B2W2 ? CP_RECEIVE_BLACK2_WHITE2 : CP_RECEIVE_BLACK_WHITE;
    unsigned formsNormal = 0x2FC, formsShiny = B2W2 ? 0x307 : 0x305, shown = B2W2 ? 0x312 : 0x30E;
    memset(dex, 0, sizeof(dex));
    /* Heat Rotom, genderless, shiny: male rows, its form bit 36 + 1. */
    spec = Basic(479); spec.form = 1;
    spec.pid = 0x0BADF00D ^ 3; spec.ot = spec.pid;
    Canonical(&spec, box);
    assert(CpReceive5_Register(which, &data, dex, box) == 0);
    assert(Bit(dex + 0x5C + 2 * 0x54, 478) && !Bit(dex + 0x5C, 478));
    assert(Bit(dex + 0x1AC + 2 * 0x54, 478));
    assert(Bit(dex + formsShiny, 37) && !Bit(dex + formsNormal, 37) && Bit(dex + (B2W2 ? 0x31D : 0x317), 37));
    /* A second one, not shiny: the shown flags stay, the seen form adds. */
    spec.form = 0; spec.pid = 0x22222222; spec.ot = 0x0BADF00D;
    Canonical(&spec, box);
    assert(CpReceive5_Register(which, &data, dex, box) == 0);
    assert(Bit(dex + 0x5C, 478) && Bit(dex + formsNormal, 36) && !Bit(dex + shown, 36));
    /* Spinda's first personality is kept. */
    spec = Basic(327); spec.pid = 0xCAFE1234;
    Canonical(&spec, box);
    assert(CpReceive5_Register(which, &data, dex, box) == 0);
    spec.pid = 0x11111111;
    Canonical(&spec, box);
    assert(CpReceive5_Register(which, &data, dex, box) == 0);
    assert(Fx5R32(dex + (B2W2 ? 0x4D8 : 0x4D0)) == 0xCAFE1234);
    /* Black/White record the language: Spanish (7) is the sixth bit. */
    spec = Basic(25); spec.language = 7;
    Canonical(&spec, box);
    assert(CpReceive5_Register(which, &data, dex, box) == 0);
    assert(Bit(dex + (B2W2 ? 0x328 : 0x320), 5 + 24 * 7) && Bit(dex + 0x008, 24));
    /* Language 0: Black/White refuse it (their game would write outside the
     * block); Black 2/White 2 skip the language and register the rest. */
    spec = Basic(26); spec.language = 0;
    Canonical(&spec, box);
    if (B2W2) {
        unsigned i, any = 0;
        assert(CpReceive5_Register(which, &data, dex, box) == 0 && Bit(dex + 0x008, 25));
        for (i = 0; i < 7; ++i) any |= Bit(dex + 0x328, i + 25 * 7);
        assert(!any);
    } else {
        assert(CpReceive5_Register(which, &data, dex, box) == -1);
    }
    /* Only Black 2/White 2 know Kyurem's forms. */
    assert(CpReceive5_Forms(which, 646) == (B2W2 ? 3u : 1u) && CpReceive5_Forms(which, 493) == 1);
    printf("PASS %s Pokédex: seen, shown, form, Spinda, language and caught bits as the retail trades set them\n", FX5_NAME);
}

static void Returned(void)
{
    WtJournalEntry entry;
    unsigned char before[FX5_MON];
    Offered(&entry, 3, 501, 0);
    memcpy(before, entry.original, FX5_MON);
    entry.deliveryKind = WT_DELIVERY_RETURN;
    memcpy(entry.incoming, entry.outgoing, 136);
    entry.phase = WT_JOURNAL_RECEIVED;
    Deliver(&entry);
    assert(memcmp(Fx5ImageParty(&image, 2), before, FX5_MON) == 0);
    printf("PASS %s returned offer: put back exactly as it left\n", FX5_NAME);
}

/* ---- Stock-game saves between steps ------------------------------------------ */

/* The stock game loads the save, changes block `id` and saves it (count + 1). */
static void StockSave(unsigned id)
{
    unsigned char digest[32];
    assert(Fx5Load(&check, &cpFile) == CP_SAVE_OK);
    CpSaveImage_Block(&check, id)[3] ^= 0x5A;
    assert(CpSaveImage_Hash(&check, digest) == CP_SAVE_OK && CpSaveImage_CommitBoth(&check, &cpFile, digest) == CP_SAVE_OK);
    assert(Fx5Load(&image, &cpFile) == CP_SAVE_OK);
}

static void StockGame(void)
{
    WtJournalEntry entry;
    unsigned char box[136];
    Spec spec = Basic(500);
    Fx5Spec specs[3];
    /* REMOVE after the stock game saved with the offer still in the party. */
    Defaults(specs, 3);
    Setup(specs, 3);
    memset(&entry, 0, sizeof(entry));
    assert(Step(WT_INVENTORY_PREPARE_REMOVE, &entry, 1) == 0);
    StockSave(40);
    assert(Step(WT_INVENTORY_REMOVE, &entry, 1) == 0 && Fx5ImageCount(&image) == 2);
    BothCopies();
    /* RECEIVE after the stock game saved: received once, never twice. */
    Match(&entry, &spec);
    assert(Step(WT_INVENTORY_PREPARE_RECEIVE, &entry, 0) == 0);
    StockSave(40);
    assert(Step(WT_INVENTORY_RECEIVE, &entry, 0) == 0 && Fx5ImageCount(&image) == 3);
    StockSave(41);
    assert(Step(WT_INVENTORY_RECEIVE, &entry, 0) == 0 && Fx5ImageCount(&image) == 3);
    assert(Step(WT_INVENTORY_VERIFY_RECEIVED, &entry, 0) == 0);
    /* The delivered Pokémon moved to the Battle Box: recorded there, never
     * received again. */
    Decoded(Fx5ImageParty(&image, 2), box, NULL);
    assert(WtPokemon_Encode(box, Fx5ImageBattleBox(&image, 0)) == 0);
    Fx5Empty(Fx5ImageParty(&image, 2), FX5_MON);
    Fx5W32(CpSaveImage_Block(&image, FX5_PARTY) + 4, 2);
    {
        unsigned char digest[32];
        assert(CpSaveImage_Hash(&image, digest) == CP_SAVE_OK && CpSaveImage_CommitBoth(&image, &cpFile, digest) == CP_SAVE_OK);
    }
    assert(Step(WT_INVENTORY_RECEIVE, &entry, 0) == 0 && Fx5ImageCount(&image) == 2);
    assert(entry.destination.kind == 3 && entry.destination.box == 0 && entry.destination.slot == 0
        && entry.destination.length == 136);
    assert(memcmp(entry.destinationAfter, Fx5ImageBattleBox(&image, 0), 136) == 0);
    assert(Step(WT_INVENTORY_VERIFY_RECEIVED, &entry, 0) == 0 && Step(WT_INVENTORY_RECEIVE, &entry, 0) == 0);
    assert(Fx5ImageCount(&image) == 2);
    entry.phase = WT_JOURNAL_APPLIED;
    assert(WtGame_CanLeave(&game, &entry) == 1);
    {
        WtReceivedPokemon received;
        assert(WtGame_Received(&entry, &received) == 0 && received.species == 500);
    }
    printf("PASS %s stock-game saves: a removal and a delivery finish once; one moved to the Battle Box is recorded there\n",
           FX5_NAME);
}

/* ---- Holding ------------------------------------------------------------------- */

static void Holding(void)
{
    unsigned char box[136];
    Spec spec = Basic(500);
    Setup(NULL, 0);
    Canonical(&spec, box);
    assert(WtGame_CanHold(&game, box));
    spec.species = 650; Canonical(&spec, box); assert(!WtGame_CanHold(&game, box));
    spec = Basic(500); spec.item = B2W2 ? 639 : 627; Canonical(&spec, box); assert(!WtGame_CanHold(&game, box));
    spec = Basic(500); spec.item = 300; Canonical(&spec, box); assert(!WtGame_CanHold(&game, box));
    spec = Basic(500); spec.language = 0; Canonical(&spec, box); assert(WtGame_CanHold(&game, box) == B2W2);
    spec = Basic(493); spec.form = 1; Canonical(&spec, box); assert(!WtGame_CanHold(&game, box));
    spec = Basic(646); spec.form = 2; Canonical(&spec, box); assert(WtGame_CanHold(&game, box) == B2W2);
    spec = Basic(479); spec.form = 5; Canonical(&spec, box); assert(WtGame_CanHold(&game, box));
    printf("PASS %s holding: species, items, forms and languages the game knows\n", FX5_NAME);
}

/* ---- The musical distribution area --------------------------------------------- */

static void SealMusical(uint32_t count)
{
    unsigned char *base = file.bytes + FX5_MUSICAL, *footer = base + FX5_MUSICAL_USED - 16, *table = footer - 4;
    unsigned crc;
    memset(base, 0x42, FX5_MUSICAL_BLOCK);
    crc = Fx5Crc(base, FX5_MUSICAL_BLOCK);
    Fx5W16(base + FX5_MUSICAL_BLOCK, 1); Fx5W16(base + FX5_MUSICAL_BLOCK + 2, crc);
    Fx5W16(table, crc); Fx5W16(table + 2, 0);
    Fx5W32(footer, count); Fx5W32(footer + 4, FX5_MUSICAL_USED); Fx5W32(footer + 8, CP_SAVE_MAGIC_GEN5);
    Fx5W16(footer + 14, Fx5Crc(table, 4));
}

static void Mark(uint32_t count, unsigned flag)
{
    unsigned char *trainer = Fx5Block(&file, 0, FX5_TRAINER);
    Fx5W16(trainer + 0x30 + 12, 0xC21E);
    Fx5W32(trainer + FX5_COUNTS + 24, count);
    Fx5Block(&file, 0, FX5_MUSICAL_SAVE)[0x29E] = (unsigned char)flag;
    Fx5SealBoth(&file, 40);
}

static void Musical(void)
{
    static unsigned char before[CP_SAVE_FILE_SIZE];
    const CpJournalArea *area = CpSaveJournal_Area(FX5_LAYOUT);
    unsigned i;
    /* A downloaded musical the game would load: refused, nothing written. */
    Fx5Build(&file, NULL, 0, 40);
    SealMusical(9);
    Mark(9, 1);
    assert(Fx5Load(&image, &cpFile) == CP_SAVE_OK && CpGen5_MusicalSaved(&image));
    game.image = &image; game.file = &cpFile;
    memcpy(before, file.bytes, sizeof(before));
    file.writes = 0;
    assert(CpGen5_ReleaseMusical(&game) == CP_GEN5_MUSICAL_SAVED && file.writes == 0);
    assert(memcmp(before, file.bytes, sizeof(before)) == 0);
    /* Its banks hold a journal-free leftover the game no longer reads: the
     * marker (whose count no longer matches) is cleared in both copies, then
     * the banks erased. */
    Fx5Build(&file, NULL, 0, 40);
    SealMusical(9);
    Mark(8, 1);
    for (i = 0; i < 0x2000; ++i) file.bytes[FX5_JOURNAL + i] = (unsigned char)i;
    assert(Fx5Load(&image, &cpFile) == CP_SAVE_OK && !CpGen5_MusicalSaved(&image));
    assert(CpSaveImage_Block(&image, FX5_MUSICAL_SAVE)[0x29E] == 0);
    assert(CpSaveJournal_DetectArea(&cpFile, area) == CP_SAVE_JOURNAL_UNUSED);
    assert(CpGen5_ReleaseMusical(&game) == 0);
    assert(CpMusicalArea_Inspect(&cpFile, area) == CP_MUSICAL_FREE);
    assert(Fx5Load(&check, &cpFile) == CP_SAVE_OK && check.globalCounter == 41);
    assert(Fx5R16(CpSaveImage_Block(&check, FX5_TRAINER) + 0x3C) == 0 && CpSaveImage_Block(&check, FX5_MUSICAL_SAVE)[0x29E] == 0);
    BothCopies();
    /* With the journal in place, nothing is written. */
    memcpy(file.bytes + FX5_JOURNAL, "DSWJ", 4);
    file.writes = 0;
    assert(CpGen5_ReleaseMusical(&game) == 0 && file.writes == 0);
    printf("PASS %s musical area: a loadable musical refuses the save untouched; a stale marker is cleared in both copies before leftovers are erased\n",
           FX5_NAME);
}

int main(void)
{
    Eligibility();
    RemoveAndRestore();
    PartyDelivery();
    BoxDelivery();
    Evolutions();
    Pokedex();
    Returned();
    StockGame();
    Holding();
    Musical();
    return 0;
}
