#include "adapter/platinum_receive.h"

#include <string.h>

#include "game/text.h"
#include "wonder_trade/pokemon_codec.h"

/* Effects the game has and this file leaves out: none. The mail sentence IDs
 * the game leaves unset are kept from the prepared image (CpReceive_Apply). */

enum {
    /* Canonical PK4 (block A at 0x08, B at 0x28, C at 0x48, D at 0x68). */
    PK_PID = 0x00, PK_CHECKSUM = 0x06, PK_SPECIES = 0x08, PK_ITEM = 0x0A, PK_OT_ID = 0x0C, PK_EXP = 0x10,
    PK_FRIENDSHIP = 0x14, PK_ABILITY = 0x15, PK_LANGUAGE = 0x17, PK_EVS = 0x18, PK_MOVES = 0x28,
    PK_PP = 0x30, PK_PP_UPS = 0x34, PK_IVS = 0x38, PK_FORM_BYTE = 0x40, PK_MET_PT = 0x46,
    PK_NICKNAME = 0x48, PK_OT_NAME = 0x68, PK_MET_DP = 0x80, PK_OT_GENDER = 0x84,
    /* PartyPokemon: status, level, capsule, HP, max HP, the five stats, mail, capsule seals. */
    PT_LEVEL = 4, PT_HP = 6, PT_MAX_HP = 8, PT_MAIL = 20,
    MAIL_SIZE = 56, MAIL_SENTENCES = 52 - 20,
    /* Pokédex (Pokedex in pokedex.h). */
    DEX_MAGIC = 0, DEX_CAUGHT = 4, DEX_SEEN = 68, DEX_GENDER = 132, DEX_GENDER_SECOND = 196,
    DEX_SPINDA = 260, DEX_SHELLOS = 264, DEX_GASTRODON = 265, DEX_BURMY = 266, DEX_WORMADAM = 267,
    DEX_UNOWN = 268, DEX_LANGUAGES = 296, DEX_ROTOM = 796, DEX_SHAYMIN = 800, DEX_GIRATINA = 801,
    /* GameRecords: 71 32-bit records, 77 16-bit ones and padding from 4 encrypted, then the seed. */
    REC_ENCODED = 4, REC_ENCODED_END = 440, REC_SEED = 440,
    REC_TRAINER_SCORE = 1, REC_POKEMON_EVOLVED = 12, REC_WIFI_TRADES = 24,
    /* Pokétch Pokémon History: 12 {species, icon, form} after the rest of the Pokétch. */
    POKETCH_HISTORY = 184, POKETCH_HISTORY_MAX = 12,
    /* Journal page 0 (SaveData_GetJournal): its two online events after the title and others. */
    JOURNAL_ONLINE = 28, JOURNAL_ONLINE_SIZE = 42, JOURNAL_ONLINE_MAX = 2, ONLINE_EVENT_GOT_POKEMON_FROM_TRADE = 7,
    /* VAR_ARCEUS_EVENT_STATE - VARS_START. */
    VAR_ARCEUS_EVENT_STATE = 86,
    SPECIES_KADABRA = 64, SPECIES_UNOWN = 201, SPECIES_SHEDINJA = 292, SPECIES_SPINDA = 327,
    SPECIES_DEOXYS = 386, SPECIES_BURMY = 412, SPECIES_WORMADAM = 413, SPECIES_SHELLOS = 422,
    SPECIES_GASTRODON = 423, SPECIES_ROTOM = 479, SPECIES_GIRATINA = 487, SPECIES_SHAYMIN = 492,
    SPECIES_ARCEUS = 493, SPECIES_EGG = 494, NATIONAL_DEX_COUNT = 493,
    HOLD_EFFECT_NO_EVOLVE = 64, BASE_FRIENDSHIP_VALUE = 70, LOCATION_HALL_OF_ORIGIN = 86,
    FATEFUL_ENCOUNTER_LOCATION = 3002, VERSION_PLATINUM = 12,
    SCORE_CAUGHT_SPECIES = 20, SCORE_GTS_TRADE_COMPLETE = 40,
    NAME_LENGTH = 10, TRAINER_NAME_LENGTH = 7
};

#define TRAINER_SCORE_LIMIT UINT32_C(99999999)
#define HIGH_LIMIT_U32 UINT32_C(999999999)
#define POKEDEX_MAGIC UINT32_C(0xBEEFCAFE)

