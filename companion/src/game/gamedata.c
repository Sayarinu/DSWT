#include "game/gamedata.h"

#include <stddef.h>

/* Deoxys, Wormadam, Giratina, Shaymin and Rotom forms follow the species. */
static const CpGameForms kPlatinumForms[] = {
    { 386, 496, 3 }, { 413, 499, 2 }, { 487, 501, 1 }, { 492, 502, 1 }, { 479, 503, 5 },
};

/* Platinum adds the Griseous Orb (112); 113..134 stay unused. */
static const CpItemGap kPlatinumItemGaps[] = { { 113, 134 } };

static const CpGameTable kPlatinum = {
    CP_FAMILY_PLATINUM,
    "poketool/personal/pl_personal.narc",
    "poketool/personal/pl_growtbl.narc",
    "poketool/personal/evo.narc",
    "itemtool/itemdata/pl_item_data.narc",
    "msgdata/pl_msg.narc",
    "graphic/pl_font.narc",
    "poketool/waza/pl_waza_tbl.narc",
    NULL,
    { 412, 408, 408 }, { 392, 390, 390 },
    468, kPlatinumItemGaps, 1,
    kPlatinumForms, sizeof kPlatinumForms / sizeof kPlatinumForms[0],
};

/* Diamond and Pearl give only Deoxys and Wormadam forms their own records
 * (pokediamond ResolveMonForm). */
static const CpGameForms kDiamondPearlForms[] = { { 386, 496, 3 }, { 413, 499, 2 } };

/* constants/items.h jumps from the Odd Keystone (111) to the Adamant Orb (135). */
static const CpItemGap kDiamondPearlItemGaps[] = { { 112, 134 } };

static const CpGameTable kDiamondPearl = {
    CP_FAMILY_DP,
    "poketool/personal/personal.narc",
    "poketool/personal/growtbl.narc",
    "poketool/personal/evo.narc",
    "itemtool/itemdata/item_data.narc",
    "msgdata/msg.narc",
    "graphic/font.narc",
    "poketool/waza/waza_tbl.narc",
    "poketool/personal_pearl/personal.narc",
    { 362, 356, 357 }, { 344, 341, 342 },
    465, kDiamondPearlItemGaps, 1,
    kDiamondPearlForms, sizeof kDiamondPearlForms / sizeof kDiamondPearlForms[0],
};

/* HeartGold and SoulSilver keep Platinum's form records (ResolveMonForm). */
static const CpGameForms kHeartGoldSoulSilverForms[] = {
    { 386, 496, 3 }, { 413, 499, 2 }, { 487, 501, 1 }, { 492, 502, 1 }, { 479, 503, 5 },
};

/* src/item.c: 113..134 and the Explorer Kit (428) read item 0's record. */
static const CpItemGap kHeartGoldSoulSilverItemGaps[] = { { 113, 134 }, { 428, 428 } };

/* Their archives are numbered: a/0/0/2 is NARC_poketool_personal_personal,
 * and so on (include/filesystem_files_def.h). */
static const CpGameTable kHeartGoldSoulSilver = {
    CP_FAMILY_HGSS,
    "a/0/0/2", /* personal */
    "a/0/0/3", /* growth table */
    "a/0/3/4", /* evolutions */
    "a/0/1/7", /* item data */
    "a/0/2/7", /* messages */
    "a/0/1/6", /* fonts */
    "a/0/1/1", /* moves */
    NULL,
    { 237, 232, 233 }, { 222, 219, 220 },
    537, kHeartGoldSoulSilverItemGaps, 2,
    kHeartGoldSoulSilverForms, sizeof kHeartGoldSoulSilverForms / sizeof kHeartGoldSoulSilverForms[0],
};

const CpGameTable *CpGame_Table(CpFamily family)
{
    switch (family) {
    case CP_FAMILY_DP: return &kDiamondPearl;
    case CP_FAMILY_PLATINUM: return &kPlatinum;
    case CP_FAMILY_HGSS: return &kHeartGoldSoulSilver;
    default: return NULL;
    }
}

uint16_t CpGame_Bank(const CpGame *game, const CpMessageBank *bank)
{
    if (game->rom.language == CP_LANGUAGE_JAPANESE) return bank->japanese;
    if (game->rom.language == CP_LANGUAGE_KOREAN) return bank->korean;
    return bank->international;
}

int CpGame_Open(const CpReader *reader, CpGame *out)
{
    int result;

    if ((result = CpRom_Identify(reader, &out->rom)) != CP_OK) return result;
    if (!(out->table = CpGame_Table(out->rom.family))) return CP_ERR_UNSUPPORTED;
    if ((result = CpNarc_OpenPath(reader, out->table->personalPearl && out->rom.version == CP_VERSION_PEARL
            ? out->table->personalPearl : out->table->personal, &out->personal)) != CP_OK ||
        (result = CpNarc_OpenPath(reader, out->table->growth, &out->growth)) != CP_OK ||
        (result = CpNarc_OpenPath(reader, out->table->evolution, &out->evolution)) != CP_OK ||
        (result = CpNarc_OpenPath(reader, out->table->items, &out->items)) != CP_OK ||
        (result = CpNarc_OpenPath(reader, out->table->messages, &out->messages)) != CP_OK ||
        (result = CpNarc_OpenPath(reader, out->table->font, &out->font)) != CP_OK ||
        (result = CpNarc_OpenPath(reader, out->table->moves, &out->moves)) != CP_OK)
        return result;
    return CP_OK;
}

