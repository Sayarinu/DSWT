#include <nitro.h>
#include <stdlib.h>

#include "CARD_common.h"

/* SDK and runtime functions the Wonder Trade code needs that Diamond and
 * Pearl never link. They follow the Nitro SDK's and the compiler runtime's
 * documented behavior on pokediamond's own decompiled structures. */

/* The guard words OS_CreateThread writes (lib/NitroSDK/src/OS_thread.c). */
#define STACK_BOTTOM_GUARD 0xfddb597dUL
#define STACK_TOP_GUARD 0x7bf9dd5bUL

extern CARDiCommon cardi_common;

OSStackStatus OS_GetStackStatus(const OSThread *thread)
{
    if (*(const u32 *)(thread->stackBottom - sizeof(u32)) != STACK_BOTTOM_GUARD) return OS_STACK_UNDERFLOW;
    /* This SDK never sets a warning offset, so there is no third guard. */
    if (*(const u32 *)thread->stackTop != STACK_TOP_GUARD) return OS_STACK_OVERFLOW;
    return OS_STACK_NO_ERROR;
}

/* CARD_IdentifyBackup records the selected device in the shared command
 * block (lib/NitroSDK/src/CARD_spi.c: CARDi_IdentifyBackupCore). */
CARDBackupType CARD_GetCurrentBackupType(void)
{
    return cardi_common.cmd->type;
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

/* 64-bit helpers the 2.0/sp1 compiler calls that the ROM's runtime lacks.
 * They follow its _ll_shl and _ll_mul (asm/RUNTIME_ll_*.s): the value in
 * r0 (low) and r1 (high), the second operand in r2 (and r3), the result in
 * r0 and r1. ARM register shifts by 32 or more yield zero. */
u64 _ll_ushr(u64 value, u32 shift);
u64 _ull_mul(u64 left, u64 right);

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

/* The low 64 bits of a product do not depend on signedness. */
asm u64 _ull_mul(u64 left, u64 right)
{
    stmdb sp!, {r4, r5}
    umull r5, r4, r0, r2
    mla r4, r0, r3, r4
    mla r4, r2, r1, r4
    mov r1, r4
    mov r0, r5
    ldmia sp!, {r4, r5}
    bx lr
}
