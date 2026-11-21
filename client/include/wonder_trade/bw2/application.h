#ifndef WONDER_TRADE_BW2_APPLICATION_H
#define WONDER_TRADE_BW2_APPLICATION_H

#include "types.h"
#include "struct_decls.h"

/* Wonder Trade's screen (src/wonder_trade/bw2/application.c). WtEvent_Create
 * opens it from the field menu with the field closed; the boot process opens
 * it in recovery mode when the save needs recovery. */
typedef struct WtApp WtApp;

WtApp *WtApp_Create(BOOL boot);
BOOL WtApp_Update(WtApp *app);
void WtApp_Free(WtApp *app);

GameEvent *WtEvent_Create(GameSystem *gsys, Field *field);

#endif
