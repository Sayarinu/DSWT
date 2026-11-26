#include "adapter/rom_data.h"

#include "game/text.h"

static int Personal(void *game, uint16_t species, uint8_t form, CpPersonal *out)
{
    return CpGame_Personal(game, species, form, out);
}

static int ExpTable(void *game, uint8_t growthRate, uint32_t out[CP_MAX_LEVEL + 1])
{
    return CpGame_ExpTable(game, growthRate, out);
}

static int TradeEvolution(void *game, uint16_t species, uint16_t heldItem, CpTradeEvolution *out)
{
    return CpGame_TradeEvolution(game, species, heldItem, out);
}

static int HoldEffect(void *game, uint16_t item, uint8_t *out)
{
    CpItem data;
    int result = CpGame_Item(game, item, &data);
    if (result == CP_OK) *out = data.holdEffect;
    return result;
}

static int MovePp(void *game, uint16_t move, uint8_t *out)
{
    return CpGame_MovePp(game, move, out);
}

static int SpeciesName(void *game, uint16_t species, uint16_t *out, size_t capacity, size_t *length)
{
    return CpGame_SpeciesName(game, species, out, capacity, length);
}

static int Evolutions(void *game, uint16_t species, CpEvolution out[CP_EVOLUTIONS])
{
    return CpGame_Evolutions(game, species, out);
}

void CpAdapter_RomData(CpAdapterData *out, const CpGame *game)
{
    out->arg = (void *)game;
    out->personal = Personal;
    out->expTable = ExpTable;
    out->tradeEvolution = TradeEvolution;
    out->holdEffect = HoldEffect;
    out->movePp = MovePp;
    out->speciesName = SpeciesName;
    out->language = game->rom.language;
    out->evolutions = Evolutions;
}
