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
/* FALSE when the deposit's absence check would refuse: an identical copy would
 * remain, or another Pokémon cannot be read. Lets the picker explain this before
 * any trade state changes. */
BOOL WtGameSave_Unique(SaveData *, unsigned slot, enum HeapID);

#endif