static unsigned Read16(const unsigned char *p) { return p[0] | ((unsigned)p[1] << 8); }
static uint32_t Read32(const unsigned char *p) { return Read16(p) | ((uint32_t)Read16(p + 2) << 16); }
static void Write16(unsigned char *p, unsigned value) { p[0] = (unsigned char)value; p[1] = (unsigned char)(value >> 8); }
static void Write32(unsigned char *p, uint32_t value) { Write16(p, value & 65535); Write16(p + 2, value >> 16); }

/* EncodeData: each halfword XORed with the high half of the next LCG state. */
static void Crypt(unsigned char *bytes, size_t size, uint32_t seed)
{
    size_t i;
    for (i = 0; i + 1 < size; i += 2) {
        seed = seed * UINT32_C(1103515245) + 24691;
        Write16(bytes + i, Read16(bytes + i) ^ (seed >> 16));
    }
}

/* ---- The Pokémon --------------------------------------------------------- */

typedef struct Mon {
    unsigned char box[136], party[100];
} Mon;

static uint32_t Pid(const Mon *mon) { return Read32(mon->box + PK_PID); }
static unsigned Species(const Mon *mon) { return Read16(mon->box + PK_SPECIES); }
static unsigned Form(const unsigned char box[136]) { return box[PK_FORM_BYTE] >> 3; }
static int Egg(const Mon *mon) { return (Read32(mon->box + PK_IVS) >> 30) & 1; }

uint8_t CpReceive_Gender(uint8_t ratio, uint32_t personality)
{
    switch (ratio) {
    case 0: return 0;
    case 254: return 1;
    case 255: return 2;
    default: return ratio > (personality & 0xFF) ? 1 : 0;
    }
}

/* Pokemon_GetGender: from the species' own record, whatever the form. */
static int Gender(const CpAdapterData *data, unsigned species, uint32_t personality, uint8_t *gender)
{
    CpPersonal personal;
    if (data->personal(data->arg, (uint16_t)species, 0, &personal) != CP_OK) return -1;
    *gender = CpReceive_Gender(personal.genderRatio, personality);
    return 0;
}

/* Pokemon_GetNatureStatValue: stat 1..5 is Attack, Defense, Speed, Sp. Atk, Sp. Def;
 * nature n raises stat n / 5 + 1 and lowers n % 5 + 1, neither when they match. */
static unsigned Nature(unsigned nature, unsigned value, unsigned stat)
{
    uint16_t result;
    unsigned up = nature / 5 + 1, down = nature % 5 + 1;
    if (up == down || (stat != up && stat != down)) return value & 0xFFFF;
    result = (uint16_t)((value & 0xFFFF) * (stat == up ? 110 : 90));
    return result / 100u;
}

int CpReceive_LevelAndStats(const CpAdapterData *data, const unsigned char box[136], unsigned char party[100])
{
    static const unsigned order[6] = { 0, 1, 2, 3, 4, 5 }; /* HP, Atk, Def, Speed, Sp. Atk, Sp. Def */
    CpPersonal personal;
    uint32_t table[CP_MAX_LEVEL + 1], exp = Read32(box + PK_EXP), ivs = Read32(box + PK_IVS);
    unsigned species = Read16(box + PK_SPECIES), level, i, maxHp, hp, nature = Read32(box + PK_PID) % 25;
    int newMaxHp = 0;
    /* Pokemon_GetSpeciesLevelAt: the species' own curve. */
    if (data->personal(data->arg, (uint16_t)species, 0, &personal) != CP_OK
        || data->expTable(data->arg, personal.growthRate, table) != CP_OK) return -1;
    for (level = 1; level < CP_MAX_LEVEL + 1; ++level) if (table[level] > exp) break;
    party[PT_LEVEL] = (unsigned char)(level - 1);
    /* Pokemon_CalcStats: the form's own record. */
    level = party[PT_LEVEL];
    maxHp = Read16(party + PT_MAX_HP); hp = Read16(party + PT_HP);
    if (data->personal(data->arg, (uint16_t)species, (uint8_t)Form(box), &personal) != CP_OK) return -1;
    for (i = 0; i < 6; ++i) {
        unsigned iv = (ivs >> (5 * order[i])) & 31, ev = box[PK_EVS + order[i]];
        int stat = (2 * personal.baseStats[i] + (int)iv + (int)ev / 4) * (int)level / 100;
        if (i == 0) {
            newMaxHp = species == SPECIES_SHEDINJA ? 1 : stat + (int)level + 10;
            Write16(party + PT_MAX_HP, (unsigned)newMaxHp);
        } else {
            Write16(party + PT_MAX_HP + 2 * i, Nature(nature, (unsigned)(stat + 5), i));
        }
    }
    if (hp != 0 || maxHp == 0) {
        if (species == SPECIES_SHEDINJA) hp = 1;
        else if (hp == 0) hp = (unsigned)newMaxHp;
        else hp += (unsigned)newMaxHp - maxHp;
    }
    if (hp) Write16(party + PT_HP, hp & 0xFFFF);
    return 0;
}

