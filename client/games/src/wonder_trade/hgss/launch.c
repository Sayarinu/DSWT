#include "wonder_trade/hgss/application.h"

#include <nitro.h>

#include "global.h"

/* The screen, its boot check and all they use, Mbed TLS included, are in the
 * wonder_trade overlay. The overlay manager loads it before Init and unloads
 * it after Exit, so only these templates need to stay resident. */
FS_EXTERN_OVERLAY(wonder_trade);

const OverlayManagerTemplate gWonderTradeAppTemplate = {
    WonderTradeApp_Init, WonderTradeApp_Main, WonderTradeApp_Exit, FS_OVERLAY_ID(wonder_trade)
};

const OverlayManagerTemplate gWonderTradeBootAppTemplate = {
    WonderTradeApp_BootInit, WonderTradeApp_Main, WonderTradeApp_Exit, FS_OVERLAY_ID(wonder_trade)
};
