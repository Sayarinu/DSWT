#include "adapter/gen5_receive.h"

#include <string.h>

#include "wonder_trade/pokemon_codec.h"

/* The canonical PK5 (blocks A, B, C, D at 0x08, 0x28, 0x48, 0x68) and the
 * decrypted battle stats that follow it in a party image (0x88..0xDB). */
enum {
    PK_PID = 0x00, PK_FLAGS = 0x04, PK_CHECKSUM = 0x06,
    PK_SPECIES = 0x08, PK_ITEM = 0x0A, PK_OT_ID = 0x0C, PK_EXP = 0x10, PK_FRIENDSHIP = 0x14,
    PK_ABILITY = 0x15, PK_LANGUAGE = 0x17, PK_EVS = 0x18,
    PK_MOVES = 0x28, PK_PP = 0x30, PK_PP_UPS = 0x34, PK_IVS = 0x38, PK_FORM = 0x40, PK_NATURE = 0x41,
    PK_HIDDEN = 0x42, PK_NICKNAME = 0x48, PK_ORIGIN = 0x5F,
    BOX = 136, PARTY = 220, EXTENSION = PARTY - BOX,
    EXT_STATUS = 0, EXT_LEVEL = 4, EXT_HP = 6, EXT_MAX_HP = 8, EXT_STATS = 10,
    NAME = 11,
    SPECIES_KADABRA = 64, SPECIES_SHEDINJA = 292, SPECIES_SPINDA = 327, SPECIES_CASTFORM = 351,
    SPECIES_CHERRIM = 421, SPECIES_SHAYMIN = 492, SPECIES_KARRABLAST = 588, SPECIES_SHELMET = 616, SPECIES_DARMANITAN = 555,
    SPECIES_MELOETTA = 648, SPECIES_LAST = 649, SPECIES_EGG = 650,
    HOLD_NO_EVOLVE = 64,
    EVO_TRADE = 5, EVO_TRADE_ITEM = 6, EVO_TRADE_SPECIES = 7,
    VERSION_WHITE = 20
};

static unsigned Read16(const unsigned char *p) { return p[0] | ((unsigned)p[1] << 8); }
static uint32_t Read32(const unsigned char *p) { return Read16(p) | ((uint32_t)Read16(p + 2) << 16); }
static void Write16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }

static void Write32(unsigned char *p, uint32_t v)
{
    Write16(p, v & 0xFFFF);
    Write16(p + 2, v >> 16);
}

/* PML_CryptoRun / XorBlock: halfwords XORed with the LCG's high halves. */
static void Crypt(unsigned char *bytes, size_t size, uint32_t seed)
{
    size_t i;
    for (i = 0; i + 1 < size; i += 2) {
        seed = seed * UINT32_C(0x41C64E6D) + 0x6073;
        bytes[i] ^= (unsigned char)(seed >> 16);
        bytes[i + 1] ^= (unsigned char)(seed >> 24);
    }
}

void CpReceive5_Empty(unsigned char image[220])
{
    memset(image, 0, PARTY);
    Crypt(image + 8, BOX - 8, 0);
    Crypt(image + BOX, EXTENSION, 0);
}

/* ---- Game data ------------------------------------------------------------ */

typedef struct Mon {
    unsigned char box[BOX], ext[EXTENSION];
    const CpAdapterData *data;
    unsigned itemLast;
} Mon;

static unsigned Species(const Mon *mon) { return Read16(mon->box + PK_SPECIES); }
static unsigned Form(const unsigned char *box) { return box[PK_FORM] >> 3; }
static uint32_t Pid(const unsigned char *box) { return Read32(box + PK_PID); }
static int Egg(const unsigned char *box) { return (Read32(box + PK_IVS) >> 30) & 1; }

static int Personal(const CpAdapterData *data, unsigned species, unsigned form, CpPersonal *out)
{
    return data->personal(data->arg, (uint16_t)species, (uint8_t)form, out) == CP_OK ? 0 : -1;
}

