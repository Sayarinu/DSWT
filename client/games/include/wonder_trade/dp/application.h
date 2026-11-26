#ifndef WONDER_TRADE_DP_APPLICATION_H
#define WONDER_TRADE_DP_APPLICATION_H

#include "overlay_manager.h"
#include "task.h"

extern const struct OverlayManagerTemplate gWonderTradeAppTemplate;
extern const struct OverlayManagerTemplate gWonderTradeBootAppTemplate;

/* The start menu's Wonder Trade entry (asm/unk_02035068.s, src/wonder_trade/dp/hooks.c). */
struct FieldSystem;
struct StartMenuTaskData;
BOOL WonderTrade_StartMenuSelect(struct TaskManager *taskManager);
BOOL WonderTrade_StartMenuInput(struct TaskManager *taskManager);
u32 WonderTrade_StartMenuHidden(struct FieldSystem *fieldSystem, u32 hidden);
void WonderTrade_StartMenuAddWindow(struct FieldSystem *fieldSystem, struct StartMenuTaskData *startMenu, u32 count);
void WonderTrade_StartMenuScaleCursor(struct StartMenuTaskData *startMenu);

#endif
