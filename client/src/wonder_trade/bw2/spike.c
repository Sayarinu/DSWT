/* LUNE-830 spike, debug builds only: can Black 2 bring up Wi-Fi through its
 * own DWC auto-connect, complete the client's pinned Mbed TLS handshake and
 * ping the Wonder Trade service, and how much memory is free while it plays?
 *
 * WtSpike_Boot runs once after the game's heaps exist and before its first
 * overlays load. It reports the main arena and root heaps, then loads the
 * Wi-Fi overlays (11 and 29) the way the game's own Wi-Fi device does, runs
 * DWC_AC, opens a socket, performs the TLS handshake on a worker thread and
 * sends GET /v1/ping. WtSpike_Frame samples the root heap every frame and
 * reports the lowest free memory seen while the field or a battle runs.
 *
 * Results go to the NO$GBA debug string port, which melonDS prints. The TLS
 * seed below is not secret: never ship this file in a player build. */

#include "types.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "dwc/ac.h"
#include "dwc/error.h"
#include "dwc/memfunc.h"
#include "gfl/heap.h"
#include "gfl/heapsys.h"
#include "gfl/overlay.h"
#include "nitro/os.h"
#include "nitrowifi/soc.h"
#include "nnsys/fnd.h"

#include <mbedtls/memory_buffer_alloc.h>
#include <mbedtls/platform.h>
#include <psa/crypto.h>

#include "wonder_trade/http.h"
#include "wonder_trade/tls.h"
#include "wonder_trade_config.h"

#define SPIKE_DWC_HEAP_SIZE 0x20000
#define SPIKE_STACK_SIZE 0x8000
#define SPIKE_ACK "DSWT_ACK_V1\n"

void WtSpike_Boot(void);
void WtSpike_Frame(void);
int snprintf(char *buffer, size_t size, const char *format, ...);

typedef struct SpikeTransport {
    OSThread thread;
    WtTls tls;
    WtHttpExchange http;
    unsigned char arena[WT_TLS_HEAP_SIZE];
    unsigned char stack[SPIKE_STACK_SIZE + 8];
    int socket;
    volatile int done;
    volatile int result;
    volatile int step;
    int lastRecv, recvCalls, lastRead;
    char plain[200];
    u32 plainSize;
    u32 recvBytes;
    u32 connectMs, handshakeMs, exchangeMs;
    size_t peak;
} SpikeTransport;

static NNSFndHeapHandle sDwcHeap;
static u32 sDwcMinFree;
static char sLine[256];

int snprintf(char *buffer, size_t size, const char *format, ...)
{
    va_list args;
    int result;
    va_start(args, format);
    result = vsnprintf(buffer, size, format, args);
    va_end(args);
    return result;
}

static void Print(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(sLine, sizeof(sLine) - 1, format, args);
    va_end(args);
    strcat(sLine, "\n");
    /* NO$GBA "String Out (raw)": melonDS logs the string at this address. */
    *(volatile u32 *)0x04FFFA10 = (u32)sLine;
}

static u32 Ms(u64 ticks)
{
    return (u32)OS_TicksToMilliSeconds(ticks);
}

static u32 HeapLargest(HeapID id)
{
    return GFL_HeapGetValidHeapBase(id) != NULL ? GFL_HeapGetHighestAllocatableSize(id) : 0;
}

static u32 HeapFree(HeapID id)
{
    return GFL_HeapGetValidHeapBase(id) != NULL ? GFL_HeapGetFreeSizeCore(id) : 0;
}

static void *DwcAlloc(DWCAllocType name, u32 size, int align)
{
    OSIntrMode interrupts = OS_DisableInterrupts();
    void *block = NNS_FndAllocFromExpHeapEx(sDwcHeap, size, align);
    u32 freeSize = NNS_FndGetTotalFreeSizeForExpHeap(sDwcHeap);
    (void)name;
    if (freeSize < sDwcMinFree) sDwcMinFree = freeSize;
    OS_RestoreInterrupts(interrupts);
    return block;
}

static void DwcFree(DWCAllocType name, void *block, u32 size)
{
    (void)name; (void)size;
    if (block != NULL) {
        OSIntrMode interrupts = OS_DisableInterrupts();
        NNS_FndFreeToExpHeap(sDwcHeap, block);
        OS_RestoreInterrupts(interrupts);
    }
}

static void *AcAlloc(u32 name, s32 size)
{
    return DWC_Alloc((DWCAllocType)name, (u32)size);
}

static void AcFree(u32 name, void *block, s32 size)
{
    DWC_Free((DWCAllocType)name, block, (u32)size);
}