/* BoxPokemon_CalcAbility: the second ability for odd personalities when there is one. */
static int Ability(const CpAdapterData *data, Mon *mon)
{
    CpPersonal personal;
    if (data->personal(data->arg, (uint16_t)Species(mon), (uint8_t)Form(mon->box), &personal) != CP_OK) return -1;
    mon->box[PK_ABILITY] = personal.abilities[1] && (Pid(mon) & 1) ? personal.abilities[1] : personal.abilities[0];
    return 0;
}

/* MON_DATA_SPECIES_NAME: the species name and its terminator over the start of
 * the nickname. A name that would not fit leaves the nickname (the game asserts). */
static int SpeciesName(const CpAdapterData *data, Mon *mon)
{
    uint16_t name[NAME_LENGTH + 1];
    size_t length, i;
    int result = data->speciesName(data->arg, (uint16_t)Species(mon), name, NAME_LENGTH + 1, &length);
    if (result == CP_ERR_RANGE) return 0;
    if (result != CP_OK) return -1;
    for (i = 0; i <= length; ++i) Write16(mon->box + PK_NICKNAME + 2 * i, name[i]);
    return 0;
}

/* MoveTable_CalcMaxPP. */
static int MaxPp(const CpAdapterData *data, unsigned move, unsigned ups, uint8_t *out)
{
    uint8_t pp;
    if (data->movePp(data->arg, (uint16_t)move, &pp) != CP_OK) return -1;
    if (ups > 3) ups = 3;
    *out = (uint8_t)(pp + pp * 20 * ups / 100);
    return 0;
}

/* ---- Pokédex ------------------------------------------------------------- */

static int Bit(const unsigned char *array, unsigned index) { --index; return (array[index >> 3] >> (index & 7)) & 1; }
static void SetBit(unsigned char *array, unsigned index, unsigned value)
{
    --index;
    array[index >> 3] = (unsigned char)((array[index >> 3] & ~(1u << (index & 7))) | (value << (index & 7)));
}

static int Seen(const unsigned char *dex, unsigned species) { return Bit(dex + DEX_SEEN, species); }

static unsigned char *TwoForms(unsigned char *dex, unsigned species)
{
    switch (species) {
    case SPECIES_SHELLOS: return dex + DEX_SHELLOS;
    case SPECIES_GASTRODON: return dex + DEX_GASTRODON;
    case SPECIES_SHAYMIN: return dex + DEX_SHAYMIN;
    default: return dex + DEX_GIRATINA;
    }
}

/* UpdateForms_TwoForms: the first form seen fills both entries. */
static void UpdateTwoForms(unsigned char *dex, unsigned species, unsigned form)
{
    unsigned char *forms = TwoForms(dex, species);
    unsigned seen = !Seen(dex, species) ? 0 : Bit(forms, 1) == Bit(forms, 2) ? 1 : 2, i;
    for (i = 0; i < seen; ++i) if ((unsigned)Bit(forms, i + 1) == form) return;
    if (seen < 2) {
        SetBit(forms, seen + 1, form & 1);
        if (seen == 0) SetBit(forms, seen + 2, form & 1);
    }
}

/* UpdateForms_ThreeForms: three 2-bit entries, 3 marking the first unused. */
static void UpdateThreeForms(unsigned char *dex, unsigned species, unsigned form)
{
    unsigned char *forms = dex + (species == SPECIES_BURMY ? DEX_BURMY : DEX_WORMADAM);
    unsigned seen = 0, i;
    if (Seen(dex, species)) {
        for (i = 0; i < 3; ++i) if (((*forms >> (2 * i)) & 3) == form) return;
        while (seen < 3 && ((*forms >> (2 * seen)) & 3) != 3) ++seen;
    }
    if (seen < 3) *forms = (unsigned char)((*forms & ~(3u << (2 * seen))) | ((form & 3) << (2 * seen)));
}

