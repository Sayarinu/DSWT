#ifndef WONDER_TRADE_GAME_SAVE_H
#define WONDER_TRADE_GAME_SAVE_H

#include "savedata.h"
#include "wonder_trade/engine.h"

typedef struct WtGameSave {
    SaveData *save;
    enum HeapID heapID;
} WtGameSave;

int WtGameSave_Checkpoint(WtGameSave *);
int WtGameSave_Eligible(SaveData *, unsigned slot);
int WtGameSave_Inventory(void *, enum WtInventoryStep, WtJournalEntry *, unsigned);
int WtGameSave_CanLeave(WtGameSave *, const WtJournalEntry *);
int WtGameSave_Boot(WtGameSave *, WtJournalIO);

#endif
