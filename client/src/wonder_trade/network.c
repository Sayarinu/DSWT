#include "wonder_trade/network.h"

#include <dwc.h>
#include <nitroWiFi/socl.h>
#include <nitroWiFi/wcm.h>
#include <string.h>
#include <mbedtls/memory_buffer_alloc.h>
#include <mbedtls/platform.h>
#include <mbedtls/platform_util.h>
#include <psa/crypto.h>

#include "constants/net.h"
#include "heap.h"
#include "system.h"
#include "unk_02033200.h"
#include "wifi_overlays.h"
#include "wonder_trade/http.h"
#include "wonder_trade/flash.h"
#include "wonder_trade/seed.h"
#include "wonder_trade/game_save.h"
#include "wonder_trade/recovery.h"
#include "wonder_trade/tls.h"
#include "wonder_trade_config.h"

#define WT_ACK "PLATINUMWT_ACK_V1\n"
#define WT_NETWORK_HEAP_SIZE 0x20000
#define WT_STACK_SIZE 0x8000

enum WonderTradeTransportPhase {
    WT_TRANSPORT_CONNECTING,
    WT_TRANSPORT_REQUEST,
    WT_TRANSPORT_SAVING,
    WT_TRANSPORT_DRAINING,
};

typedef struct WonderTradeTransport {
    OSThread thread;
    WtTls tls;
    WtSeed seed;
    WtEngine engine;
    WtGameSave game;
    BOOL trading;
    int slot;
    unsigned action;
    BOOL completed, returned, rejected, waiting;
    unsigned tradePhase;
    BOOL canLeave;
    WtFlashAccess flash;
    WtHttpExchange http;
    WtWireReply reply;
    unsigned char request[WT_WIRE_REQUEST_MAX];
    size_t requestSize;
    unsigned char arena[WT_TLS_HEAP_SIZE];
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
    return WtSeed_Read(sTransport->trading ? &sTransport->engine.journal : &sTransport->seed, seed, size);
}