/* Deoxys: four 4-bit entries in the top byte of the last caught and seen words, 15 unused. */
static unsigned char *DeoxysWord(unsigned char *dex, unsigned index)
{
    return dex + (index < 2 ? DEX_CAUGHT : DEX_SEEN) + 60;
}

static unsigned DeoxysForm(unsigned char *dex, unsigned index)
{
    return (Read32(DeoxysWord(dex, index)) >> (24 + 4 * (index & 1))) & 15;
}

static void UpdateDeoxys(unsigned char *dex, unsigned form)
{
    unsigned i, seen = 0;
    unsigned char *word;
    for (i = 0; i < 4; ++i) if (DeoxysForm(dex, i) == form) return;
    while (seen < 4 && DeoxysForm(dex, seen) != 15) ++seen;
    /* With all four used the game shifts by 32, which on the DS changes nothing. */
    if (seen == 4) return;
    word = DeoxysWord(dex, seen);
    Write32(word, (Read32(word) & ~(UINT32_C(15) << (24 + 4 * (seen & 1)))) | ((uint32_t)form << (24 + 4 * (seen & 1))));
}

/* UpdateForms_Rotom: six 3-bit entries, 7 unused. */
static void UpdateRotom(unsigned char *dex, unsigned form)
{
    uint32_t forms = Read32(dex + DEX_ROTOM);
    unsigned seen = 0, i;
    if (Seen(dex, SPECIES_ROTOM)) {
        while (seen < 6 && ((forms >> (3 * seen)) & 7) != 7) ++seen;
        for (i = 0; i < seen; ++i) if (((forms >> (3 * i)) & 7) == form) return;
    }
    if (seen < 6) Write32(dex + DEX_ROTOM, (forms & ~(UINT32_C(7) << (3 * seen))) | ((uint32_t)(form & 7) << (3 * seen)));
}

/* SetUnownForm: forms in the order first seen, 0xFF unused. */
static void UpdateUnown(unsigned char *dex, unsigned form)
{
    unsigned i;
    for (i = 0; i < 28; ++i) if (dex[DEX_UNOWN + i] == form) return;
    for (i = 0; i < 28; ++i) if (dex[DEX_UNOWN + i] == 0xFF) { dex[DEX_UNOWN + i] = (unsigned char)form; return; }
}

/* PokedexLanguage_LanguageToIndex: Japanese, English, French, German, Italian, Spanish. */
static int LanguageIndex(unsigned language)
{
    static const unsigned char languages[6] = { 1, 2, 3, 5, 4, 7 };
    int i;
    for (i = 0; i < 6; ++i) if (languages[i] == language) return i;
    return -1;
}

void CpReceive_DexCapture(unsigned char *dex, uint16_t species, uint8_t form, uint8_t gender,
                          uint32_t personality, uint8_t language)
{
    int index;
    if (species == 0 || species > NATIONAL_DEX_COUNT) return;
    if (!Seen(dex, species)) {
        if (species == SPECIES_SPINDA) Write32(dex + DEX_SPINDA, personality);
        /* UpdateGender: genderless records as male; a first sighting sets both. */
        SetBit(dex + DEX_GENDER_SECOND, species, gender == 2 ? 0 : gender);
        SetBit(dex + DEX_GENDER, species, gender == 2 ? 0 : gender);
    } else if ((unsigned)Bit(dex + DEX_GENDER, species) != gender) {
        SetBit(dex + DEX_GENDER_SECOND, species, gender == 2 ? 0 : gender);
    }
    switch (species) {
    case SPECIES_UNOWN: UpdateUnown(dex, form); break;
    case SPECIES_BURMY: case SPECIES_WORMADAM: UpdateThreeForms(dex, species, form); break;
    case SPECIES_SHELLOS: case SPECIES_GASTRODON: case SPECIES_SHAYMIN: case SPECIES_GIRATINA:
        UpdateTwoForms(dex, species, form); break;
    case SPECIES_DEOXYS: UpdateDeoxys(dex, form); break;
    case SPECIES_ROTOM: UpdateRotom(dex, form); break;
    default: break;
    }
    if ((index = LanguageIndex(language)) >= 0) dex[DEX_LANGUAGES + species] |= (unsigned char)(1u << index);
    SetBit(dex + DEX_CAUGHT, species, 1);
    SetBit(dex + DEX_SEEN, species, 1);
}

