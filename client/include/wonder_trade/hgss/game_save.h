#ifndef WONDER_TRADE_HGSS_GAME_SAVE_H
#define WONDER_TRADE_HGSS_GAME_SAVE_H

#include "global.h"

#include "constants/heap.h"

#include "save.h"
#include "wonder_trade/game_adapter.h"

/* HeartGold and SoulSilver's game context for the shared core. */
struct WtGame {
    SaveData *save;
    enum HeapID heapID;
};

/* Nonzero when the running game's save layout is the one recorded in
 * protocol/save-layout-hgss.json, with every retail block and extra chunk
 * ending before the first journal bank. Anything else is never written. */
int WtGameSave_LayoutKnown(const SaveData *);
int WtGameSave_Eligible(SaveData *, unsigned slot);
/* FALSE when the deposit's absence check would refuse: an identical copy would
 * remain, or another Pokémon cannot be read. */
BOOL WtGameSave_Unique(SaveData *, unsigned slot, enum HeapID);

#endif
