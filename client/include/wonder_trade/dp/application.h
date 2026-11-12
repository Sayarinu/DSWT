#ifndef WONDER_TRADE_DP_APPLICATION_H
#define WONDER_TRADE_DP_APPLICATION_H

#include "overlay_manager.h"
#include "task.h"

extern const struct OverlayManagerTemplate gWonderTradeAppTemplate;
extern const struct OverlayManagerTemplate gWonderTradeBootAppTemplate;

/* Called first by the start menu's input handler (sub_020358E4 in
 * asm/unk_02035068.s). Nonzero when Select opened Wonder Trade; the handler
 * then returns at once, as it does after any menu action. */
BOOL WonderTrade_StartMenuInput(struct TaskManager *taskManager);

#endif
