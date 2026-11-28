#include "platform/nds/worker.h"

#include <string.h>

#include <nds.h>
#include <mbedtls/memory_buffer_alloc.h>
#include <mbedtls/platform.h>
#include <mbedtls/platform_util.h>
#include <psa/crypto.h>

#include "platform/entropy_pool.h"
#include "platform/nds/net.h"
#include "wonder_trade/tls.h"

/* Twice the game's network worker: besides a visit (the game's whole
 * worker), jobs here detect and provision journals (save/journal_save.h asks
 * for 32 KiB of its own), make backups and run the boot check. */
#define CP_WORKER_STACK_SIZE 0x10000

static struct {
    cothread_t thread;
    int started, finished, result;
    volatile int cancel;
    size_t peak, leaked;
    CpWorkerJob job;
    void *arg;
    CpSeed seed;
    unsigned char volatileSeed[WT_SEED_SIZE];
} sWorker;
static unsigned char sArena[WT_TLS_HEAP_SIZE] __attribute__((aligned(32)));
static unsigned char sStack[CP_WORKER_STACK_SIZE] __attribute__((aligned(8)));

static int ReadSeed(unsigned char *seed, size_t size)
{
    int result = -1;
    if (size != WT_SEED_SIZE) return -1;
    if (sWorker.seed.kind == CP_SEED_JOURNAL) {
        result = sWorker.seed.journal ? WtSeed_Read(sWorker.seed.journal, seed, size) : -1;
    } else {
        memcpy(seed, sWorker.volatileSeed, size);
        result = (int)size;
    }
    /* A copied or replayed save must not repeat its random stream. */
    if (result > 0 && CpEntropy_Mix(seed) != 0) result = -1;
    if (result < 0) mbedtls_platform_zeroize(seed, size);
    return result;
}

static int WriteSeed(unsigned char *seed, size_t size)
{
    if (size != WT_SEED_SIZE) return -1;
    if (sWorker.seed.kind == CP_SEED_JOURNAL)
        return sWorker.seed.journal ? WtSeed_Write(sWorker.seed.journal, seed, size) : -1;
    memcpy(sWorker.volatileSeed, seed, size);
    return (int)size;
}

static int Run(void *arg)
{
    size_t blocks;
    (void)arg;
    mbedtls_memory_buffer_alloc_init(sArena, sizeof(sArena));
    mbedtls_platform_set_nv_seed(ReadSeed, WriteSeed);
    sWorker.result = sWorker.job(sWorker.arg);
    CpNet_EndVisit();
    mbedtls_psa_crypto_free();
    mbedtls_memory_buffer_alloc_max_get(&sWorker.peak, &blocks);
    mbedtls_memory_buffer_alloc_cur_get(&sWorker.leaked, &blocks);
    mbedtls_memory_buffer_alloc_free();
    mbedtls_platform_zeroize(sArena, sizeof(sArena));
    sWorker.finished = 1;
    return 0;
}

int CpWorker_Start(CpWorkerJob job, void *arg, CpSeed seed)
{
    if (sWorker.started || !job) return -1;
    if (seed.kind == CP_SEED_JOURNAL && !seed.journal) return -1;
    sWorker.job = job;
    sWorker.arg = arg;
    sWorker.seed = seed;
    sWorker.finished = sWorker.cancel = 0;
    sWorker.result = -1;
    sWorker.peak = sWorker.leaked = 0;
    sWorker.thread = cothread_create_manual(Run, NULL, sStack, sizeof(sStack), 0);
    if (sWorker.thread < 0) return -1;
    sWorker.started = 1;
    return 0;
}

int CpWorker_Busy(void)
{
    return sWorker.started;
}

void CpWorker_Cancel(void)
{
    sWorker.cancel = 1;
}

const volatile int *CpWorker_Cancelled(void)
{
    return &sWorker.cancel;
}

int CpWorker_Collect(int *result, size_t *peak, size_t *leaked)
{
    if (!sWorker.started || !sWorker.finished || !cothread_has_joined(sWorker.thread)) return 0;
    cothread_delete(sWorker.thread);
    sWorker.started = 0;
    if (result) *result = sWorker.result;
    if (peak) *peak = sWorker.peak;
    if (leaked) *leaked = sWorker.leaked;
    mbedtls_platform_zeroize(sStack, sizeof(sStack));
    return 1;
}
