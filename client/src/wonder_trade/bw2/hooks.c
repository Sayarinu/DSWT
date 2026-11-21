#include "wonder_trade/game_adapter.h"

#include <nitro.h>

#include "gfl/heapsys.h"
#include "gfl/key.h"
#include "gfl/overlay.h"
#include "gfl/touchpanel.h"
#include "gfl/ui.h"
#include "wonder_trade/bw2/game_save.h"

/* The build sets each version's enrolled variant (configure.py WT_VARIANTS):
 * 0x05170200 for Black 2 and 0x05160200 for White 2. */
#ifndef WT_GAME_VARIANT
#error "The build sets WT_GAME_VARIANT, the enrolled game variant"
#endif

/* The journal banks lie in the musical distribution save, which the
 * integration patch stops the game from using (protocol/save-layout-b2w2.json).
 * Wi-Fi takes DMA channel 2 and power mode 1, as the game's own DWC_InitInetEx
 * call does (src/wifi/device/dwc_rap.c). */
const WtGameConfig gWtGameConfig = {
    { 0x6B000, 0x6C000 },
    2, 1,
    { CARD_BACKUP_TYPE_FLASH_4MBITS_EX, CARD_BACKUP_TYPE_FLASH_4MBITS_EX },
    WT_GAME_VARIANT,
    WT_SAVE_MAGIC,
};

uint32_t WtGame_EnrollVariant(void)
{
    return gWtGameConfig.enrollVariant;
}

/* Never terminates the game on exhaustion, unlike GFL_HeapAllocate. */
void *WtGame_Alloc(WtGame *game, size_t size)
{
    return GFL_HeapAllocateCore(game->heapId, size);
}

void WtGame_Free(WtGame *game, void *pointer)
{
    (void)game;
    if (pointer) GFL_HeapFreeCore(pointer);
}

/* The Wi-Fi code is overlay 11 (DWC's wrapper, SOC, CPS, WCM) with overlay 29,
 * which share memory with the common overlays 12 and 27: the game's own Wi-Fi
 * device swaps them the same way (src/system/net_define.c). DWC_AC_Create
 * starts the wireless manager itself and DWC_AC_Destroy stops it. */
void WtGame_WirelessStart(void)
{
    GFL_OvlUnload(OVERLAY_ID(12));
    GFL_OvlUnload(OVERLAY_ID(27));
    GFL_OvlLoad(OVERLAY_ID(11));
    GFL_OvlLoad(OVERLAY_ID(29));
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
    GFL_OvlUnload(OVERLAY_ID(29));
    GFL_OvlUnload(OVERLAY_ID(11));
    GFL_OvlLoad(OVERLAY_ID(12));
    GFL_OvlLoad(OVERLAY_ID(27));
}

/* The save system blocks sleep with the same flag (src/gfl/backup_system.c). */
void WtGame_HoldAwake(int hold)
{
    if (hold) GCTX_HIDBlockSleep(1);
    else GCTX_HIDUnblockSleep(1);
}

void WtGame_SampleInput(WtEntropySample *sample)
{
    static u32 sFrame;
    u32 x = 0, y = 0;
    sample->frame = ++sFrame;
    sample->keys = GCTX_HIDGetHeldKeys();
    sample->touching = GFL_HIDGetTouchHeldPoint(&x, &y) ? 1 : 0;
    sample->x = (u16)x;
    sample->y = (u16)y;
}