/* Debug only: a predictable seed. A player build reads its journal seed. */
static int SeedRead(unsigned char *seed, size_t size)
{
    u32 tick = (u32)OS_GetTick();
    size_t i;
    for (i = 0; i < size; i++) seed[i] = (unsigned char)((tick >> ((i % 4) * 8)) ^ (i * 151u) ^ 0x5a);
    return (int)size;
}

static int SeedWrite(unsigned char *seed, size_t size)
{
    (void)seed;
    return (int)size;
}

static int Send(void *arg, const unsigned char *data, size_t size)
{
    SpikeTransport *transport = arg;
    int result = SOC_Send(transport->socket, data, (int)size, 0);
    if (result == SOC_EWOULDBLOCK || result == SOC_EINPROGRESS) return MBEDTLS_ERR_SSL_WANT_WRITE;
    return result < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : result;
}

static int Receive(void *arg, unsigned char *data, size_t size)
{
    SpikeTransport *transport = arg;
    int result = SOC_Recv(transport->socket, data, (int)size, 0);
    transport->lastRecv = result;
    transport->recvCalls++;
    if (result > 0) transport->recvBytes += result;
    if (result == SOC_EWOULDBLOCK || result == SOC_EINPROGRESS) return MBEDTLS_ERR_SSL_WANT_READ;
    return result < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : result;
}

static int HttpWrite(void *arg, const unsigned char *data, size_t size)
{
    return WtTls_Write(&((SpikeTransport *)arg)->tls, data, size);
}

static int HttpRead(void *arg, unsigned char *data, size_t size)
{
    SpikeTransport *transport = arg;
    int result = WtTls_Read(&transport->tls, data, size), i;
    transport->lastRead = result;
    for (i = 0; i < result && transport->plainSize < sizeof(transport->plain) - 1; i++) {
        char c = (char)data[i];
        transport->plain[transport->plainSize++] = c == '\r' ? '<' : c == '\n' ? '>' : c < 32 || c > 126 ? '?' : c;
    }
    return result;
}

static int HttpWait(void *arg, int result)
{
    (void)arg;
    if (result != MBEDTLS_ERR_SSL_WANT_READ && result != MBEDTLS_ERR_SSL_WANT_WRITE) return 0;
    OS_Sleep(1);
    return 1;
}

static int Exchange(SpikeTransport *transport)
{
    static const unsigned char pins[2][32] = WONDER_TRADE_PINS;
    WtHttpIO io = { transport, HttpWrite, HttpRead, HttpWait };
    SOCSockAddrIn address;
    u64 start = OS_GetTick();
    int result;

    transport->step = 1;
    transport->socket = SOC_Socket(SOC_AF_INET, SOC_SOCK_STREAM, 0);
    if (transport->socket < 0) return -100 + transport->socket;
    memset(&address, 0, sizeof(address));
    address.len = sizeof(address);
    address.family = SOC_AF_INET;
    address.port = SOC_HtoNs(WONDER_TRADE_PORT);
    address.addr.addr = SOC_HtoNl(WONDER_TRADE_IPV4);
    transport->step = 2;
    result = SOC_Connect(transport->socket, &address);
    if (result != 0) return -200 + result;
    transport->connectMs = Ms(OS_GetTick() - start);
    transport->step = 3;
    result = WtTls_Open(&transport->tls, WONDER_TRADE_HOSTNAME, pins, transport, Send, Receive);
    if (result != 0) return result;
    start = OS_GetTick();
    transport->step = 4;
    do {
        result = WtTls_Handshake(&transport->tls);
    } while (HttpWait(transport, result));
    if (result != 0) return result;
    transport->handshakeMs = Ms(OS_GetTick() - start);
    start = OS_GetTick();
    transport->step = 5;
    if (WtHttp_Exchange(&transport->http, io, WONDER_TRADE_AUTHORITY, NULL, 0) != 0) return -12;
    transport->exchangeMs = Ms(OS_GetTick() - start);
    transport->step = 6;
    if (transport->http.bodySize != sizeof(SPIKE_ACK) - 1
        || memcmp(transport->http.body, SPIKE_ACK, sizeof(SPIKE_ACK) - 1) != 0) return -14;
    return 0;
}

/* The client's ping parser wants application/octet-stream, which only /wt/v1
 * replies carry; the gateway answers GET /v1/ping as text/plain. Accept the
 * ping on its decrypted status line and body instead. */
static int PingAnswered(const SpikeTransport *transport)
{
    static const char status[] = "HTTP/1.1 200 OK<>", body[] = "<><>DSWT_ACK_V1>";
    u32 size = transport->plainSize;
    return size > sizeof(status) + sizeof(body) && memcmp(transport->plain, status, sizeof(status) - 1) == 0
        && memcmp(transport->plain + size - (sizeof(body) - 1), body, sizeof(body) - 1) == 0;
}

