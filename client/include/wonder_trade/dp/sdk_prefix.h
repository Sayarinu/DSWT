#ifndef WONDER_TRADE_DP_SDK_PREFIX_H
#define WONDER_TRADE_DP_SDK_PREFIX_H

/* Included first in every Wonder Trade unit built for Diamond and Pearl.
 * pokediamond's Nitro SDK 3.2 headers lack a few names the shared core uses;
 * these are written from the SDK's documented behavior. */

#include <nitro.h>
#include "CARD_backup.h"
#include "OS_ownerInfo.h"

/* OS_GetLowEntropyData fills eight words (lib/NitroSDK/src/OS_entropy.c). */
#define OS_LOW_ENTROPY_DATA_SIZE 32

/* Tick conversions matching OS_MilliSecondsToTicks: the tick timer runs at
 * the system clock divided by 64. */
#define OS_SecondsToTicks(seconds) ((OSTick)(((u64)OS_SYSTEM_CLOCK * (u64)(seconds)) / 64))
#define OS_TicksToMilliSeconds(ticks) (((u64)(ticks) * 64) / (OS_SYSTEM_CLOCK / 1000))

/* Thread stack guard check, implemented by the Diamond/Pearl adapter
 * (src/wonder_trade/dp/sdk.c) because the ROM does not link one. */
typedef enum {
    OS_STACK_NO_ERROR = 0,
    OS_STACK_OVERFLOW,
    OS_STACK_ABOUT_TO_OVERFLOW,
    OS_STACK_UNDERFLOW
} OSStackStatus;
OSStackStatus OS_GetStackStatus(const OSThread *thread);

/* The backup device type the last identification selected; also
 * implemented by the adapter. */
CARDBackupType CARD_GetCurrentBackupType(void);

/* Synchronous flash access on the already locked backup device. */
static inline BOOL CARD_ReadFlash(u32 source, void *destination, u32 length)
{
    return CARDi_ReadBackup(source, destination, length, NULL, NULL, FALSE);
}

static inline BOOL CARD_WriteAndVerifyFlash(u32 destination, const void *source, u32 length)
{
    return CARDi_WriteAndVerifyBackup(destination, (void *)source, length, NULL, NULL, FALSE);
}

#endif
