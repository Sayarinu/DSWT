#ifndef WONDER_TRADE_BW_SDK_WCM_H
#define WONDER_TRADE_BW_SDK_WCM_H

/* NitroWiFi's wireless connection manager in overlay 9. */

#include <nitro.h>

#define WCM_PHASE_DCF 9
#define WCM_RESULT_FAILURE 1
#define WCM_RESULT_FATAL_ERROR 7

int WCM_GetPhase(void);
int WCM_DisconnectAsync(void);

#endif