/* PML_UtilDerivePkmSex / CalcGender. */
static unsigned Gender(unsigned ratio, uint32_t pid)
{
    if (ratio == 0) return 0;
    if (ratio == 254) return 1;
    if (ratio == 255) return 2;
    return (pid & 0xFF) < ratio ? 1 : 0;
}

static int HoldEffect(const CpAdapterData *data, unsigned item, unsigned itemLast, unsigned *out)
{
    uint8_t effect = 0;
    int result;
    *out = 0;
    if (item == 0 || item > itemLast) return 0;
    result = data->holdEffect(data->arg, (uint16_t)item, &effect);
    if (result == CP_ERR_NOT_FOUND) return 0;
    if (result != CP_OK) return -1;
    *out = effect;
    return 0;
}

/* PML_MoveGetMaxPP / WAZA_GetMaxPP. */
static int MaxPp(const CpAdapterData *data, unsigned move, unsigned ups, unsigned *out)
{
    uint8_t base;
    if (data->movePp(data->arg, (uint16_t)move, &base) != CP_OK) return -1;
    if (ups > 3) ups = 3;
    *out = (unsigned char)(base + base * 20u * ups / 100u);
    return 0;
}

/* ---- Party parameters ----------------------------------------------------- */

/* adjustStatForNature: Atk, Def, Spe, SpA, SpD raised by nature / 5 and
 * lowered by nature % 5, the product truncated to 16 bits before dividing. */
static unsigned Nature(unsigned nature, unsigned value, unsigned stat)
{
    unsigned up = nature / 5, down = nature % 5;
    if (nature >= 25 || up == down) return value;
    if (stat == up) return (uint16_t)(value * 110) / 100u;
    if (stat == down) return (uint16_t)(value * 90) / 100u;
    return value;
}

/* POKE_TOOL_RecalcParam / PokeParty_RecalcStats. */
static int Recalc(Mon *mon)
{
    static const unsigned order[5] = { 1, 2, 3, 4, 5 }; /* base stats Atk, Def, Spe, SpA, SpD */
    CpPersonal personal;
    uint32_t table[CP_MAX_LEVEL + 1], exp = Read32(mon->box + PK_EXP), ivs = Read32(mon->box + PK_IVS);
    unsigned species = Species(mon), level, i, maxHp, hp, oldMax;
    int change;
    if (Personal(mon->data, species, Form(mon->box), &personal) != 0
        || mon->data->expTable(mon->data->arg, personal.growthRate, table) != CP_OK) return -1;
    for (level = 1; level < 101; ++level) if (table[level] > exp) break;
    level -= 1;
    mon->ext[EXT_LEVEL] = (unsigned char)level;
    hp = Read16(mon->ext + EXT_HP);
    oldMax = Read16(mon->ext + EXT_MAX_HP);
    maxHp = species == SPECIES_SHEDINJA ? 1
        : (2 * personal.baseStats[0] + (ivs & 31) + mon->box[PK_EVS] / 4u) * level / 100 + level + 10;
    Write16(mon->ext + EXT_MAX_HP, maxHp);
    for (i = 0; i < 5; ++i) {
        unsigned iv = (ivs >> (5 * (i + 1))) & 31, ev = mon->box[PK_EVS + 1 + i];
        unsigned value = (2 * personal.baseStats[order[i]] + iv + ev / 4) * level / 100 + 5;
        Write16(mon->ext + EXT_STATS + 2 * i, Nature(mon->box[PK_NATURE], (uint16_t)value, i));
    }
    if (hp != 0 || oldMax == 0) {
        if (species == SPECIES_SHEDINJA) hp = 1;
        else if (hp == 0) hp = maxHp;
        else {
            change = (int)maxHp - (int)oldMax;
            if (change < 0) { if (hp > maxHp) hp = maxHp; }
            else hp += (unsigned)change;
        }
    }
    if (hp != 0) Write16(mon->ext + EXT_HP, hp);
    return 0;
}