static int Capture(const CpAdapterData *data, unsigned char *general, const Mon *mon)
{
    uint8_t gender;
    if (Gender(data, Species(mon), Pid(mon), &gender) != 0) return -1;
    CpReceive_DexCapture(general + CP_RECEIVE_POKEDEX_OFFSET, (uint16_t)Species(mon), (uint8_t)Form(mon->box),
                         gender, Pid(mon), mon->box[PK_LANGUAGE]);
    return 0;
}

/* ---- Game records, Pokétch, journal -------------------------------------- */

typedef struct Records {
    unsigned char *at, plain[REC_ENCODED_END];
} Records;

static uint32_t RecordSeed(const unsigned char *records) { return Read16(records + REC_SEED) | ((uint32_t)Read16(records + REC_SEED + 2) << 16); }

static void OpenRecords(Records *records, unsigned char *general)
{
    records->at = general + CP_RECEIVE_RECORDS_OFFSET;
    memcpy(records->plain, records->at, sizeof(records->plain));
    Crypt(records->plain + REC_ENCODED, REC_ENCODED_END - REC_ENCODED, RecordSeed(records->at));
}

/* EncodeGameRecords: the byte sum of the plain records is the seed's low half. */
static void CloseRecords(Records *records)
{
    unsigned sum = 0, i;
    for (i = REC_ENCODED; i < REC_ENCODED_END; ++i) sum += records->plain[i];
    Write16(records->at + REC_SEED, sum & 65535);
    Crypt(records->plain + REC_ENCODED, REC_ENCODED_END - REC_ENCODED, RecordSeed(records->at));
    memcpy(records->at + REC_ENCODED, records->plain + REC_ENCODED, REC_ENCODED_END - REC_ENCODED);
}

/* GameRecords_IncrementRecordValue for a 32-bit record with the high limit. */
static void Increment(Records *records, unsigned id)
{
    uint32_t value = Read32(records->plain + 4 * id) + 1;
    Write32(records->plain + 4 * id, value < HIGH_LIMIT_U32 ? value : HIGH_LIMIT_U32);
}

/* GameRecords_IncrementTrainerScore. */
static void Score(Records *records, uint32_t points)
{
    uint32_t current = Read32(records->plain + 4 * REC_TRAINER_SCORE), value;
    uint32_t limited = current > HIGH_LIMIT_U32 ? HIGH_LIMIT_U32 : current;
    if (limited + points > TRAINER_SCORE_LIMIT) value = TRAINER_SCORE_LIMIT;
    else value = current + points < HIGH_LIMIT_U32 ? current + points : HIGH_LIMIT_U32;
    Write32(records->plain + 4 * REC_TRAINER_SCORE, value);
}

/* Poketch_PokemonHistoryEnqueue: the oldest entry leaves a full history. */
static void History(unsigned char *general, const Mon *mon)
{
    unsigned char *history = general + CP_RECEIVE_POKETCH_OFFSET + POKETCH_HISTORY;
    unsigned count = 0, species = Species(mon), form = Form(mon->box), icon = 0;
    while (count < POKETCH_HISTORY_MAX && Read16(history + 8 * count) != 0) ++count;
    if (count == POKETCH_HISTORY_MAX) {
        memmove(history, history + 8, 8 * (POKETCH_HISTORY_MAX - 1));
        count = POKETCH_HISTORY_MAX - 1;
    }
    /* BoxPokemon_IconFormOffset. */
    switch (Egg(mon) ? SPECIES_EGG : species) {
    case SPECIES_UNOWN: case SPECIES_DEOXYS: case SPECIES_BURMY: case SPECIES_WORMADAM: case SPECIES_SHELLOS:
    case SPECIES_GASTRODON: case SPECIES_GIRATINA: case SPECIES_SHAYMIN: case SPECIES_ROTOM:
        icon = form; break;
    default: break;
    }
    Write16(history + 8 * count, species); Write16(history + 8 * count + 2, icon);
    Write32(history + 8 * count + 4, form);
}

/* JournalEntry_StringCopy: fill with CHAR_NONE, then copy up to it. journal.c
 * defines its own CHAR_NONE as 0xFFFF, the terminator. */
static void JournalCopy(const uint16_t *source, unsigned char *destination, unsigned length)
{
    unsigned i;
    memset(destination, 0xFF, 2 * length);
    for (i = 0; i < length && source[i] != CP_CHAR_EOS; ++i) Write16(destination + 2 * i, source[i]);
}

