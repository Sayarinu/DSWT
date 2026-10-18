#include "wonder_trade/network.h"

#include <dwc.h>
#include <nitroWiFi/socl.h>
#include <nitroWiFi/wcm.h>
#include <string.h>
#include <mbedtls/memory_buffer_alloc.h>
#include <mbedtls/platform.h>
#include <mbedtls/platform_util.h>

#include "constants/net.h"
#include "heap.h"
#include "system.h"
#include "unk_02033200.h"
#include "wifi_overlays.h"
#include "wonder_trade/http.h"
#include "wonder_trade/flash.h"
#include "wonder_trade/seed.h"
#include "wonder_trade/tls.h"
#include "wonder_trade_config.h"

#define WT_ACK "PLATINUMWT_ACK_V1\n"
#define WT_NETWORK_HEAP_SIZE 0x20000
#define WT_STACK_SIZE 0x8000

enum WonderTradeTransportPhase {
    WT_TRANSPORT_CONNECTING,
    WT_TRANSPORT_REQUEST,
    WT_TRANSPORT_DRAINING,
};

typedef struct WonderTradeTransport {
    OSThread thread;
    WtTls tls;
    WtSeed seed;
    WtFlashAccess flash;
    WtHttpResponse response;
    unsigned char arena[WT_TLS_HEAP_SIZE];
    unsigned char input[512];
    unsigned char body[WT_HTTP_BODY_MAX];
    unsigned char stack[WT_STACK_SIZE + 8];
    volatile BOOL cancel;
    volatile int phase;
    int socket;
    volatile int result;
    u32 handshakeMs;
    u32 peakBytes;
    OSTick deadline;
    OSTick lastProgress;
} WonderTradeTransport;

static NNSFndHeapHandle sNetworkHeap;
static WonderTradeTransport *sTransport;

static void *WonderTradeNetwork_Alloc(DWCAllocType type, u32 size, int alignment)
{
    OSIntrMode interrupts = OS_DisableInterrupts();
    void *pointer = NNS_FndAllocFromExpHeapEx(sNetworkHeap, size, alignment);
    OS_RestoreInterrupts(interrupts);
    return pointer;
}

