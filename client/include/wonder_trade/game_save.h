#ifndef WONDER_TRADE_GAME_SAVE_H
#define WONDER_TRADE_GAME_SAVE_H

#include "constants/string.h"

#include "charcode.h"
#include "savedata.h"
#include "wonder_trade/engine.h"

typedef struct WtGameSave {
    SaveData *save;
    enum HeapID heapID;
} WtGameSave;

typedef struct WtReceivedPokemon {
    u16 species;
    u8 gender, form, shiny;
    u32 personality;
    BOOL hasNickname;
    charcode_t nickname[MON_NAME_LEN + 1], originalTrainer[TRAINER_NAME_LEN + 1];
} WtReceivedPokemon;

/* Reads the delivered image as saved, including any trade evolution. Display only. */
int WtGameSave_Received(const WtJournalEntry *, WtReceivedPokemon *);
int WtGameSave_Checkpoint(WtGameSave *);
int WtGameSave_Eligible(SaveData *, unsigned slot);
/* FALSE when the deposit's absence check would refuse: an identical copy would
 * remain, or another Pokémon cannot be read. Lets the picker explain this before
 * any trade state changes. */
BOOL WtGameSave_Unique(SaveData *, unsigned slot, enum HeapID);
int WtGameSave_Inventory(void *, enum WtInventoryStep, WtJournalEntry *, unsigned);
int WtGameSave_CanLeave(WtGameSave *, const WtJournalEntry *);
int WtGameSave_Boot(WtGameSave *, WtJournalIO);

#endif
