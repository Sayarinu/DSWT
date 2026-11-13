#include "wonder_trade/entropy_game.h"
#include "wonder_trade/entropy.h"

#include <nitro.h>
#include <string.h>
#include <mbedtls/platform_util.h>
#include "wonder_trade/game_adapter.h"

static WtEntropyPool sPool;

void WonderTradeEntropy_Init(void)
{
    unsigned char boot[64];
    u32 low[OS_LOW_ENTROPY_DATA_SIZE / sizeof(u32)];
    u64 tick = OS_GetTick();
    u32 reset = OS_GetResetParameter(), console = OS_GetConsoleType();
    memset(boot, 0, sizeof(boot));
    OS_GetLowEntropyData(low);
    memcpy(boot, low, sizeof(low));
    OS_GetMacAddress(boot + 32);
    memcpy(boot + 38, &tick, sizeof(tick));
    memcpy(boot + 46, &reset, sizeof(reset));
    memcpy(boot + 50, &console, sizeof(console));
    WtEntropy_Init(&sPool, boot);
    mbedtls_platform_zeroize(boot, sizeof(boot));
    mbedtls_platform_zeroize(low, sizeof(low));
}

void WonderTradeEntropy_Frame(void)
{
    WtEntropyPool next = sPool;
    WtEntropySample sample;
    u32 low[OS_LOW_ENTROPY_DATA_SIZE / sizeof(u32)];
    OSIntrMode interrupts;
    memset(&sample, 0, sizeof(sample));
    OS_GetLowEntropyData(low);
    memcpy(sample.low, low, sizeof(low));
    WtGame_SampleInput(&sample);
    WtEntropy_Frame(&next, &sample);
    /* Only the main loop writes; workers copy a coherent pool with IRQs masked.
     * Hash outside the short publication/snapshot critical sections. */
    interrupts = OS_DisableInterrupts();
    memcpy(&sPool, &next, sizeof(sPool));
    OS_RestoreInterrupts(interrupts);
    mbedtls_platform_zeroize(&next, sizeof(next));
    mbedtls_platform_zeroize(&sample, sizeof(sample));
    mbedtls_platform_zeroize(low, sizeof(low));
}

static void Snapshot(WtEntropyPool *snapshot)
{
    OSIntrMode interrupts = OS_DisableInterrupts();
    memcpy(snapshot, &sPool, sizeof(*snapshot));
    OS_RestoreInterrupts(interrupts);
}

int WonderTradeEntropy_Seed(unsigned char seed[32])
{
    WtEntropyPool snapshot;
    int result;
    Snapshot(&snapshot);
    result = WtEntropy_Seed(&snapshot, seed);
    mbedtls_platform_zeroize(&snapshot, sizeof(snapshot));
    return result;
}

int WonderTradeEntropy_Mix(unsigned char *seed, size_t size)
{
    WtEntropyPool snapshot;
    int result;
    if (!seed || size != 32) return -1;
    Snapshot(&snapshot);
    result = WtEntropy_Mix(&snapshot, seed, seed);
    mbedtls_platform_zeroize(&snapshot, sizeof(snapshot));
    return result;
}