static void WonderTradeNetwork_Free(DWCAllocType type, void *pointer, u32 size)
{
    if (pointer != NULL) {
        OSIntrMode interrupts = OS_DisableInterrupts();
        NNS_FndFreeToExpHeap(sNetworkHeap, pointer);
        OS_RestoreInterrupts(interrupts);
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

static int WonderTradeNetwork_ReadSeed(unsigned char *seed, size_t size)
{
    return WtSeed_Read(&sTransport->seed, seed, size);
}

static int WonderTradeNetwork_WriteSeed(unsigned char *seed, size_t size)
{
    return WtSeed_Write(&sTransport->seed, seed, size);
}

static BOOL WonderTradeNetwork_Continue(WonderTradeTransport *transport)
{
    OSTick now = OS_GetTick();
    return !transport->cancel && now < transport->deadline
        && now - transport->lastProgress < OS_SecondsToTicks(5);
}

static int WonderTradeNetwork_Send(void *arg, const unsigned char *buffer, size_t size)
{
    WonderTradeTransport *transport = arg;
    int result;
    if (!WonderTradeNetwork_Continue(transport)) {
        return MBEDTLS_ERR_SSL_TIMEOUT;
    }
    result = SOCL_WriteNoBlock(transport->socket, buffer, size, 0, 0);
    if (result == SOCL_EWOULDBLOCK || result == SOCL_EINPROGRESS) {
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    }
    if (result > 0) {
        transport->lastProgress = OS_GetTick();
    }
    return result < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : result;
}

static int WonderTradeNetwork_Receive(void *arg, unsigned char *buffer, size_t size)
{
    WonderTradeTransport *transport = arg;
    int result;
    if (!WonderTradeNetwork_Continue(transport)) {
        return MBEDTLS_ERR_SSL_TIMEOUT;
    }
    result = SOCL_ReadNoBlock(transport->socket, buffer, size, NULL, NULL);
    if (result == SOCL_EWOULDBLOCK || result == SOCL_EINPROGRESS) {
        return MBEDTLS_ERR_SSL_WANT_READ;
    }
    if (result > 0) {
        transport->lastProgress = OS_GetTick();
    }
    return result < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : result;
}

static BOOL WonderTradeNetwork_Wait(WonderTradeTransport *transport, int result)
{
    if ((result != MBEDTLS_ERR_SSL_WANT_READ && result != MBEDTLS_ERR_SSL_WANT_WRITE)
        || !WonderTradeNetwork_Continue(transport)) {
        return FALSE;
    }
    OS_Sleep(1);
    return TRUE;
}

static void WonderTradeNetwork_Worker(void *arg)
{
    WonderTradeTransport *transport = arg;
    static const unsigned char pins[2][32] = WONDER_TRADE_PINS;
    static const unsigned char request[] = "GET /v1/ping HTTP/1.1\r\nHost: " WONDER_TRADE_AUTHORITY "\r\nConnection: close\r\n\r\n";
    size_t sent = 0;
    size_t blocks;
    size_t peak;
    int result = -1;
    BOOL tlsStarted = FALSE;
    OSTick start = OS_GetTick();

    sTransport = transport;
    transport->socket = -1;
    transport->deadline = start + OS_SecondsToTicks(30);
    transport->lastProgress = start;
    mbedtls_memory_buffer_alloc_init(transport->arena, sizeof(transport->arena));
    mbedtls_platform_set_nv_seed(WonderTradeNetwork_ReadSeed, WonderTradeNetwork_WriteSeed);
    transport->flash.requireDurability = TRUE;
    result = WtSeed_Open(&transport->seed, &transport->flash,
        WtFlash_ReadBank, WtFlash_WriteBank);
    if (transport->cancel || result != 0) {
        result = transport->flash.error != 0 ? transport->flash.error : result;
        goto done;
    }
    transport->socket = SOCL_TcpSocket();
    if (transport->socket <= 0) {
        result = -11;
        goto done;
    }
    SOCL_SetNoBlock(transport->socket);
    do {
        result = SOCL_Connect(transport->socket, WONDER_TRADE_PORT, WONDER_TRADE_IPV4);
        if (result != SOCL_EINPROGRESS && result != SOCL_EALREADY) {
            break;
        }
        OS_Sleep(1);
    } while (!transport->cancel && OS_GetTick() < transport->deadline);
    if (result != 0 || transport->cancel) {
        goto done;
    }
    tlsStarted = TRUE;
    result = WtTls_Open(&transport->tls, WONDER_TRADE_HOSTNAME, pins, transport,
        WonderTradeNetwork_Send, WonderTradeNetwork_Receive);
    transport->lastProgress = OS_GetTick();
    if (result != 0) {
        goto done;
    }
    do {
        result = WtTls_Handshake(&transport->tls);
    } while (WonderTradeNetwork_Wait(transport, result));
    if (result != 0 || !WonderTradeNetwork_Continue(transport)) {
        result = result == 0 ? MBEDTLS_ERR_SSL_TIMEOUT : result;
        goto done;
    }
    transport->handshakeMs = OS_TicksToMilliSeconds(OS_GetTick() - start);
    transport->deadline = OS_GetTick() + OS_SecondsToTicks(15);
    transport->lastProgress = OS_GetTick();
    transport->phase = WT_TRANSPORT_REQUEST;
    while (sent < sizeof(request) - 1) {
        result = WtTls_Write(&transport->tls, request + sent, sizeof(request) - 1 - sent);
        if (result > 0) {
            sent += result;
        } else if (!WonderTradeNetwork_Wait(transport, result)) {
            goto done;
        }
    }
    WtHttp_Init(&transport->response);
    while (!transport->response.complete) {
        result = WtTls_Read(&transport->tls, transport->input, sizeof(transport->input));
        if (result > 0) {
            if (WtHttp_Feed(&transport->response, transport->input, result,
                    transport->body, sizeof(transport->body)) < 0) {
                result = -12;
                goto done;
            }
        } else if (!WonderTradeNetwork_Wait(transport, result)) {
            result = -13;
            goto done;
        }
    }
    result = transport->response.bodySize == sizeof(WT_ACK) - 1
        && memcmp(transport->body, WT_ACK, sizeof(WT_ACK) - 1) == 0 ? 0 : -14;
done:
    if (tlsStarted) {
        WtTls_Close(&transport->tls);
    }
    transport->result = result;
    transport->phase = WT_TRANSPORT_DRAINING;
    if (transport->socket > 0) {
        SOCL_Close(transport->socket);
        while (!SOCL_IsClosed(transport->socket)) {
            SOCLi_TrashSocket();
            OS_Sleep(1);
        }
    }
    mbedtls_memory_buffer_alloc_max_get(&peak, &blocks);
    transport->peakBytes = peak;
    mbedtls_memory_buffer_alloc_free();
    mbedtls_platform_zeroize(&transport->seed, sizeof(transport->seed));
    mbedtls_platform_zeroize(transport->arena, sizeof(transport->arena));
    transport->result = transport->cancel ? -15 : result;
    sTransport = NULL;
}

static BOOL WonderTradeNetwork_FinishWorker(WonderTradeNetwork *network)
{
    WonderTradeTransport *transport = network->transport;
    const unsigned char *stack;
    u32 size;
    u32 unused;
    if (transport == NULL) {
        return TRUE;
    }
    if (!OS_IsThreadTerminated(&transport->thread)) {
        return FALSE;
    }
    OS_JoinThread(&transport->thread);
    network->error = transport->result;
    if (network->timedOut) {
        network->error = -6;
    }
    network->handshakeMs = transport->handshakeMs;
    network->tlsPeakBytes = transport->peakBytes;
    stack = (const unsigned char *)(transport->thread.stackTop + sizeof(u32));
    size = transport->thread.stackBottom - transport->thread.stackTop - 2 * sizeof(u32);
    for (unused = 0; unused < size && stack[unused] == 0xa5; unused++) {
        (void)0;
    }
    network->stackBytes = size - unused;
    if (unused < 256 || OS_GetStackStatus(&transport->thread) != OS_STACK_NO_ERROR) {
        network->error = -16;
    }
    mbedtls_platform_zeroize(transport, sizeof(*transport));
    Heap_Free(transport);
    network->transport = NULL;
    return TRUE;
}

static void WonderTradeNetwork_Disconnect(WonderTradeNetwork *network)
{
    if (network->accessPointStarted && WCM_GetPhase() == WCM_PHASE_DCF) {
        int result = WCM_DisconnectAsync();
        /* REJECT leaves DCF intact: retry on the next frame, without blocking. */
        if (result == WCM_RESULT_FAILURE || result == WCM_RESULT_FATAL_ERROR) {
            network->error = -17;
        }
    }
}

void WonderTradeNetwork_Start(WonderTradeNetwork *network, enum HeapID heapID)
{
    memset(network, 0, sizeof(*network));
    network->heapID = heapID;
    network->heapBuffer = Heap_Alloc(heapID, WT_NETWORK_HEAP_SIZE + 32);
    network->heap = NNS_FndCreateExpHeap((void *)(((u32)network->heapBuffer + 31) & ~31), WT_NETWORK_HEAP_SIZE);
    sNetworkHeap = network->heap;
    Overlay_LoadWFCOverlay();
    WirelessDriver_Init();
    network->state = WT_NET_STARTING;
    network->deadline = OS_GetTick() + OS_SecondsToTicks(30);
}

void WonderTradeNetwork_Ping(WonderTradeNetwork *network)
{
    WonderTradeTransport *transport;
    void *stackTop;
    if (network->transport != NULL || !network->accessPointStarted || WCM_GetPhase() != WCM_PHASE_DCF) {
        network->error = -2;
        network->state = WT_NET_ERROR;
        return;
    }
    transport = Heap_Alloc(network->heapID, sizeof(*transport));
    memset(transport, 0, sizeof(*transport));
    network->transport = transport;
    network->lastUpdate = OS_GetTick();
    network->deadline = network->lastUpdate + OS_SecondsToTicks(30);
    network->requestStarted = FALSE;
    network->timedOut = FALSE;
    network->maxFrameMs = 0;
    network->state = WT_NET_PINGING;
    stackTop = (void *)(((u32)transport->stack + WT_STACK_SIZE) & ~7);
    OS_CreateThread(&transport->thread, WonderTradeNetwork_Worker, transport,
        stackTop, WT_STACK_SIZE - 8, OS_GetThreadPriority(OS_GetCurrentThread()) + 1);
    /* Creation clears the interior and writes SDK guards; preserve those guards. */
    memset((void *)(transport->thread.stackTop + sizeof(u32)), 0xa5,
        transport->thread.stackBottom - transport->thread.stackTop - 2 * sizeof(u32));
    OS_WakeupThreadDirect(&transport->thread);
}

void WonderTradeNetwork_Close(WonderTradeNetwork *network)
{
    WonderTradeTransport *transport = network->transport;
    if (transport != NULL) {
        transport->cancel = TRUE;
    }
    network->state = WT_NET_CLOSING;
}

void WonderTradeNetwork_Update(WonderTradeNetwork *network)
{
    if (network->state == WT_NET_CLOSED) {
        return;
    }
    if (network->state == WT_NET_CLOSING) {
        /* Dropping our AP aborts long SDK ARP/TCP retries; retain all owned memory. */
        WonderTradeNetwork_Disconnect(network);
        if (!WonderTradeNetwork_FinishWorker(network)) {
            return;
        }
        if (network->accessPointStarted) {
            if (!DWC_AC_Destroy()) {
                return;
            }
            network->accessPointStarted = FALSE;
        }
        if (!network->driverStopping) {
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
        config.option.skipNetCheck = TRUE;
        if (!DWC_AC_Create(&config)) {
            network->error = -3;
            network->state = WT_NET_ERROR;
            return;
        }
        network->accessPointStarted = TRUE;
        network->state = WT_NET_CONNECTING;
    }
    if (network->state == WT_NET_CONNECTING) {
        DWC_AC_Process();
        int status = DWC_AC_GetStatus();
        if (status == DWC_AC_STATE_COMPLETE) {
            network->state = WT_NET_READY;
        } else if (status < 0) {
            network->error = status;
            network->state = WT_NET_ERROR;
        }
    }
    if (network->state == WT_NET_PINGING) {
        WonderTradeTransport *transport = network->transport;
        OSTick now = OS_GetTick();
        u32 elapsed = OS_TicksToMilliSeconds(now - network->lastUpdate);
        if (elapsed > network->maxFrameMs) {
            network->maxFrameMs = elapsed;
        }
        network->lastUpdate = now;
        if (transport->phase == WT_TRANSPORT_REQUEST && !network->requestStarted) {
            network->requestStarted = TRUE;
            network->deadline = now + OS_SecondsToTicks(15);
        }
        if (now >= network->deadline) {
            network->timedOut = TRUE;
            transport->cancel = TRUE;
        }
        if (transport->cancel || (transport->phase == WT_TRANSPORT_DRAINING && transport->result != 0)) {
            WonderTradeNetwork_Disconnect(network);
        }
        if (WonderTradeNetwork_FinishWorker(network)) {
            network->state = network->error == 0 ? WT_NET_ACKNOWLEDGED : WT_NET_ERROR;
        }
    }
    if ((network->state == WT_NET_STARTING || network->state == WT_NET_CONNECTING)
        && OS_GetTick() >= network->deadline) {
        network->error = -6;
        network->state = WT_NET_ERROR;
    }
}