static int WonderTradeNetwork_WriteSeed(unsigned char *seed, size_t size)
{
    return WtSeed_Write(sTransport->trading ? &sTransport->engine.journal : &sTransport->seed, seed, size);
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

static int WonderTradeNetwork_WriteHttp(void *arg, const unsigned char *bytes, size_t size)
{
    return WtTls_Write(&((WonderTradeTransport *)arg)->tls, bytes, size);
}

static int WonderTradeNetwork_ReadHttp(void *arg, unsigned char *bytes, size_t size)
{
    return WtTls_Read(&((WonderTradeTransport *)arg)->tls, bytes, size);
}

static int WonderTradeNetwork_WaitHttp(void *arg, int result)
{
    return WonderTradeNetwork_Wait(arg, result);
}

static int WonderTradeNetwork_Random(void *arg, unsigned char *bytes, size_t size)
{
    (void)arg;
    return psa_generate_random(bytes, size) == PSA_SUCCESS ? 0 : -1;
}

static int WonderTradeNetwork_Inventory(void *arg, enum WtInventoryStep step, WtJournalEntry *entry, unsigned slot)
{
    WonderTradeTransport *transport = arg;
    int phase = transport->phase;
    int result;
    transport->phase = WT_TRANSPORT_SAVING;
    result = WtGameSave_Inventory(&transport->game, step, entry, slot);
    transport->phase = phase;
    return result;
}

static void WonderTradeNetwork_DrainSocket(WonderTradeTransport *transport)
{
    if (transport->socket > 0) {
        SOCL_Close(transport->socket);
        while (!SOCL_IsClosed(transport->socket)) {
            SOCLi_TrashSocket(); OS_Sleep(1);
        }
        transport->socket = -1;
    }
}

static int WonderTradeNetwork_OneExchange(WonderTradeTransport *transport)
{
    static const unsigned char pins[2][32] = WONDER_TRADE_PINS;
    WtHttpIO io = { transport, WonderTradeNetwork_WriteHttp, WonderTradeNetwork_ReadHttp, WonderTradeNetwork_WaitHttp };
    int result = -1;
    BOOL tlsStarted = FALSE;
    OSTick start = OS_GetTick();
    transport->deadline = start + OS_SecondsToTicks(30);
    transport->lastProgress = start;
    transport->phase = WT_TRANSPORT_CONNECTING;
    transport->socket = SOCL_TcpSocket();
    if (transport->socket <= 0) return -11;
    SOCL_SetNoBlock(transport->socket);
    do {
        result = SOCL_Connect(transport->socket, WONDER_TRADE_PORT, WONDER_TRADE_IPV4);
        if (result != SOCL_EINPROGRESS && result != SOCL_EALREADY) break;
        OS_Sleep(1);
    } while (!transport->cancel && OS_GetTick() < transport->deadline);
    if (result != 0 || transport->cancel) goto done;
    tlsStarted = TRUE;
    result = WtTls_Open(&transport->tls, WONDER_TRADE_HOSTNAME, pins, transport,
        WonderTradeNetwork_Send, WonderTradeNetwork_Receive);
    transport->lastProgress = OS_GetTick();
    if (result != 0) goto done;
    do { result = WtTls_Handshake(&transport->tls); } while (WonderTradeNetwork_Wait(transport, result));
    if (result != 0 || !WonderTradeNetwork_Continue(transport)) {
        result = result == 0 ? MBEDTLS_ERR_SSL_TIMEOUT : result; goto done;
    }
    transport->handshakeMs = OS_TicksToMilliSeconds(OS_GetTick() - start);
    transport->deadline = OS_GetTick() + OS_SecondsToTicks(15);
    transport->lastProgress = OS_GetTick(); transport->phase = WT_TRANSPORT_REQUEST;
    if (WtHttp_Exchange(&transport->http, io, WONDER_TRADE_AUTHORITY,
            transport->requestSize ? transport->request : NULL, transport->requestSize) != 0) {
        result = -12; goto done;
    }
    if (transport->requestSize) {
        result = WtWire_Decode(transport->request, transport->requestSize,
            transport->http.body, transport->http.bodySize, WtFlash_Hash, &transport->reply) == 0 ? 0 : -18;
        if (result == 0 && transport->trading) result = WtEngine_Reply(&transport->engine, transport->http.body, transport->http.bodySize);
    } else {
        result = transport->http.bodySize == sizeof(WT_ACK) - 1
            && memcmp(transport->http.body, WT_ACK, sizeof(WT_ACK) - 1) == 0 ? 0 : -14;
    }
done:
    mbedtls_platform_zeroize(transport->request, sizeof(transport->request));
    mbedtls_platform_zeroize(&transport->http, sizeof(transport->http));
    if (tlsStarted) WtTls_Close(&transport->tls);
    WonderTradeNetwork_DrainSocket(transport);
    return result;
}

static int WonderTradeNetwork_RunTrade(WonderTradeTransport *transport)
{
    WtJournalIO journal = { &transport->flash, WtFlash_ReadBank, WtFlash_WriteBank, WtFlash_Hash };
    WtEngineIO inventory = { transport, WonderTradeNetwork_Random, WonderTradeNetwork_Inventory };
    WtEngine *engine = &transport->engine;
    unsigned count, operation, wireResult;
    int result = -1;
    if (WtEngine_Open(engine, journal, inventory) != 0) return -30;
    WtRecovery_Block();
    for (count = 0; count < 24 && !transport->cancel; ++count) {
        /* TLS teardown releases PSA; initialize it before the next nonce.
         * Its NV-seed update uses this same active engine journal handle. */
        if (psa_crypto_init() != PSA_SUCCESS) { result = -31; break; }
        if (WtJournal_Read(&engine->journal, &engine->entry) != 0) { result = -30; break; }
        if (engine->entry.phase == WT_JOURNAL_EMPTY && WtEngine_Enroll(engine, 1) != 0) { result = -30; break; }
        if (engine->entry.phase == WT_JOURNAL_IDLE && engine->synchronized) {
            if (transport->slot < 0) { result = 0; break; }
            transport->phase = WT_TRANSPORT_SAVING;
            result = WtGameSave_Checkpoint(&transport->game);
            transport->phase = WT_TRANSPORT_CONNECTING;
            if (transport->cancel) { result = -15; break; }
            if (result != 0 || WtEngine_Deposit(engine, transport->slot) != 0) { result = -32; break; }
            transport->slot = -1;
        }
        operation = transport->action;
        if (engine->entry.deliveryKind == WT_DELIVERY_RETURN) operation = WT_OP_COLLECT;
        if (engine->lastResult == WT_RESULT_ILLEGAL_POKEMON
            || (engine->entry.requestLength && engine->entry.request[5] == WT_OP_CANCEL
                && (engine->lastResult == WT_RESULT_CONFLICT || engine->lastResult == WT_RESULT_NOT_FOUND))) operation = WT_OP_RECONCILE;
        if (operation == WT_OP_CANCEL && engine->entry.deliveryKind == WT_DELIVERY_MATCH) operation = WT_OP_COLLECT;
        result = WtEngine_Request(engine, operation);
        if (result != 0) { result = result < 0 ? -30 : 0; break; }
        transport->requestSize = engine->frameSize;
        memcpy(transport->request, engine->frame, engine->frameSize);
        operation = transport->request[5];
        if (operation == WT_OP_ACKNOWLEDGE) transport->returned = engine->entry.deliveryKind == WT_DELIVERY_RETURN;
        result = WonderTradeNetwork_OneExchange(transport);
        if (result < 0) break;
        wireResult = transport->reply.result;
        if (wireResult == WT_RESULT_ILLEGAL_POKEMON) transport->rejected = TRUE;
        if (operation == WT_OP_ACKNOWLEDGE && wireResult == WT_RESULT_OK) transport->completed = TRUE;
        /* Collect reports CONFLICT only while the offer is still queued; the journal stays QUEUED. */
        if (operation == WT_OP_COLLECT && wireResult == WT_RESULT_CONFLICT) { transport->waiting = TRUE; result = 0; break; }
        if (wireResult != WT_RESULT_OK && wireResult != WT_RESULT_ILLEGAL_POKEMON
            && !(operation == WT_OP_CANCEL && (wireResult == WT_RESULT_CONFLICT || wireResult == WT_RESULT_NOT_FOUND))) {
            result = -100 - wireResult; break;
        }
        if (operation == WT_OP_STATUS) { result = 0; break; }
    }
    if (count == 24) result = -33;
    transport->tradePhase = engine->entry.phase;
    transport->phase = WT_TRANSPORT_SAVING;
    transport->canLeave = !engine->blocked && WtGameSave_CanLeave(&transport->game, &engine->entry);
    /* The main thread also checks the worker stack before opening gameplay. */
    WtRecovery_Block();
    return result;
}

static void WonderTradeNetwork_Worker(void *arg)
{
    WonderTradeTransport *transport = arg;
    size_t blocks, peak;
    int result = -1;
    sTransport = transport;
    transport->socket = -1;
    mbedtls_memory_buffer_alloc_init(transport->arena, sizeof(transport->arena));
    mbedtls_platform_set_nv_seed(WonderTradeNetwork_ReadSeed, WonderTradeNetwork_WriteSeed);
    transport->flash.requireDurability = TRUE;
    if (transport->trading) {
        result = WonderTradeNetwork_RunTrade(transport);
    } else {
        if (transport->requestSize && WtWire_CheckRequest(transport->request, transport->requestSize, WtFlash_Hash) != 0) result = -18;
        else {
            result = transport->requestSize
                ? WtSeed_OpenRecovery(&transport->seed, &transport->flash, WtFlash_ReadBank, WtFlash_WriteBank)
                : WtSeed_Open(&transport->seed, &transport->flash, WtFlash_ReadBank, WtFlash_WriteBank);
            if (result == 0 && transport->requestSize && !WtSeed_RequestMatches(&transport->seed, transport->request, transport->requestSize)) result = -18;
            if (result == 0 && !transport->cancel) result = WonderTradeNetwork_OneExchange(transport);
        }
    }
    transport->result = transport->flash.error ? transport->flash.error : result;
    transport->phase = WT_TRANSPORT_DRAINING;
    WonderTradeNetwork_DrainSocket(transport);
    mbedtls_psa_crypto_free();
    mbedtls_memory_buffer_alloc_max_get(&peak, &blocks); transport->peakBytes = peak;
    mbedtls_memory_buffer_alloc_free();
    WtEngine_Close(&transport->engine);
    mbedtls_platform_zeroize(&transport->seed, sizeof(transport->seed));
    mbedtls_platform_zeroize(transport->arena, sizeof(transport->arena));
    if (transport->cancel) transport->result = -15;
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
    } else if (transport->cancel) {
        network->error = -15;
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
        transport->canLeave = FALSE;
    }
    if (transport->trading) {
        network->tradePhase = transport->tradePhase; network->canLeave = transport->canLeave;
        network->completed = transport->completed; network->returned = transport->returned; network->rejected = transport->rejected;
        network->waiting = transport->waiting;
        if (network->canLeave) WtRecovery_Allow(network->tradePhase);
        else WtRecovery_Block();
    }
    if (network->error == 0 && transport->requestSize) network->reply = transport->reply;
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
    network->canLeave = WtRecovery_CanWrite();
    network->heapBuffer = Heap_Alloc(heapID, WT_NETWORK_HEAP_SIZE + 32);
    network->heap = NNS_FndCreateExpHeap((void *)(((u32)network->heapBuffer + 31) & ~31), WT_NETWORK_HEAP_SIZE);
    sNetworkHeap = network->heap;
    Overlay_LoadWFCOverlay();
    WirelessDriver_Init();
    network->state = WT_NET_STARTING;
    network->deadline = OS_GetTick() + OS_SecondsToTicks(30);
}