/* POKE_TOOL_HealFull / PokeParty_Recover: HP, status, then any move's PP below its maximum. */
static int Heal(Mon *mon)
{
    unsigned i, max;
    Write16(mon->ext + EXT_HP, Read16(mon->ext + EXT_MAX_HP));
    Write32(mon->ext + EXT_STATUS, 0);
    for (i = 0; i < 4; ++i) {
        unsigned move = Read16(mon->box + PK_MOVES + 2 * i);
        if (!move) continue;
        if (MaxPp(mon->data, move, mon->box[PK_PP_UPS + i], &max) != 0) return -1;
        if (mon->box[PK_PP + i] < max) mon->box[PK_PP + i] = (unsigned char)max;
    }
    return 0;
}

/* POKE_TOOL_RecoverBoxAllPP / PML_PkmPPRecover: every move's maximum. */
static int RestorePp(Mon *mon)
{
    unsigned i, max;
    for (i = 0; i < 4; ++i) {
        unsigned move = Read16(mon->box + PK_MOVES + 2 * i);
        if (!move) continue;
        if (MaxPp(mon->data, move, mon->box[PK_PP_UPS + i], &max) != 0) return -1;
        mon->box[PK_PP + i] = (unsigned char)max;
    }
    return 0;
}

/* PKM_PARAM_ITEM: an ID past the game's items reads as none. */
static unsigned Item(const unsigned char *box, unsigned itemLast)
{
    unsigned item = Read16(box + PK_ITEM);
    return item > itemLast ? 0 : item;
}

/* SHINKA_CHECK_CheckEvolution / CheckEvolveSpecies in trade mode: the first
 * entry that applies, and its method; 0 for none. */
static int TradeTarget(const Mon *mon, const unsigned char partner[PARTY], unsigned *target, unsigned *method)
{
    CpEvolution rows[CP_EVOLUTIONS];
    unsigned char other[BOX];
    unsigned item = Item(mon->box, mon->itemLast), effect, species = Species(mon), i, otherSpecies = 0, otherEffect = 0;
    int otherRead = 0;
    *target = *method = 0;
    if (Egg(mon->box)) return 0;
    if (HoldEffect(mon->data, item, mon->itemLast, &effect) != 0) return -1;
    if (species != SPECIES_KADABRA && effect == HOLD_NO_EVOLVE) return 0;
    if (!mon->data->evolutions || mon->data->evolutions(mon->data->arg, (uint16_t)species, rows) != CP_OK) return -1;
    for (i = 0; i < CP_EVOLUTIONS; ++i) {
        unsigned result = 0;
        if (rows[i].method == EVO_TRADE) {
            result = rows[i].target;
        } else if (rows[i].method == EVO_TRADE_ITEM) {
            if (item == rows[i].parameter) result = rows[i].target;
        } else if (rows[i].method == EVO_TRADE_SPECIES) {
            if (!otherRead) {
                /* The partner as the game reads it: a bad checksum makes a bad egg. */
                if (WtPokemon_Decode(partner, other) != 0) return -1;
                otherSpecies = Read16(other + PK_SPECIES);
                if (otherSpecies && Egg(other)) otherSpecies = SPECIES_EGG;
                if (HoldEffect(mon->data, Item(other, mon->itemLast), mon->itemLast, &otherEffect) != 0) return -1;
                otherRead = 1;
            }
            if (otherEffect != HOLD_NO_EVOLVE
                && ((species == SPECIES_KARRABLAST && otherSpecies == SPECIES_SHELMET)
                    || (species == SPECIES_SHELMET && otherSpecies == SPECIES_KARRABLAST)))
                result = rows[i].target;
        }
        if (result) {
            *target = result;
            *method = rows[i].method;
            return 0;
        }
    }
    return 0;
}

