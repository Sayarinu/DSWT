#include "wonder_trade/game_adapter.h"

#include <nitro.h>

#include "wonder_trade/bw/game.h"
#include "wonder_trade/bw/game_save.h"

/* The port sets each version's enrolled variant (scripts/retail_port/bw.py):
 * 0x04150200 for Black and 0x04140200 for White. */
#ifndef WT_GAME_VARIANT
#error "The port sets WT_GAME_VARIANT, the enrolled game variant"
#endif

/* The journal banks lie in the musical distribution save, which the
 * integration patch stops the game from using (protocol/save-layout-bw.json).
 * Wi-Fi takes DMA channel 2 and power mode 1, as the game's own
 * DWC_InitInetEx call does. */
const WtGameConfig gWtGameConfig = {
    { 0x74000, 0x75000 },
    2, 1,
    { CARD_BACKUP_TYPE_FLASH_4MBITS_EX, CARD_BACKUP_TYPE_FLASH_4MBITS_EX },
    WT_GAME_VARIANT,
    WT_SAVE_MAGIC,
};

uint32_t WtGame_EnrollVariant(void)
{
    return gWtGameConfig.enrollVariant;
}

/* Never terminates the game on exhaustion. */
void *WtGame_Alloc(WtGame *game, size_t size)
{
    return GFL_HEAP_AllocMemory(game->heapId, size);
}

void WtGame_Free(WtGame *game, void *pointer)
{
    (void)game;
    if (pointer) GFL_HEAP_FreeMemory(pointer);
}

/* The Wi-Fi code is overlay 9 (DWC's wrapper, SOC, CPS, WCM) with overlay 13,
 * which share memory with the field's common overlays 10 and 11: the game's
 * own Wi-Fi device swaps them the same way (arm9/net_device_table.c). */
void WtGame_WirelessStart(void)
{
    GF_OVERLAY_Unload(10);
    GF_OVERLAY_Unload(11);
    GF_OVERLAY_Load(9);
    GF_OVERLAY_Load(13);
}

int WtGame_WirelessReady(void)
{
    return 1;
}

void WtGame_WirelessShutdown(void)
{
}

int WtGame_WirelessActive(void)
{
    return 0;
}

void WtGame_WirelessRelease(void)
{
    GF_OVERLAY_Unload(13);
    GF_OVERLAY_Unload(9);
    GF_OVERLAY_Load(10);
    GF_OVERLAY_Load(11);
}

/* The save system keeps the console awake with the same flag. */
void WtGame_HoldAwake(int hold)
{
    if (hold) UI_SetKeepAwakeFlagsGlobal(1);
    else UI_ClearKeepAwakeFlagsGlobal(1);
}

void WtGame_SampleInput(WtEntropySample *sample)
{
    static u32 sFrame;
    u32 x = 0, y = 0;
    sample->frame = ++sFrame;
    sample->keys = KEY_GetCont();
    sample->touching = TOUCHPANEL_GetPointCont(&x, &y) ? 1 : 0;
    sample->x = (u16)x;
    sample->y = (u16)y;
}

/* The last save write was refused (client/aletheia.patch, arm9/flash_access.c). */
u32 gWtFlashRefused;

/* Overlay entries into the module, written each time their overlay loads
 * (scripts/retail_port/bw.py fills the table): the overlay, the address and
 * the bytes. An overlay file stays the cartridge's own, so the SDK's digest
 * check of it passes; the entries go in after the check. */
typedef struct WtOverlayHook {
    u32 overlay, address, size;
    u8 code[8];
} WtOverlayHook;

#define WT_OVERLAY_HOOK_MAX 48
#define WT_OVERLAY_HOOK_END 0xFFFFFFFF

WtOverlayHook gWtOverlayHooks[WT_OVERLAY_HOOK_MAX] = { { WT_OVERLAY_HOOK_END } };
/* The start of the SDK's FS_StartOverlay, moved into the module by the port. */
u32 gWtOverlayResume = 0xFFFFFFFF;

void IC_InvalidateRange(void *startAddr, u32 nBytes);
void WtOverlay_Started(const u32 *info);
void WtOverlay_Detour(void);

/* After FS_StartOverlay: its first word is the overlay's ID (FSOverlayInfo). */
void WtOverlay_Started(const u32 *info)
{
    const WtOverlayHook *hook;
    for (hook = gWtOverlayHooks; hook->overlay != WT_OVERLAY_HOOK_END; ++hook) {
        if (hook->overlay != info[0]) continue;
        MIi_CpuCopy16(hook->code, (void *)hook->address, hook->size);
        DC_FlushRange((void *)hook->address, hook->size);
        IC_InvalidateRange((void *)hook->address, hook->size);
    }
}

/* FS_StartOverlay's entry jumps here: the moved start runs the SDK's own
 * function, then the overlay's entries go in. */
#pragma thumb off

asm void WtOverlay_Detour(void)
{
    stmdb sp!, {r0, lr}
    ldr r12, =gWtOverlayResume
    ldr r12, [r12]
    blx r12
    ldmia sp!, {r0, lr}
    ldr r12, =WtOverlay_Started
    bx r12
}

#pragma thumb reset