static BOOL WonderTradeNetwork_BeginRequest(WonderTradeNetwork *network, const unsigned char *frame, size_t size, SaveData *save, int slot, unsigned action)
{
    WonderTradeTransport *transport;
    void *stackTop;
    mbedtls_platform_zeroize(&network->reply, sizeof(network->reply));
    /* A rejected call must not orphan or replace an already running worker. */
    if (network->transport != NULL || network->state == WT_NET_CLOSING || network->state == WT_NET_CLOSED) return FALSE;
    if (!network->accessPointStarted || WCM_GetPhase() != WCM_PHASE_DCF
        || ((frame == NULL) != (size == 0)) || size > WT_WIRE_REQUEST_MAX) {
        network->error = -2;
        network->state = WT_NET_ERROR;
        return FALSE;
    }
    transport = Heap_Alloc(network->heapID, sizeof(*transport));
    if (transport == NULL) {
        network->error = -1;
        network->state = WT_NET_ERROR;
        return FALSE;
    }
    memset(transport, 0, sizeof(*transport));
    transport->requestSize = size;
    transport->trading = save != NULL; transport->game.save = save;
    transport->game.heapID = network->heapID; transport->slot = slot;
    transport->action = action;
    if (save) network->canLeave = FALSE;
    if (size) memcpy(transport->request, frame, size);
    network->transport = transport;
    network->error = 0;
    network->lastUpdate = OS_GetTick();
    network->deadline = network->lastUpdate + OS_SecondsToTicks(save ? 180 : 30);
    network->requestStarted = FALSE;
    network->timedOut = FALSE;
    network->maxFrameMs = 0;
    network->state = size || save ? WT_NET_EXCHANGING : WT_NET_PINGING;
    stackTop = (void *)(((u32)transport->stack + WT_STACK_SIZE) & ~7);
    OS_CreateThread(&transport->thread, WonderTradeNetwork_Worker, transport,
        stackTop, WT_STACK_SIZE - 8, OS_GetThreadPriority(OS_GetCurrentThread()) + 1);
    /* Creation clears the interior and writes SDK guards; preserve those guards. */
    memset((void *)(transport->thread.stackTop + sizeof(u32)), 0xa5,
        transport->thread.stackBottom - transport->thread.stackTop - 2 * sizeof(u32));
    OS_WakeupThreadDirect(&transport->thread);
    return TRUE;
}

