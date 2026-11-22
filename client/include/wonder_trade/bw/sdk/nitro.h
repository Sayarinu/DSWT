#ifndef WONDER_TRADE_BW_SDK_NITRO_H
#define WONDER_TRADE_BW_SDK_NITRO_H

/* The TwlSDK names the shared core and the Black/White adapter use. Types come
 * from aletheia's SDK headers (src/game); the functions are TwlSDK's, at the
 * addresses aletheia's link gives them, declared here with the SDK's own
 * signatures. Black and White build with TwlSDK 5.3. */

#include "types.h"
#include "sdk_os.h"
#include "sdk_card.h"

#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
#ifndef NULL
#define NULL ((void *)0)
#endif

#define OS_SYSTEM_CLOCK 33514000
#define OS_TicksToMilliSeconds(tick) (((tick) * 64) / (OS_SYSTEM_CLOCK / 1000))
#define OS_SecondsToTicks(seconds) ((OSTick)(((u64)OS_SYSTEM_CLOCK * (u64)(seconds)) / 64))
/* OS_GetLowEntropyData fills eight words. */
#define OS_LOW_ENTROPY_DATA_SIZE 32
#define OS_LOCK_ID_ERROR (-3)
#define HW_RESET_PARAMETER_BUF 0x02fffc20
#define OSi_STACK_CHECKNUM_BOTTOM 0xfddb597d
#define OSi_STACK_CHECKNUM_TOP 0x7bf9dd5b

OSTick OS_GetTick(void);
OSIntrMode OS_DisableInterrupts(void);
OSIntrMode OS_RestoreInterrupts(OSIntrMode state);
s32 OS_GetLockID(void);
void OS_ReleaseLockID(u16 lockID);
u32 OS_GetConsoleType(void);
void OS_GetMacAddress(u8 *macAddress);
void OS_GetLowEntropyData(u32 buffer[8]);
void OS_CreateThread(OSThread *thread, void (*func)(void *), void *arg, void *stack, u32 stackSize, u32 prio);
void OS_WakeupThreadDirect(OSThread *thread);
void OS_JoinThread(OSThread *thread);
BOOL OS_IsThreadTerminated(const OSThread *thread);
u32 OS_GetThreadPriority(const OSThread *thread);
void OS_Sleep(u32 msec);
void OS_ResetSystem(u32 parameter);
void DC_FlushRange(const void *startAddr, u32 nBytes);

extern OSThreadInfo OSi_ThreadInfo;
#define OS_GetCurrentThread() (OSi_ThreadInfo.current)

/* Thread stack guard check, implemented by the adapter (src/wonder_trade/bw/sdk.c). */
typedef enum {
    OS_STACK_NO_ERROR = 0,
    OS_STACK_OVERFLOW,
    OS_STACK_ABOUT_TO_OVERFLOW,
    OS_STACK_UNDERFLOW
} OSStackStatus;
OSStackStatus OS_GetStackStatus(const OSThread *thread);

/* The reset parameter the previous program left (HW_RESET_PARAMETER_BUF). */
u32 OS_GetResetParameter(void);

void MI_CpuFill8(void *dest, u8 data, u32 size);
void MIi_CpuClear16(u16 data, void *destp, u32 size);
void MIi_CpuClear32(u32 data, void *destp, u32 size);
void MIi_CpuCopy16(const void *srcp, void *destp, u32 size);
#define MI_CpuCopy16(src, dest, size) MIi_CpuCopy16((src), (dest), (size))

/* A backup type: the device, the log2 of its size and the vendor. */
#define CARD_BACKUP_TYPE_DEFINE(device, sizeShift, vendor) ((device) | ((sizeShift) << 8) | ((vendor) << 16))
#define CARD_BACKUP_TYPE_FLASH_4MBITS_EX CARD_BACKUP_TYPE_DEFINE(2, 19, 0xff)
#define CARD_BACKUP_TYPE_FLASH_4MBITS CARD_BACKUP_TYPE_FLASH_4MBITS_EX
#define CARD_REQ_READ_BACKUP 6
#define CARD_REQ_WRITE_BACKUP 7
#define CARD_REQUEST_MODE_RECV 0
#define CARD_REQUEST_MODE_SEND_VERIFY 2
#define CARD_RETRY_COUNT_MAX 10

void CARD_LockBackup(u16 lockID);
void CARD_UnlockBackup(u16 lockID);
BOOL CARDi_RequestStreamCommand(u32 src, u32 dst, u32 len, void (*callback)(void *), void *arg, BOOL is_async,
                                int req_type, int req_retry, int req_mode);

/* The backup type CARD_IdentifyBackup selected. */
CARDBackupType CARD_GetCurrentBackupType(void);

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
