#include <nitro.h>
#include <nitroWiFi/socl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "arm9/card_common.h"

extern CARDiCommon cardi_common;

/* SDK and runtime functions the Wonder Trade code needs that Black and White
 * never link. They follow TwlSDK's documented behavior on aletheia's own
 * structures. */

OSStackStatus OS_GetStackStatus(const OSThread *thread)
{
    /* os_thread.c writes the bottom guard 8 bytes before the stack bottom. */
    if (*(const u32 *)(thread->stackBottom - 8) != OSi_STACK_CHECKNUM_BOTTOM) return OS_STACK_UNDERFLOW;
    if (*(const u32 *)thread->stackTop != OSi_STACK_CHECKNUM_TOP) return OS_STACK_OVERFLOW;
    return OS_STACK_NO_ERROR;
}

u32 OS_GetResetParameter(void)
{
    return *(const u32 *)HW_RESET_PARAMETER_BUF;
}

/* CARD_IdentifyBackup records the selected device in the command block. */
CARDBackupType CARD_GetCurrentBackupType(void)
{
    return cardi_common.cmd->type;
}

int WtSocl_TcpSocket(void)
{
    return SOC_Socket(SOC_AF_INET, SOC_SOCK_STREAM, 0);
}

int WtSocl_SetNoBlock(int socket)
{
    return SOC_Fcntl(socket, SOC_F_SETFL, SOC_O_NONBLOCK);
}

/* The ROM's C library has vsnprintf but not snprintf. */
int snprintf(char *buffer, size_t size, const char *format, ...)
{
    va_list args;
    int result;
    va_start(args, format);
    result = vsnprintf(buffer, size, format, args);
    va_end(args);
    return result;
}

/* The ROM's C library fills memory through MI_CpuFill8 and links no memset. */
void *memset(void *destination, int value, size_t size)
{
    MI_CpuFill8(destination, (u8)value, size);
    return destination;
}

/* Mbed TLS's default allocator pointers name the C library's calloc and free,
 * which this ROM never links. The network worker installs its own buffer
 * allocator (mbedtls_memory_buffer_alloc_init) before any allocation, so the
 * defaults are never called; they fail closed if they ever were. */
void *calloc(size_t count, size_t size)
{
    (void)count; (void)size;
    return NULL;
}

void free(void *pointer)
{
    (void)pointer;
}

/* Mbed TLS's buffer allocator stops on a corrupted heap (MBEDTLS_MEMORY_DEBUG):
 * the console halts, as the SDK's own fatal errors do. */
void OS_Terminate(void);

void exit(int status)
{
    (void)status;
    OS_Terminate();
}

/* The 64-bit right shift the compiler calls and the ROM's runtime lacks: the
 * value in r0 (low) and r1 (high), the shift in r2. */
u64 _ll_ushr(u64 value, u32 shift);

#pragma thumb off

asm u64 _ll_ushr(u64 value, u32 shift)
{
    and r2, r2, #0x3f
    subs r3, r2, #0x20
    movge r0, r1, lsr r3
    movge r1, #0x0
    bxge lr
    rsb r3, r2, #0x20
    mov r0, r0, lsr r2
    orr r0, r0, r1, lsl r3
    mov r1, r1, lsr r2
    bx lr
}

#pragma thumb reset
