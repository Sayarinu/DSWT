#ifndef WONDER_TRADE_DP_SDK_WCM_H
#define WONDER_TRADE_DP_SDK_WCM_H

/* The wireless connection manager calls the shared network core makes,
 * declared for Diamond and Pearl's static WCM library (arm9/asm/WCM_system.s). */

/* WCM_DisconnectAsync acts only in this phase, the connected (DCF) state. */
#define WCM_PHASE_DCF 9

/* WCM_DisconnectAsync's results. */
#define WCM_RESULT_SUCCESS 0
#define WCM_RESULT_FAILURE 1
#define WCM_RESULT_PROGRESS 2
#define WCM_RESULT_ACCEPT 3
#define WCM_RESULT_REJECT 4
#define WCM_RESULT_FATAL_ERROR 7

int WCM_GetPhase(void);
int WCM_DisconnectAsync(void);

#endif
