#include "wonder_trade/network.h"

#include <dwc.h>
#include <nitroWiFi/wcm.h>
#include <string.h>

#include "constants/net.h"

#include "heap.h"
#include "unk_02033200.h"
#include "wifi_overlays.h"
#include "wonder_trade_config.h"

#define WT_ACK               "PLATINUMWT_ACK_V1\n"
#define WT_NETWORK_HEAP_SIZE 0x20000

static NNSFndHeapHandle sNetworkHeap;

static void *WonderTradeNetwork_Alloc(DWCAllocType type, u32 size, int alignment)
{
    return NNS_FndAllocFromExpHeapEx(sNetworkHeap, size, alignment);
}

static void WonderTradeNetwork_Free(DWCAllocType type, void *pointer, u32 size)
{
    if (pointer != NULL) {
        NNS_FndFreeToExpHeap(sNetworkHeap, pointer);
    }
}

static void *WonderTradeNetwork_AccessPointAlloc(u32 type, s32 size)
{
    return DWC_Alloc((DWCAllocType)type, size);
}

static void WonderTradeNetwork_AccessPointFree(u32 type, void *pointer, s32 size)
{
    DWC_Free((DWCAllocType)type, pointer, size);
}

static void WonderTradeNetwork_Fail(WonderTradeNetwork *network, int error)
{
    if (network->request >= 0) {
        DWC_CancelGHTTPRequest(network->request);
        network->request = -1;
    }
    network->error = error;
    network->state = WT_NET_ERROR;
}

static void WonderTradeNetwork_Completed(const char *buffer, int length, DWCGHTTPResult result, void *arg)
{
    WonderTradeNetwork *network = arg;

    network->request = -1;
    if (network->state != WT_NET_PINGING) {
        return;
    }

    if (result == DWC_GHTTP_SUCCESS && buffer != NULL
        && length == sizeof(WT_ACK) - 1 && memcmp(buffer, WT_ACK, sizeof(WT_ACK) - 1) == 0) {
        network->state = WT_NET_ACKNOWLEDGED;
    } else {
        network->error = result == DWC_GHTTP_SUCCESS ? -1 : result;
        network->state = WT_NET_ERROR;
    }
}

void WonderTradeNetwork_Start(WonderTradeNetwork *network, enum HeapID heapID)
{
    memset(network, 0, sizeof(*network));
    network->request = -1;
    network->heapBuffer = Heap_Alloc(heapID, WT_NETWORK_HEAP_SIZE + 32);
    network->heap = NNS_FndCreateExpHeap((void *)(((u32)network->heapBuffer + 31) & ~31), WT_NETWORK_HEAP_SIZE);
    sNetworkHeap = network->heap;

    Overlay_LoadWFCOverlay();
    Overlay_LoadHttpOverlay();
    WirelessDriver_Init();
    network->state = WT_NET_STARTING;
    network->deadline = OS_GetTick() + OS_SecondsToTicks(30);
}

void WonderTradeNetwork_Ping(WonderTradeNetwork *network)
{
    DWC_ClearError();
    if (!network->accessPointStarted || WCM_GetPhase() != WCM_PHASE_DCF) {
        WonderTradeNetwork_Fail(network, -2);
        return;
    }

    network->state = WT_NET_PINGING;
    network->deadline = OS_GetTick() + OS_SecondsToTicks(10);
    network->request = DWC_GetGHTTPDataEx(WONDER_TRADE_BASE_URL "/v1/ping", 64, TRUE, NULL, WonderTradeNetwork_Completed, network);
    if (network->request < 0) {
        WonderTradeNetwork_Fail(network, network->request);
    }
}

void WonderTradeNetwork_Close(WonderTradeNetwork *network)
{
    network->state = WT_NET_CLOSING;
    if (network->request >= 0) {
        DWC_CancelGHTTPRequest(network->request);
        network->request = -1;
    }
    if (network->httpStarted) {
        DWC_ShutdownGHTTP();
        network->httpStarted = FALSE;
    }
}

void WonderTradeNetwork_Update(WonderTradeNetwork *network)
{
    if (network->state == WT_NET_CLOSED) {
        return;
    }

    if (network->state == WT_NET_CLOSING) {
        if (network->accessPointStarted) {
            if (!DWC_AC_Destroy()) {
                return;
            }
            network->accessPointStarted = FALSE;
        }
        if (!network->driverStopping) {
            // Startup and teardown both use ARM7 asynchronously.
            if (!WirelessDriver_IsReady()) {
                return;
            }
            WirelessDriver_Shutdown();
            network->driverStopping = TRUE;
        }
        if (WirelessDriver_Initialized()) {
            return;
        }

        DWC_ClearError();
        DWC_SetMemFunc(NULL, NULL);
        NNS_FndDestroyExpHeap(network->heap);
        Heap_Free(network->heapBuffer);
        sNetworkHeap = NULL;
        Overlay_UnloadHttpOverlay();
        Overlay_UnloadWFCOverlay();
        network->state = WT_NET_CLOSED;
        return;
    }

    if (network->state == WT_NET_STARTING && WirelessDriver_IsReady()) {
        DWCACConfig config;

        DWC_SetMemFunc(WonderTradeNetwork_Alloc, WonderTradeNetwork_Free);
        DWC_ClearError();
        memset(&config, 0, sizeof(config));
        config.alloc = WonderTradeNetwork_AccessPointAlloc;
        config.free = WonderTradeNetwork_AccessPointFree;
        config.dmaNo = DEFAULT_DWC_DMA_NUMBER;
        config.powerMode = DEFAULT_DWC_POWER_MODE;
        // Test our own server after selection; Nintendo's netcheck is unrelated.
        config.option.skipNetCheck = TRUE;
        if (!DWC_AC_Create(&config)) {
            WonderTradeNetwork_Fail(network, -3);
            return;
        }
        network->accessPointStarted = TRUE;
        network->state = WT_NET_CONNECTING;
    }

    if (network->state == WT_NET_CONNECTING) {
        DWC_AC_Process();
        int status = DWC_AC_GetStatus();
        if (status == DWC_AC_STATE_COMPLETE) {
            if (!DWC_InitGHTTP(NULL)) {
                WonderTradeNetwork_Fail(network, -4);
                return;
            }
            network->httpStarted = TRUE;
            network->state = WT_NET_READY;
        } else if (status < 0) {
            WonderTradeNetwork_Fail(network, status);
        }
    }

    if (network->state == WT_NET_PINGING) {
        if (!DWC_ProcessGHTTP() && network->state == WT_NET_PINGING) {
            WonderTradeNetwork_Fail(network, -5);
        }
    }

    if ((network->state == WT_NET_STARTING || network->state == WT_NET_CONNECTING
            || network->state == WT_NET_PINGING)
        && OS_GetTick() >= network->deadline) {
        WonderTradeNetwork_Fail(network, -6);
    }
}