/* POKE_TOOL_ChangeSpecies / setChangedPkmSpecies. */
static int ChangeSpecies(Mon *mon, unsigned target)
{
    CpPersonal now, before;
    uint16_t name[NAME + 1];
    unsigned old = Species(mon), form = Form(mon->box), i, newRatio;
    uint32_t mask, ivs = Read32(mon->box + PK_IVS);
    int nicknamed = (int)(ivs >> 31);
    size_t length = 0;
    if (old == target) return 0;
    if (Personal(mon->data, target, form, &now) != 0 || Personal(mon->data, old, form, &before) != 0) return -1;
    Write16(mon->box + PK_SPECIES, target);
    mask = mon->box[PK_ORIGIN] < VERSION_WHITE ? 1 : 0x10000;
    if (mon->box[PK_HIDDEN] & 1) mon->box[PK_ABILITY] = now.hiddenAbility;
    else mon->box[PK_ABILITY] = now.abilities[1] && (Pid(mon->box) & mask) ? now.abilities[1] : now.abilities[0];
    newRatio = now.genderRatio;
    if ((newRatio != 0 && newRatio != 254 && newRatio != 255) || before.genderRatio == 255)
        mon->box[PK_FORM] = (unsigned char)((mon->box[PK_FORM] & ~6u) | Gender(newRatio, Pid(mon->box)) << 1);
    if (!nicknamed) {
        /* WORDSET_CopyString: up to and with the first terminator, at most 11;
         * the codes after it stay, then the last code is the terminator. */
        if (mon->data->speciesName(mon->data->arg, (uint16_t)target, name, sizeof name / sizeof name[0], &length)
            != CP_OK || length > NAME - 1) return -1;
        for (i = 0; i <= length && i < NAME; ++i) Write16(mon->box + PK_NICKNAME + 2 * i, name[i]);
        Write16(mon->box + PK_NICKNAME + 2 * (NAME - 1), 0xFFFF);
        /* The nickname is now the species' name. */
        Write32(mon->box + PK_IVS, Read32(mon->box + PK_IVS) & ~(UINT32_C(1) << 31));
    }
    return Recalc(mon);
}

/* POKE_TOOL_ChangeForm / PokeParty_ChangeForme: change the form, update
 * ability and gender from that form's personal data, then recalculate stats. */
static int ChangeForm(Mon *mon, unsigned form)
{
    CpPersonal personal;
    uint32_t mask = mon->box[PK_ORIGIN] < VERSION_WHITE ? 1 : 0x10000;
    if (Personal(mon->data, Species(mon), form, &personal) != 0) return -1;
    mon->box[PK_FORM] = (unsigned char)((mon->box[PK_FORM] & 7u) | (form << 3));
    if (mon->box[PK_HIDDEN] & 1) mon->box[PK_ABILITY] = personal.hiddenAbility;
    else mon->box[PK_ABILITY] = personal.abilities[1] && (Pid(mon->box) & mask)
        ? personal.abilities[1] : personal.abilities[0];
    if ((personal.genderRatio != 0 && personal.genderRatio != 254 && personal.genderRatio != 255)
        || personal.genderRatio == 255)
        mon->box[PK_FORM] = (unsigned char)((mon->box[PK_FORM] & ~6u) | Gender(personal.genderRatio, Pid(mon->box)) << 1);
    return Recalc(mon);
}

/* ---- Pokédex -------------------------------------------------------------- */

typedef struct Dex {
    unsigned seenForms, seenFormsShiny, shownForms, shownFormsShiny, languages, spinda;
    const uint16_t (*forms)[2];
    unsigned formCount;
} Dex;

enum { DEX_CAUGHT = 0x008, DEX_SEEN = 0x05C, DEX_SHOWN = 0x1AC, DEX_ROW = 0x54 };

/* ZUKAN_SAVE's form table (0x0209DE4C in Black) and getNumberOfForms':
 * {species, count}; each species' bits follow the one before. */
static const uint16_t kBlackWhiteForms[][2] = {
    { 201, 28 }, { 386, 4 }, { 492, 2 }, { 487, 2 }, { 479, 6 }, { 422, 2 }, { 423, 2 }, { 412, 3 },
    { 413, 3 }, { 351, 4 }, { 421, 2 }, { 585, 4 }, { 586, 4 }, { 648, 2 }, { 555, 2 }, { 550, 2 }
};