/* JournalEntry_CreateEventGotPokemonFromTrade, saved to page 0 as the game does. */
static void Journal(unsigned char *general, const Mon *mon, uint8_t gender)
{
    unsigned char *events = general + CP_RECEIVE_JOURNAL_OFFSET + JOURNAL_ONLINE, *event = NULL;
    uint16_t ot[TRAINER_NAME_LENGTH + 1], name[NAME_LENGTH + 1];
    unsigned i;
    /* MON_DATA_OT_NAME and MON_DATA_NICKNAME end each name with the terminator. */
    for (i = 0; i < TRAINER_NAME_LENGTH; ++i) ot[i] = (uint16_t)Read16(mon->box + PK_OT_NAME + 2 * i);
    ot[i] = CP_CHAR_EOS;
    for (i = 0; i < NAME_LENGTH; ++i) name[i] = (uint16_t)Read16(mon->box + PK_NICKNAME + 2 * i);
    name[i] = CP_CHAR_EOS; /* so the 12-code copy stops within the buffer */
    for (i = 0; i < JOURNAL_ONLINE_MAX; ++i) {
        if (events[i * JOURNAL_ONLINE_SIZE] == 0) { event = events + i * JOURNAL_ONLINE_SIZE; break; }
    }
    if (!event) {
        memmove(events, events + JOURNAL_ONLINE_SIZE, JOURNAL_ONLINE_SIZE * (JOURNAL_ONLINE_MAX - 1));
        event = events + JOURNAL_ONLINE_SIZE * (JOURNAL_ONLINE_MAX - 1);
        memset(event, 0, JOURNAL_ONLINE_SIZE);
    }
    event[0] = ONLINE_EVENT_GOT_POKEMON_FROM_TRADE;
    event[1] = (unsigned char)(((mon->box[PK_OT_GENDER] >> 7) << 2) | (gender & 3));
    JournalCopy(ot, event + 2, TRAINER_NAME_LENGTH + 1);
    JournalCopy(name, event + 18, 12);
}

/* MON_DATA_MET_LOCATION in Platinum. */
static unsigned MetLocation(const Mon *mon)
{
    unsigned dp = Read16(mon->box + PK_MET_DP), pt = Read16(mon->box + PK_MET_PT);
    return dp == FATEFUL_ENCOUNTER_LOCATION && pt ? pt : dp;
}

/* ---- The delivery -------------------------------------------------------- */

/* ReceiveEffects for a matched delivery. */
static int Effects(const CpAdapterData *data, unsigned char *general, Mon *mon)
{
    CpTradeEvolution evolution;
    Records records;
    uint8_t gender, effect = 0;
    unsigned held;
    int result;
    /* Pokemon_FromBoxPokemon: an empty party extension, then level and stats. */
    memset(mon->party, 0, sizeof(mon->party));
    if (CpReceive_LevelAndStats(data, mon->box, mon->party) != 0) return -1;
    mon->box[PK_FRIENDSHIP] = BASE_FRIENDSHIP_VALUE;
    if (Gender(data, Species(mon), Pid(mon), &gender) != 0) return -1;
    mon->box[PK_FORM_BYTE] = (unsigned char)((mon->box[PK_FORM_BYTE] & ~6u) | (gender << 1));
    OpenRecords(&records, general);
    /* SaveData_UpdateCatchRecords. */
    if (!Egg(mon)) {
        unsigned char *dex = general + CP_RECEIVE_POKEDEX_OFFSET;
        unsigned species = Species(mon);
        if (species == 0 || species > NATIONAL_DEX_COUNT || !Bit(dex + DEX_CAUGHT, species) || !Seen(dex, species))
            Score(&records, SCORE_CAUGHT_SPECIES);
        if (Capture(data, general, mon) != 0) return -1;
        History(general, mon);
    }
    if (Species(mon) == SPECIES_ARCEUS && ((mon->box[PK_FORM_BYTE] & 1) || MetLocation(mon) == LOCATION_HALL_OF_ORIGIN))
        Write16(general + CP_RECEIVE_VARS_OFFSET + 2 * VAR_ARCEUS_EVENT_STATE, 1);
    /* Pokemon_GetEvolutionTargetSpecies(EVO_CLASS_BY_TRADE): an Everstone stops
     * any evolution but Kadabra's. */
    held = Read16(mon->box + PK_ITEM);
    result = data->holdEffect(data->arg, (uint16_t)held, &effect);
    if (result == CP_ERR_NOT_FOUND) effect = 0;
    else if (result != CP_OK) return -1;
    result = Species(mon) != SPECIES_KADABRA && effect == HOLD_EFFECT_NO_EVOLVE ? CP_ERR_NOT_FOUND
        : data->tradeEvolution(data->arg, (uint16_t)Species(mon), (uint16_t)held, &evolution);
    if (result == CP_OK) {
        Write16(mon->box + PK_SPECIES, evolution.target);
        if (Ability(data, mon) != 0 || CpReceive_LevelAndStats(data, mon->box, mon->party) != 0) return -1;
        if (!(Read32(mon->box + PK_IVS) >> 31) && SpeciesName(data, mon) != 0) return -1;
        if (evolution.item) Write16(mon->box + PK_ITEM, 0);
        if (Capture(data, general, mon) != 0) return -1;
        Increment(&records, REC_POKEMON_EVOLVED);
        Score(&records, SCORE_CAUGHT_SPECIES);
        History(general, mon);
    } else if (result != CP_ERR_NOT_FOUND) return -1;
    Increment(&records, REC_WIFI_TRADES);
    Score(&records, SCORE_GTS_TRADE_COMPLETE);
    CloseRecords(&records);
    if (Gender(data, Species(mon), Pid(mon), &gender) != 0) return -1;
    Journal(general, mon, gender);
    return 0;
}

