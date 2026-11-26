#ifndef WONDER_TRADE_BW2_SDK_NITRO_H
#define WONDER_TRADE_BW2_SDK_NITRO_H

/* The Nitro/TWL SDK names the shared core uses, from pokebw2's decompiled
 * SDK headers. Black 2 and White 2 build with TwlSDK 5.3; the few names its
 * headers lack are declared from the SDK's documented behavior. */

#include "types.h"
#include "nitro/card.h"
#include "nitro/mi.h"
#include "nitro/os.h"

/* OS_GetLowEntropyData fills eight words. */
#define OS_LOW_ENTROPY_DATA_SIZE 32

#define OS_SecondsToTicks(seconds) ((OSTick)(((u64)OS_SYSTEM_CLOCK * (u64)(seconds)) / 64))

/* Thread stack guard check, implemented by the adapter (src/wonder_trade/bw2/sdk.c). */
typedef enum {
    OS_STACK_NO_ERROR = 0,
    OS_STACK_OVERFLOW,
    OS_STACK_ABOUT_TO_OVERFLOW,
    OS_STACK_UNDERFLOW
} OSStackStatus;
OSStackStatus OS_GetStackStatus(const OSThread *thread);

/* The reset parameter the previous program left (HW_RESET_PARAMETER_BUF). */
u32 OS_GetResetParameter(void);

/* The backup type CARD_IdentifyBackup selected. */
CARDBackupType CARD_GetCurrentBackupType(void);

#define CARD_BACKUP_TYPE_FLASH_4MBITS CARD_BACKUP_TYPE_FLASH_4MBITS_EX

/* Synchronous flash access on the already locked backup device. */
static inline BOOL CARD_ReadFlash(u32 source, void *destination, u32 length)
{
    return CARDi_RequestStreamCommand(source, (u32)destination, length, NULL, NULL, FALSE, CARD_REQ_READ_BACKUP, 1,
                                      CARD_REQUEST_MODE_RECV);
}

static inline BOOL CARD_WriteAndVerifyFlash(u32 destination, const void *source, u32 length)
{
    return CARDi_RequestStreamCommand((u32)source, destination, length, NULL, NULL, FALSE, CARD_REQ_WRITE_BACKUP,
                                      CARD_RETRY_COUNT_MAX, CARD_REQUEST_MODE_SEND_VERIFY);
}

#endif
