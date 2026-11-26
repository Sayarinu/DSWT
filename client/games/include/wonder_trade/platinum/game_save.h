#ifndef WONDER_TRADE_PLATINUM_GAME_SAVE_H
#define WONDER_TRADE_PLATINUM_GAME_SAVE_H

#include "constants/heap.h"

#include "savedata.h"
#include "wonder_trade/game_adapter.h"

/* Platinum's game context for the shared core. */
struct WtGame {
    SaveData *save;
    enum HeapID heapID;
};

int WtGameSave_Eligible(SaveData *, unsigned slot);
/* FALSE when the deposit's uniqueness check would refuse: a Pokémon with the
 * same identity (personality and original trainer ID) would remain, or another
 * Pokémon cannot be read (wonder_trade/delivery.h). Lets the picker explain this before
 * any trade state changes. */
BOOL WtGameSave_Unique(SaveData *, unsigned slot, enum HeapID);

#endif