/* Mail_Reset on zeroed mail: no trainer, the cartridge's language and
 * version, no type, names and icons empty, sentences of no type. */
static void ResetMail(unsigned char mail[MAIL_SIZE], uint8_t language, const uint16_t *keptSentences)
{
    unsigned i;
    memset(mail, 0, MAIL_SIZE);
    mail[5] = language; mail[6] = VERSION_PLATINUM; mail[7] = 0xFF;
    for (i = 8; i < 30; i += 2) Write16(mail + i, 0xFFFF);
    for (i = 0; i < 3; ++i) {
        unsigned char *sentence = mail + MAIL_SENTENCES + 8 * i;
        Write16(sentence, 0xFFFF);
        Write16(sentence + 2, keptSentences ? keptSentences[i] : 0);
        Write16(sentence + 4, 0xFFFF); Write16(sentence + 6, 0xFFFF);
    }
}

int CpReceive_Apply(const CpAdapterData *data, unsigned char *general, const unsigned char incoming[136],
                    int destination, const uint16_t *keptSentences, unsigned char image[236])
{
    Mon mon;
    unsigned sum = 0, i;
    uint8_t pp;
    if (!data || !general || (destination != CP_RECEIVE_PARTY && destination != CP_RECEIVE_BOX)
        || Read32(general + CP_RECEIVE_POKEDEX_OFFSET + DEX_MAGIC) != POKEDEX_MAGIC) return -1;
    memcpy(mon.box, incoming, sizeof(mon.box));
    if (WtPokemon_Encode(mon.box, image) != 0 || Effects(data, general, &mon) != 0) return -1;
    if (destination == CP_RECEIVE_PARTY) {
        ResetMail(mon.party + PT_MAIL, data->language, keptSentences);
    } else {
        /* BoxPokemon_RestorePP, then BoxPokemon_SetShayminForm to the Land Forme. */
        for (i = 0; i < 4; ++i) {
            unsigned move = Read16(mon.box + PK_MOVES + 2 * i);
            if (!move) continue;
            if (MaxPp(data, move, mon.box[PK_PP_UPS + i], &pp) != 0) return -1;
            mon.box[PK_PP + i] = pp;
        }
        if (Species(&mon) == SPECIES_SHAYMIN) {
            mon.box[PK_FORM_BYTE] &= 7;
            if (Ability(data, &mon) != 0) return -1;
        }
    }
    for (i = 8; i < sizeof(mon.box); i += 2) sum += Read16(mon.box + i);
    Write16(mon.box + PK_CHECKSUM, sum & 65535);
    if (WtPokemon_Encode(mon.box, image) != 0) return -1;
    Crypt(mon.party, sizeof(mon.party), Pid(&mon));
    if (destination == CP_RECEIVE_PARTY) memcpy(image + 136, mon.party, sizeof(mon.party));
    return 0;
}