/* Read the first `size` bytes of an archive member. */
static int ReadMember(const CpNarc *narc, uint32_t index, void *destination, uint32_t size)
{
    CpRomFile member;
    int result = CpNarc_Member(narc, index, &member);

    if (result != CP_OK) return result;
    return member.size < size ? CP_ERR_FORMAT : CpRomFile_Read(&member, 0, destination, size);
}

static uint32_t PersonalIndex(const CpGameTable *table, uint16_t species, uint8_t form)
{
    uint8_t i;

    for (i = 0; i < table->formCount; ++i)
        if (table->forms[i].species == species && form >= 1 && form <= table->forms[i].count)
            return table->forms[i].firstIndex + form - 1u;
    return species;
}

int CpGame_Personal(const CpGame *game, uint16_t species, uint8_t form, CpPersonal *out)
{
    /* The record: six base stats, types, catch rate, base experience, EV
     * yields, wild held items, then these fields from offset 16. */
    uint8_t record[24];
    int result;

    if (species == 0) return CP_ERR_NOT_FOUND;
    result = ReadMember(&game->personal, PersonalIndex(game->table, species, form), record, sizeof record);
    if (result != CP_OK) return result;
    out->baseStats[0] = record[0];
    out->baseStats[1] = record[1];
    out->baseStats[2] = record[2];
    out->baseStats[3] = record[3];
    out->baseStats[4] = record[4];
    out->baseStats[5] = record[5];
    out->genderRatio = record[16];
    out->baseFriendship = record[18];
    out->growthRate = record[19];
    out->abilities[0] = record[22];
    out->abilities[1] = record[23];
    return CP_OK;
}

int CpGame_ExpTable(const CpGame *game, uint8_t growthRate, uint32_t out[CP_MAX_LEVEL + 1])
{
    uint8_t raw[(CP_MAX_LEVEL + 1) * 4];
    int i, result = ReadMember(&game->growth, growthRate, raw, sizeof raw);

    if (result != CP_OK) return result;
    for (i = 0; i <= CP_MAX_LEVEL; ++i) {
        out[i] = CpLe32(raw + i * 4);
        if (i > 0 && out[i] < out[i - 1]) return CP_ERR_FORMAT;
    }
    return CP_OK;
}

uint8_t CpExp_Level(const uint32_t table[CP_MAX_LEVEL + 1], uint32_t exp)
{
    uint8_t level = 1;

    while (level < CP_MAX_LEVEL && exp >= table[level + 1]) ++level;
    return level;
}

#define EVOLUTIONS 7

int CpGame_TradeEvolution(const CpGame *game, uint16_t species, uint16_t heldItem, CpTradeEvolution *out)
{
    /* Up to seven {method, parameter, target species} entries. */
    uint8_t record[EVOLUTIONS * 6];
    int i, result;

    if (species == 0) return CP_ERR_NOT_FOUND;
    if ((result = ReadMember(&game->evolution, species, record, sizeof record)) != CP_OK) return result;
    for (i = 0; i < EVOLUTIONS; ++i) {
        uint16_t method = CpLe16(record + i * 6), parameter = CpLe16(record + i * 6 + 2);
        uint16_t target = CpLe16(record + i * 6 + 4);

        if (target == 0) continue;
        if (method == CP_EVO_TRADE || (method == CP_EVO_TRADE_WITH_HELD_ITEM && parameter == heldItem && heldItem)) {
            out->target = target;
            out->item = method == CP_EVO_TRADE ? 0 : parameter;
            return CP_OK;
        }
    }
    return CP_ERR_NOT_FOUND;
}

int CpGame_Item(const CpGame *game, uint16_t item, CpItem *out)
{
    const CpGameTable *table = game->table;
    /* price, hold effect, effect parameter, Pluck and Fling effects, Fling and
     * Natural Gift power, then a bit field: Natural Gift type (5 bits),
     * prevent toss, can register, field pocket (4 bits), battle pocket. */
    uint8_t record[10];
    uint16_t bits, index = item;
    uint8_t i;
    int result;

    if (item == 0 || item >= table->itemCount) return CP_ERR_NOT_FOUND;
    for (i = 0; i < table->itemGapCount; ++i) {
        const CpItemGap *gap = &table->itemGaps[i];
        if (item >= gap->first && item <= gap->last) return CP_ERR_NOT_FOUND;
        if (item > gap->last) index = (uint16_t)(index - (gap->last - gap->first + 1));
    }
    if ((result = ReadMember(&game->items, index, record, sizeof record)) != CP_OK) return result;
    bits = CpLe16(record + 8);
    out->price = CpLe16(record);
    out->holdEffect = record[2];
    out->preventToss = (uint8_t)(bits >> 5 & 1);
    out->fieldPocket = (uint8_t)(bits >> 7 & 0xF);
    out->holdable = !out->preventToss;
    return CP_OK;
}

int CpGame_MovePp(const CpGame *game, uint16_t move, uint8_t *out)
{
    /* effect, class, power, type, accuracy, then the PP. */
    uint8_t record[7];
    int result;

    if (move == 0) return CP_ERR_NOT_FOUND;
    if ((result = ReadMember(&game->moves, move, record, sizeof record)) != CP_OK) return result;
    *out = record[6];
    return CP_OK;
}