static void Worker(void *arg)
{
    SpikeTransport *transport = arg;
    size_t blocks;
    transport->socket = -1;
    mbedtls_memory_buffer_alloc_init(transport->arena, sizeof(transport->arena));
    mbedtls_platform_set_nv_seed(SeedRead, SeedWrite);
    transport->result = Exchange(transport);
    WtTls_Close(&transport->tls);
    if (transport->socket >= 0) {
        SOC_Shutdown(transport->socket, SOC_SHUT_RDWR);
        SOC_Close(transport->socket);
    }
    mbedtls_memory_buffer_alloc_max_get(&transport->peak, &blocks);
    mbedtls_memory_buffer_alloc_free();
    transport->done = 1;
}

static void ReportHeaps(const char *when)
{
    Print("WTSPIKE %s arena lo=%08x hi=%08x slack=%u heap0 free=%u heap1 free=%u largest=%u heap9 free=%u",
        when, (u32)OS_GetArenaLo(OS_ARENA_MAIN), (u32)OS_GetArenaHi(OS_ARENA_MAIN),
        (u32)OS_GetArenaHi(OS_ARENA_MAIN) - (u32)OS_GetArenaLo(OS_ARENA_MAIN),
        HeapFree(HEAPID_SYSTEM), HeapFree(HEAPID_USER), HeapLargest(HEAPID_USER), HeapFree(HEAPID_DEVICE_ALLOC));
}

/* Gives up after 60 seconds; the game then boots as usual. */
static void RunNetwork(void)
{
    void *dwcMemory, *transportMemory;
    SpikeTransport *transport;
    DWCACConfig config;
    u64 start, deadline;
    s32 status = 0;
    int created = 0;

    dwcMemory = GFL_HeapAllocateCore(HEAPID_USER, SPIKE_DWC_HEAP_SIZE + 32);
    transportMemory = GFL_HeapAllocateCore(HEAPID_USER, sizeof(SpikeTransport) + 32);
    Print("WTSPIKE network memory dwc=%u transport=%u (arena %u, stack %u) heap1 largest after=%u",
        SPIKE_DWC_HEAP_SIZE + 32, (u32)sizeof(SpikeTransport) + 32, WT_TLS_HEAP_SIZE, SPIKE_STACK_SIZE,
        HeapLargest(HEAPID_USER));
    if (dwcMemory == NULL || transportMemory == NULL) {
        Print("WTSPIKE FAIL no heap memory");
        return;
    }
    sDwcHeap = NNS_FndCreateExpHeapEx((void *)(((u32)dwcMemory + 31) & ~31), SPIKE_DWC_HEAP_SIZE, 0);
    sDwcMinFree = NNS_FndGetTotalFreeSizeForExpHeap(sDwcHeap);
    transport = (SpikeTransport *)(((u32)transportMemory + 31) & ~31);
    memset(transport, 0, sizeof(*transport));

    GFL_OvlLoad(OVERLAY_ID(11));
    GFL_OvlLoad(OVERLAY_ID(29));
    DWC_SetMemFunc(DwcAlloc, DwcFree);
    DWC_ClearError();
    memset(&config, 0, sizeof(config));
    config.alloc = AcAlloc;
    config.free = AcFree;
    config.dmaNo = 2;
    config.powerMode = 1;
    config.skipConnectionTest = 1;
    start = OS_GetTick();
    deadline = start + OS_MilliSecondsToTicks(60000);
    if (!DWC_AC_Create(&config)) {
        Print("WTSPIKE FAIL DWC_AC_Create");
    } else {
        created = 1;
        while (OS_GetTick() < deadline) {
            DWC_AC_Process();
            status = DWC_AC_GetStatus();
            if (status == 5 || status < 0) break;
            OS_WaitVBlankIntr();
        }
        Print("WTSPIKE DWC_AC status=%d after %u ms, address %08x", status, Ms(OS_GetTick() - start),
            (u32)SOC_GetHostID());
    }
    if (status == 5) {
        u32 frames = 0;
        void *stackTop = (void *)(((u32)transport->stack + SPIKE_STACK_SIZE) & ~7);
        OS_CreateThread(&transport->thread, Worker, transport, stackTop, SPIKE_STACK_SIZE - 8,
            OS_GetThreadPriority(OS_GetCurrentThread()) + 1);
        memset((void *)(transport->thread.stackTop + sizeof(u32)), 0xa5,
            transport->thread.stackBottom - transport->thread.stackTop - 2 * sizeof(u32));
        start = OS_GetTick();
        OS_WakeupThreadDirect(&transport->thread);
        while (!transport->done && OS_GetTick() < deadline) {
            DWC_AC_Process();
            OS_WaitVBlankIntr();
            frames++;
        }
        if (!transport->done) {
            Print("WTSPIKE FAIL timeout at step %d", transport->step);
            if (transport->socket >= 0) SOC_Close(transport->socket);
            while (!transport->done) OS_WaitVBlankIntr();
        }
        OS_JoinThread(&transport->thread);
        {
            const unsigned char *stack = (const unsigned char *)(transport->thread.stackTop + sizeof(u32));
            u32 size = transport->thread.stackBottom - transport->thread.stackTop - 2 * sizeof(u32), unused = 0;
            while (unused < size && stack[unused] == 0xa5) unused++;
            Print("WTSPIKE worker %s result=%d step=%d connect=%u ms handshake=%u ms ping=%u ms tls peak=%u stack=%u frames=%u",
                transport->result == 0 ? "ok" : "error", transport->result, transport->step, transport->connectMs,
                transport->handshakeMs, transport->exchangeMs, (u32)transport->peak, size - unused, frames);
        }
        Print("WTSPIKE socket reads=%d bytes=%u last=%d tls read=%d http body=%u", transport->recvCalls,
            transport->recvBytes, transport->lastRecv, transport->lastRead, (u32)transport->http.bodySize);
        Print("WTSPIKE response: %s", transport->plain);
        Print("WTSPIKE %s: TLS handshake %s, GET /v1/ping answered %s; DWC heap peak use=%u of %u",
            transport->handshakeMs && PingAnswered(transport) ? "PASS" : "FAIL",
            transport->handshakeMs ? "completed" : "failed", PingAnswered(transport) ? "200 DSWT_ACK_V1" : "no",
            SPIKE_DWC_HEAP_SIZE - sDwcMinFree, SPIKE_DWC_HEAP_SIZE);
        if (transport->result == 0) {
            Print("WTSPIKE server said %s", "DSWT_ACK_V1");
        }
    }
    if (created) {
        while (!DWC_AC_Destroy()) OS_WaitVBlankIntr();
    }
    DWC_ClearError();
    DWC_SetMemFunc(NULL, NULL);
    GFL_OvlUnload(OVERLAY_ID(29));
    GFL_OvlUnload(OVERLAY_ID(11));
    NNS_FndDestroyExpHeap(sDwcHeap);
    GFL_HeapFreeCore(transportMemory);
    GFL_HeapFreeCore(dwcMemory);
    ReportHeaps("after-network");
}

