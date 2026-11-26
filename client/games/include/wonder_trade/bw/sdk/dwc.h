#ifndef WONDER_TRADE_BW_SDK_DWC_H
#define WONDER_TRADE_BW_SDK_DWC_H

/* TWL-DWC 5.3's auto-connect and memory functions, which Black and White link
 * in ARM9 main. */

#include <nitro.h>

typedef enum DWCAllocType {
    DWC_ALLOCTYPE_AUTH,
    DWC_ALLOCTYPE_AC,
    DWC_ALLOCTYPE_BM,
    DWC_ALLOCTYPE_UTIL,
    DWC_ALLOCTYPE_BASE,
    DWC_ALLOCTYPE_LOGIN,
    DWC_ALLOCTYPE_FRIEND,
    DWC_ALLOCTYPE_MATCH,
    DWC_ALLOCTYPE_TRANSPORT,
    DWC_ALLOCTYPE_GS,
    DWC_ALLOCTYPE_ND
} DWCAllocType;

typedef void *(*DWCAllocEx)(DWCAllocType name, u32 size, int align);
typedef void (*DWCFreeEx)(DWCAllocType name, void *ptr, u32 size);
typedef void *(*DWCACAlloc)(u32 name, s32 size);
typedef void (*DWCACFree)(u32 name, void *ptr, s32 size);

/* What DWC_AC_Create is given, 0x14 bytes. */
typedef struct DWCACConfig {
    DWCACAlloc alloc;
    DWCACFree free;
    u8 dmaNo;
    u8 powerMode;
    u8 mode;
    u8 skipConnectionTest : 4;
    u8 rssiPacked : 4;
    s32 rwin;
    u8 ignoreEula;
    u8 reserved[3];
} DWCACConfig;

typedef char WtDwcAcConfigSize[sizeof(DWCACConfig) == 0x14 ? 1 : -1];

/* DWC_AC_GetStatus reports 5 once the connection is up. */
#define DWC_AC_STATE_COMPLETE 5

/* TWL-DWC has no connection-test option block; the test has its own bit. */
#define WT_DWC_SKIP_NET_CHECK(config) ((config).skipConnectionTest = 1)

BOOL DWC_AC_Create(DWCACConfig *config);
BOOL DWC_AC_Destroy(void);
int DWC_AC_Process(void);
int DWC_AC_GetStatus(void);
void *DWC_Alloc(DWCAllocType name, u32 size);
void DWC_Free(DWCAllocType name, void *ptr, u32 size);
void DWC_ClearError(void);
/* DWC_SetMemFunc as aletheia names it. */
void DWCi_SetMemFunc(DWCAllocEx alloc, DWCFreeEx free);
#define DWC_SetMemFunc(alloc, free) DWCi_SetMemFunc((alloc), (free))

#endif
