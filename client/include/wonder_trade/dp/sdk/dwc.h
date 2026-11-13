#ifndef WONDER_TRADE_DP_SDK_DWC_H
#define WONDER_TRADE_DP_SDK_DWC_H

/* The Wi-Fi Connection (DWC) calls the shared network core makes, declared for
 * Diamond and Pearl. Their DWC library exists only as undecompiled assembly in
 * overlay 4 (arm9/overlays/04/asm), so each name below maps to that overlay's
 * label. Every mapping, argument layout and return value was read from the
 * assembly; the overlay must be loaded while any of them runs. */

#include <nitro.h>

/* Passed through to the allocator unchanged; the core never inspects it. */
typedef int DWCAllocType;
typedef void *(*DWCAllocEx)(DWCAllocType type, u32 size, int alignment);
typedef void (*DWCFreeEx)(DWCAllocType type, void *pointer, u32 size);

/* ov04_021DD968 stores the allocator and release functions that
 * ov04_021DD944 (allocate with 32-byte alignment) and ov04_021DD904
 * (release) call; ov04_021DD6F0 clears the error state unless it is fatal (9). */
void ov04_021DD968(DWCAllocEx alloc, DWCFreeEx free);
void *ov04_021DD944(DWCAllocType type, u32 size);
void ov04_021DD904(DWCAllocType type, void *pointer, u32 size);
void ov04_021DD6F0(void);
#define DWC_SetMemFunc ov04_021DD968
#define DWC_Alloc ov04_021DD944
#define DWC_Free ov04_021DD904
#define DWC_ClearError ov04_021DD6F0

/* ov04_021EBC60 reads the access-point client configuration as two function
 * pointers followed by four bytes: DMA channel, power mode, connection type
 * (low four bits) and skip-connection-test flag (two bits), the last of which
 * ov04_021EDEC8 checks before the Nintendo connection test. */
typedef struct DWCACOption {
    u8 connectType;
    u8 skipNetCheck;
} DWCACOption;

typedef struct DWCACConfig {
    void *(*alloc)(u32 type, s32 size);
    void (*free)(u32 type, void *pointer, s32 size);
    u8 dmaNo;
    u8 powerMode;
    DWCACOption option;
} DWCACConfig;

/* ov04_021EBAE8 maps the client's phase to these states, or returns the
 * client's negative error once it has failed. */
enum {
    DWC_AC_STATE_NOOPERATION,
    DWC_AC_STATE_SEARCH,
    DWC_AC_STATE_CONNECT,
    DWC_AC_STATE_TEST,
    DWC_AC_STATE_ERROR,
    DWC_AC_STATE_COMPLETE,
};

/* Create returns nonzero once the WCM startup was accepted; Destroy returns
 * nonzero only after the client is idle and its memory released. */
BOOL ov04_021EBC60(DWCACConfig *config);
int ov04_021EBB80(void);
int ov04_021EBAE8(void);
BOOL ov04_021EB9D8(void);
#define DWC_AC_Create ov04_021EBC60
#define DWC_AC_Process ov04_021EBB80
#define DWC_AC_GetStatus ov04_021EBAE8
#define DWC_AC_Destroy ov04_021EB9D8

#endif