void WtSpike_Boot(void)
{
    ReportHeaps("boot");
    RunNetwork();
}

/* The lowest root heap free and largest block seen in each kind of scene. */
enum { SCENE_OTHER, SCENE_FIELD, SCENE_BATTLE, SCENE_COUNT };
static const char *const sSceneNames[SCENE_COUNT] = { "other", "field", "battle" };
static u32 sMinFree[SCENE_COUNT], sMinLargest[SCENE_COUNT];
static u32 sReported[SCENE_COUNT];
static u32 sFrame;

void WtSpike_Frame(void)
{
    int scene = GFL_HeapGetValidHeapBase(HEAPID_BATTLE) != NULL ? SCENE_BATTLE
        : GFL_HeapGetValidHeapBase(HEAPID_FIELDMAP) != NULL ? SCENE_FIELD : SCENE_OTHER;
    u32 freeSize = HeapFree(HEAPID_USER), largest = HeapLargest(HEAPID_USER);
    if (sMinFree[scene] == 0 || freeSize < sMinFree[scene]) sMinFree[scene] = freeSize;
    if (sMinLargest[scene] == 0 || largest < sMinLargest[scene]) sMinLargest[scene] = largest;
    if (++sFrame % 1200 == 0) {
        Print("WTSPIKE heap1 now %s free=%u largest=%u (frame %u)", sSceneNames[scene], freeSize, largest, sFrame);
    }
    if (sFrame % 300 == 0) {
        int i;
        for (i = 0; i < SCENE_COUNT; i++) {
            if (sMinLargest[i] && sMinLargest[i] != sReported[i]) {
                sReported[i] = sMinLargest[i];
                Print("WTSPIKE heap1 %s min free=%u min largest=%u (now %s free=%u largest=%u, frame %u)",
                    sSceneNames[i], sMinFree[i], sMinLargest[i], sSceneNames[scene], freeSize, largest, sFrame);
            }
        }
    }
}
