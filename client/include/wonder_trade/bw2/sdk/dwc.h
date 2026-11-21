#ifndef WONDER_TRADE_BW2_SDK_DWC_H
#define WONDER_TRADE_BW2_SDK_DWC_H

/* TWL-DWC 5.3's auto-connect and memory functions, as pokebw2 declares them. */

#include "dwc/ac.h"
#include "dwc/error.h"
#include "dwc/memfunc.h"

/* DWC_AC_GetStatus reports 5 once the connection is up (dwc/ac.h). */
#define DWC_AC_STATE_COMPLETE 5

/* TWL-DWC has no connection-test option block; the test has its own bit. */
#define WT_DWC_SKIP_NET_CHECK(config) ((config).skipConnectionTest = 1)

#endif
