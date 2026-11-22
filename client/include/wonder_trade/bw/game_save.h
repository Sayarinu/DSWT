#ifndef WONDER_TRADE_BW_GAME_SAVE_H
#define WONDER_TRADE_BW_GAME_SAVE_H

#include "wonder_trade/bw/game.h"
#include "wonder_trade/game_adapter.h"

/* Black and White's game context for the shared core: the save and the heap
 * Wonder Trade's screen allocates from, a child of heap 1 that exists only
 * while the screen runs with the field closed. */
struct WtGame {
    struct SaveControl *save;
    u32 heapId;
};

int WtGameSave_Eligible(WtGame *, unsigned slot);
/* FALSE when the deposit's absence check would refuse: an identical copy would
 * remain, or another Pokémon cannot be read. */
BOOL WtGameSave_Unique(WtGame *, unsigned slot);
/* Frees the musical distribution save for the journal before its first
 * write: zero once the game will never read it again and no data a musical
 * left remains in the journal banks. */
int WtGameSave_ReleaseMusicalArea(WtGame *);
/* TRUE when the journal banks hold data a musical left and no journal. */
BOOL WtGameSave_MusicalLeftover(void);

#endif
