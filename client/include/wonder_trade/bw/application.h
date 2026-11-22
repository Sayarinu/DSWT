#ifndef WONDER_TRADE_BW_APPLICATION_H
#define WONDER_TRADE_BW_APPLICATION_H

#include "wonder_trade/bw/game.h"

/* Wonder Trade's screen (src/wonder_trade/bw/application.c). WtEvent_Create
 * opens it from the field menu with the field closed; the boot process opens
 * it in recovery mode when the save needs recovery, and WtApp_CreateChecking
 * shows only "Checking saved game..." while a slow boot check runs. */
typedef struct WtApp WtApp;

WtApp *WtApp_Create(BOOL boot);
WtApp *WtApp_CreateChecking(void);
BOOL WtApp_Update(WtApp *app);
void WtApp_Free(WtApp *app);

struct GameEvent *WtEvent_Create(struct GameSystem *gsys, struct FieldMap *field);

#endif
