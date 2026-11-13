#ifndef WONDER_TRADE_DP_GAME_SAVE_H
#define WONDER_TRADE_DP_GAME_SAVE_H

#include "global.h"

#include "constants/heap.h"

#include "save.h"
#include "wonder_trade/game_adapter.h"

/* Diamond and Pearl's game context for the shared core. */
struct WtGame {
    SaveData *save;
    enum HeapID heapID;
};

/* Nonzero when the running game's save layout is the one recorded in
 * protocol/save-layout-dp.json, with both retail blocks and every extra chunk
 * ending before the first journal bank. Anything else is never written. */
int WtGameSave_LayoutKnown(const SaveData *);
/* Nonzero once the adventure has been saved to flash; until then the journal
 * cannot checkpoint it (SaveBoth refuses), so Wonder Trade does not connect. */
int WtGameSave_Written(const SaveData *);
int WtGameSave_Eligible(SaveData *, unsigned slot);
/* FALSE when the deposit's absence check would refuse: an identical copy would
 * remain, or another Pokémon cannot be read. */
BOOL WtGameSave_Unique(SaveData *, unsigned slot, enum HeapID);

#endif
