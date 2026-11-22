#include <nitro.h>
#include <string.h>
#include <mbedtls/platform_util.h>

#include "wonder_trade/bw/game.h"
#include "wonder_trade/boot.h"
#include "wonder_trade/bw/application.h"
#include "wonder_trade/bw/game_save.h"
#include "wonder_trade/flash.h"
#include "wonder_trade/recovery.h"

/* The first process of the game (client/aletheia.patch queues it once from OVERLAY_LAUNCH_Driver, arm9/app_power.c): the
 * journal is checked against the save before the title screen, on a worker
 * thread. Retail writes stay refused until it succeeds; when it does not, the
 * game stops here. A check that takes longer than WT_BOOT_SILENT_FRAMES, as
 * after an interrupted trade, says so on screen. */

#define WT_BOOT_STACK_SIZE 0x8000

typedef struct WtBootWorker {
    OSThread thread;
    WtFlashAccess flash;
    WtGame game;
    int result;
    unsigned char stack[WT_BOOT_STACK_SIZE + 8];
} WtBootWorker;

static WtBootWorker *sWorker;
static WtApp *sApp;
static int sFrames;

static void WtBoot_Worker(void *arg)
{
    WtBootWorker *worker = arg;
    WtJournalIO io = { &worker->flash, WtFlash_ReadBank, WtFlash_WriteBank, WtFlash_Hash };
    /* Banks holding only a musical's data hold no journal: nothing to recover.
     * Opening Wonder Trade erases them before creating one. */
    if (WtGameSave_MusicalLeftover()) {
        WtRecovery_Allow(WT_JOURNAL_EMPTY);
        worker->result = 0;
        return;
    }
    worker->result = WtBoot_Check(&worker->game, io);
}

static u32 WtBoot_Init(void *proc, u32 *state, void *param, void *work)
{
    WtBootWorker *worker;
    void *stackTop;
    (void)proc; (void)state; (void)param; (void)work;
    WtRecovery_Begin();
    worker = GFL_HEAP_AllocMemory(WT_HEAP_APP, sizeof(*worker));
    if (!worker) {
        WtRecovery_Block();
        return TRUE;
    }
    memset(worker, 0, sizeof(*worker));
    worker->game.save = SAVE_CONTROL_Get();
    worker->game.heapId = WT_HEAP_APP;
    stackTop = (void *)(((u32)worker->stack + WT_BOOT_STACK_SIZE) & ~7);
    OS_CreateThread(&worker->thread, WtBoot_Worker, worker, stackTop, WT_BOOT_STACK_SIZE - 8,
        OS_GetThreadPriority(OS_GetCurrentThread()) + 1);
    memset((void *)(worker->thread.stackTop + sizeof(u32)), 0xa5,
        worker->thread.stackBottom - worker->thread.stackTop - WT_STACK_GUARD_BYTES);
    sWorker = worker;
    OS_WakeupThreadDirect(&worker->thread);
    return TRUE;
}

static u32 WtBoot_Tick(void *proc, u32 *state, void *param, void *work)
{
    WtBootWorker *worker = sWorker;
    const unsigned char *stack;
    u32 size, unused;
    (void)proc; (void)param; (void)work;
    if (worker) {
        if (!OS_IsThreadTerminated(&worker->thread)) {
            if (!sApp && ++sFrames == WT_BOOT_SILENT_FRAMES) sApp = WtApp_CreateChecking();
            return FALSE;
        }
        OS_JoinThread(&worker->thread);
        stack = (const unsigned char *)(worker->thread.stackTop + sizeof(u32));
        size = worker->thread.stackBottom - worker->thread.stackTop - WT_STACK_GUARD_BYTES;
        for (unused = 0; unused < size && stack[unused] == 0xa5; unused++) { (void)0; }
        if (unused < 256 || OS_GetStackStatus(&worker->thread) != OS_STACK_NO_ERROR || worker->result != 0)
            WtRecovery_Block();
        mbedtls_platform_zeroize(worker, sizeof(*worker));
        GFL_HEAP_FreeMemory(worker);
        sWorker = NULL;
        if (sApp) WtApp_Free(sApp);
        sApp = NULL;
    }
    (void)state;
    if (WtRecovery_CanWrite()) return TRUE;
    /* A blocked save keeps the game here, on Wonder Trade's recovery screen,
     * until a connection resolves it. */
    if (!sApp) sApp = WtApp_Create(TRUE);
    if (sApp && WtApp_Update(sApp)) {
        WtApp_Free(sApp);
        sApp = NULL;
    }
    return FALSE;
}

static u32 WtBoot_End(void *proc, u32 *state, void *param, void *work)
{
    (void)proc; (void)state; (void)param; (void)work;
    return TRUE;
}

const WtProcData gWtBootProcData = { WtBoot_Init, WtBoot_Tick, WtBoot_End };