void WonderTradeNetwork_Ping(WonderTradeNetwork *network)
{
    WonderTradeNetwork_BeginRequest(network, NULL, 0, NULL, -1, 0);
}

BOOL WonderTradeNetwork_Exchange(WonderTradeNetwork *network, const unsigned char *frame, size_t size)
{
    if (!frame || size == 0) {
        mbedtls_platform_zeroize(&network->reply, sizeof(network->reply));
        return FALSE;
    }
    return WonderTradeNetwork_BeginRequest(network, frame, size, NULL, -1, 0);
}

BOOL WonderTradeNetwork_Trade(WonderTradeNetwork *network, SaveData *save, int slot)
{
    return save && WonderTradeNetwork_BeginRequest(network, NULL, 0, save, slot, 0);
}

BOOL WonderTradeNetwork_Collect(WonderTradeNetwork *network, SaveData *save, BOOL cancel)
{
    return save && WonderTradeNetwork_BeginRequest(network, NULL, 0, save, -1, cancel ? WT_OP_CANCEL : WT_OP_COLLECT);
}

void WonderTradeNetwork_Close(WonderTradeNetwork *network)
{
    WonderTradeTransport *transport = network->transport;
    mbedtls_platform_zeroize(&network->reply, sizeof(network->reply));
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
    if (network->state == WT_NET_PINGING || network->state == WT_NET_EXCHANGING) {
        WonderTradeTransport *transport = network->transport;
        BOOL exchanging = transport->trading || transport->requestSize != 0;
        OSTick now = OS_GetTick();
        u32 elapsed = OS_TicksToMilliSeconds(now - network->lastUpdate);
        if (elapsed > network->maxFrameMs) {
            network->maxFrameMs = elapsed;
        }
        network->lastUpdate = now;
        if (transport->trading && transport->phase == WT_TRANSPORT_SAVING) {
            network->deadline = now + OS_SecondsToTicks(180);
        }
        if (!transport->trading && transport->phase == WT_TRANSPORT_REQUEST && !network->requestStarted) {
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
            network->state = network->error == 0 ? (exchanging ? WT_NET_RESPONSE : WT_NET_ACKNOWLEDGED) : WT_NET_ERROR;
        }
    }
    if ((network->state == WT_NET_STARTING || network->state == WT_NET_CONNECTING)
        && OS_GetTick() >= network->deadline) {
        network->error = -6;
        network->state = WT_NET_ERROR;
    }
}
