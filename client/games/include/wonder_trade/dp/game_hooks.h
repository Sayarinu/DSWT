#ifndef WONDER_TRADE_DP_GAME_HOOKS_H
#define WONDER_TRADE_DP_GAME_HOOKS_H

/* What pokediamond's own sources call: main.c, save.c and overlay 52's new
 * game. Those build with the game's partial C library, which lacks the
 * standard headers the core's own headers need, so these few declarations
 * repeat theirs (wonder_trade/entropy_game.h, wonder_trade/recovery.h). */

#include "overlay_manager.h"

void WonderTradeEntropy_Init(void);
void WonderTradeEntropy_Frame(void);

int WtRecovery_Started(void);
int WtRecovery_CanWrite(void);
int WtRecovery_CanClear(void);

/* The boot screen that inspects the journal before anything else runs. */
extern const struct OverlayManagerTemplate gWonderTradeBootAppTemplate;

#endif
