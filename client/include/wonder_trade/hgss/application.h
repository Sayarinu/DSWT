#ifndef WONDER_TRADE_HGSS_APPLICATION_H
#define WONDER_TRADE_HGSS_APPLICATION_H

#include "overlay_manager.h"

/* Resident templates that load the wonder_trade overlay for the application. */
extern const OverlayManagerTemplate gWonderTradeAppTemplate;
extern const OverlayManagerTemplate gWonderTradeBootAppTemplate;

/* In the wonder_trade overlay: call only through the templates above. */
BOOL WonderTradeApp_Init(OverlayManager *man, int *state);
BOOL WonderTradeApp_BootInit(OverlayManager *man, int *state);
BOOL WonderTradeApp_Main(OverlayManager *man, int *state);
BOOL WonderTradeApp_Exit(OverlayManager *man, int *state);

#endif