static const uint16_t kBlack2White2Forms[][2] = {
    { 201, 28 }, { 386, 4 }, { 492, 2 }, { 487, 2 }, { 479, 6 }, { 422, 2 }, { 423, 2 }, { 412, 3 },
    { 413, 3 }, { 351, 4 }, { 421, 2 }, { 585, 4 }, { 586, 4 }, { 648, 2 }, { 555, 2 }, { 550, 2 },
    { 646, 3 }, { 647, 2 }, { 642, 2 }, { 641, 2 }, { 645, 2 }
};

static const Dex kDex[2] = {
    { 0x2FC, 0x305, 0x30E, 0x317, 0x320, 0x4D0, kBlackWhiteForms,
      sizeof kBlackWhiteForms / sizeof kBlackWhiteForms[0] },
    { 0x2FC, 0x307, 0x312, 0x31D, 0x328, 0x4D8, kBlack2White2Forms,
      sizeof kBlack2White2Forms / sizeof kBlack2White2Forms[0] },
};

unsigned CpReceive5_Forms(CpReceive5Game game, unsigned species)
{
    const Dex *dex = &kDex[game == CP_RECEIVE_BLACK2_WHITE2];
    unsigned i;
    for (i = 0; i < dex->formCount; ++i) if (dex->forms[i][0] == species) return dex->forms[i][1];
    return 1;
}

/* The first bit of a species' forms and their count, 0 when it has none listed. */
static unsigned FormBase(const Dex *dex, unsigned species, unsigned *count)
{
    unsigned i, base = 0;
    for (i = 0; i < dex->formCount; ++i) {
        if (dex->forms[i][0] == species) { *count = dex->forms[i][1]; return base; }
        base += dex->forms[i][1];
    }
    *count = 0;
    return 0;
}

static void SetBit(unsigned char *bits, unsigned bit) { bits[bit >> 3] |= (unsigned char)(1u << (bit & 7)); }
static void ClearBit(unsigned char *bits, unsigned bit) { bits[bit >> 3] &= (unsigned char)~(1u << (bit & 7)); }
static int TestBit(const unsigned char *bits, unsigned bit) { return (bits[bit >> 3] >> (bit & 7)) & 1; }

int CpReceive5_Register(CpReceive5Game game, const CpAdapterData *data, unsigned char *pokedex,
                        const unsigned char box[136])
{
    const Dex *dex = &kDex[game == CP_RECEIVE_BLACK2_WHITE2];
    CpPersonal personal;
    unsigned species = Read16(box + PK_SPECIES), form = Form(box), gender, kind, count, base, i, bit;
    uint32_t pid = Pid(box), ot = Read32(box + PK_OT_ID);
    int shiny, seen = 0;
    if (Egg(box)) return 0;
    if (species == 0 || species > SPECIES_LAST) return 0;
    if (Personal(data, species, form, &personal) != 0) return -1;
    gender = Gender(personal.genderRatio, pid);
    shiny = ((ot >> 16) ^ (ot & 0xFFFF) ^ (pid >> 16) ^ (pid & 0xFFFF)) < 8;
    kind = (gender == 1) + 2u * (unsigned)shiny;
    bit = species - 1;
    base = FormBase(dex, species, &count);
    for (i = 0; i < 4; ++i) seen |= TestBit(pokedex + DEX_SEEN + i * DEX_ROW, bit);
    if (!seen) {
        /* The gender, shiny and form the Pokédex shows. */
        if (count) {
            for (i = 0; i < count; ++i) {
                ClearBit(pokedex + dex->shownForms, base + i);
                ClearBit(pokedex + dex->shownFormsShiny, base + i);
            }
            SetBit(pokedex + (shiny ? dex->shownFormsShiny : dex->shownForms), base + (form < count ? form : 0));
        }
        for (i = 0; i < 4; ++i) ClearBit(pokedex + DEX_SHOWN + i * DEX_ROW, bit);
        SetBit(pokedex + DEX_SHOWN + kind * DEX_ROW, bit);
    }
    if (species == SPECIES_SPINDA && Read32(pokedex + dex->spinda) == 0) Write32(pokedex + dex->spinda, pid);
    SetBit(pokedex + DEX_SEEN + kind * DEX_ROW, bit);
    if (count) {
        unsigned char *forms = pokedex + (shiny ? dex->seenFormsShiny : dex->seenForms);
        if (species == SPECIES_CASTFORM) for (i = 0; i < 4; ++i) SetBit(forms, base + i);
        else if (species == SPECIES_CHERRIM || species == SPECIES_DARMANITAN || species == SPECIES_MELOETTA)
            for (i = 0; i < 2; ++i) SetBit(forms, base + i);
        else SetBit(forms, base + (form < count ? form : 0));
    }
    {
        /* The language and the catch. Black/White's ZUKAN_SAVE_SetLanguageFlag
         * has no check for language 0 and would write outside the block:
         * refused; Black 2/White 2's PokeDex_SetLanguageSeen skips it. */
        unsigned language = box[PK_LANGUAGE];
        if (language == 0 && game == CP_RECEIVE_BLACK_WHITE) return -1;
        if (language != 0 && language <= 8 && language != 6 && species <= 493) {
            if (language >= 7) --language;
            SetBit(pokedex + dex->languages, (language - 1) + (species - 1) * 7);
        }
        SetBit(pokedex + DEX_CAUGHT, bit);
    }
    return 0;
}

