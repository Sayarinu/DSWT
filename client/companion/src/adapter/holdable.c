#include "adapter/holdable.h"

#include "game/gamedata.h"

int CpAdapter_RomHoldable(void *game, unsigned item)
{
    CpItem data;
    return game && item <= UINT16_MAX && CpGame_Item(game, (uint16_t)item, &data) == CP_OK && data.holdable;
}
