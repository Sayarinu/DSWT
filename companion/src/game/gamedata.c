#include "game/gamedata.h"

#include <stddef.h>

/* Deoxys, Wormadam, Giratina, Shaymin and Rotom forms follow the species. */
static const CpGameForms kPlatinumForms[] = {
    { 386, 496, 3 }, { 413, 499, 2 }, { 487, 501, 1 }, { 492, 502, 1 }, { 479, 503, 5 },
};

static const CpGameTable kPlatinum = {
    CP_FAMILY_PLATINUM,
    "poketool/personal/pl_personal.narc",
    "poketool/personal/pl_growtbl.narc",
    "poketool/personal/evo.narc",
    "itemtool/itemdata/pl_item_data.narc",
    "msgdata/pl_msg.narc",
    "graphic/pl_font.narc",
    412, 408,
    113, 134, 468,
    kPlatinumForms, sizeof kPlatinumForms / sizeof kPlatinumForms[0],
};

const CpGameTable *CpGame_Table(CpFamily family)
{
    return family == CP_FAMILY_PLATINUM ? &kPlatinum : NULL;
}

int CpGame_Open(const CpReader *reader, CpGame *out)
{
    int result;

    if ((result = CpRom_Identify(reader, &out->rom)) != CP_OK) return result;
    if (!(out->table = CpGame_Table(out->rom.family))) return CP_ERR_UNSUPPORTED;
    if ((result = CpNarc_OpenPath(reader, out->table->personal, &out->personal)) != CP_OK ||
        (result = CpNarc_OpenPath(reader, out->table->growth, &out->growth)) != CP_OK ||
        (result = CpNarc_OpenPath(reader, out->table->evolution, &out->evolution)) != CP_OK ||
        (result = CpNarc_OpenPath(reader, out->table->items, &out->items)) != CP_OK ||
        (result = CpNarc_OpenPath(reader, out->table->messages, &out->messages)) != CP_OK ||
        (result = CpNarc_OpenPath(reader, out->table->font, &out->font)) != CP_OK)
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
    int result;

    if (item == 0 || item >= table->itemCount) return CP_ERR_NOT_FOUND;
    if (item >= table->itemGapFirst && item <= table->itemGapLast) return CP_ERR_NOT_FOUND;
    if (item > table->itemGapLast) index = (uint16_t)(item - (table->itemGapLast - table->itemGapFirst + 1));
    if ((result = ReadMember(&game->items, index, record, sizeof record)) != CP_OK) return result;
    bits = CpLe16(record + 8);
    out->price = CpLe16(record);
    out->holdEffect = record[2];
    out->preventToss = (uint8_t)(bits >> 5 & 1);
    out->fieldPocket = (uint8_t)(bits >> 7 & 0xF);
    out->holdable = !out->preventToss;
    return CP_OK;
}