/* ---- The delivery ----------------------------------------------------------- */

int CpReceive5_Apply(CpReceive5Game game, const CpAdapterData *data, unsigned char *pokedex,
                     const unsigned char incoming[136], const unsigned char partner[220], int destination,
                     unsigned char image[220])
{
    Mon mon;
    unsigned target, method, i;
    uint32_t sum = 0;
    if (!data || !pokedex || !incoming || !partner || !image
        || (destination != CP_RECEIVE_PARTY && destination != CP_RECEIVE_BOX)) return -1;
    memset(&mon, 0, sizeof(mon));
    mon.data = data;
    mon.itemLast = game == CP_RECEIVE_BLACK2_WHITE2 ? 638 : 626;
    memcpy(mon.box, incoming, BOX);
    /* The battle stats start as 84 zero bytes of ciphertext: their plaintext
     * is the PID's key stream, which fields never written keep. */
    Crypt(mon.ext, EXTENSION, Pid(mon.box));
    if (Read16(mon.box + PK_FLAGS) != 0 || Recalc(&mon) != 0 || Heal(&mon) != 0) return -1;
    mon.box[PK_FRIENDSHIP] = 70;
    /* The trade registers the Pokémon received, the evolution the species it became. */
    if (CpReceive5_Register(game, data, pokedex, mon.box) != 0) return -1;
    if (Species(&mon) == SPECIES_SHAYMIN && Form(mon.box) == 1) {
        if (ChangeForm(&mon, 0) != 0 || CpReceive5_Register(game, data, pokedex, mon.box) != 0) return -1;
    }
    if (TradeTarget(&mon, partner, &target, &method) != 0) return -1;
    if (target) {
        if (ChangeSpecies(&mon, target) != 0) return -1;
        if (method == EVO_TRADE_ITEM) Write16(mon.box + PK_ITEM, 0);
        if (CpReceive5_Register(game, data, pokedex, mon.box) != 0) return -1;
    }
    if (destination == CP_RECEIVE_BOX && RestorePp(&mon) != 0) return -1;
    for (i = 8; i < BOX; i += 2) sum += Read16(mon.box + i);
    Write16(mon.box + PK_CHECKSUM, sum & 0xFFFF);
    memset(image, 0, PARTY);
    if (WtPokemon_Encode(mon.box, image) != 0) return -1;
    if (destination == CP_RECEIVE_PARTY) {
        memcpy(image + BOX, mon.ext, EXTENSION);
        Crypt(image + BOX, EXTENSION, Pid(mon.box));
    }
    memset(&mon, 0, sizeof(mon));
    return 0;
}
