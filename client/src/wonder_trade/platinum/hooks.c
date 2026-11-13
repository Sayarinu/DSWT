#include "wonder_trade/game_adapter.h"

#include <nitro.h>

#include "constants/net.h"
#include "heap.h"
#include "system.h"
#include "unk_02033200.h"
#include "wifi_overlays.h"
#include "wonder_trade/platinum/game_save.h"

/* The build sets the enrolled variant: legacy code 1 for US English Platinum,
 * 0x020C020A for European English (revision 10). */
#ifndef WT_GAME_VARIANT
#error "The build sets WT_GAME_VARIANT, the enrolled game variant"
#endif

/* Platinum never uses the 64 KiB flash sectors at 0x30000 and 0x70000. */
const WtGameConfig gWtGameConfig = {
    { 0x30000, 0x70000 },
    DEFAULT_DWC_DMA_NUMBER, DEFAULT_DWC_POWER_MODE,
    { CARD_BACKUP_TYPE_FLASH_4MBITS, CARD_BACKUP_TYPE_FLASH_4MBITS },
    WT_GAME_VARIANT,
};

void *WtGame_Alloc(WtGame *game, size_t size)
{
    return Heap_Alloc(game->heapID, size);
}

void WtGame_Free(WtGame *game, void *pointer)
{
    (void)game;
    Heap_Free(pointer);
}

void WtGame_WirelessStart(void)
{
    Overlay_LoadWFCOverlay();
    WirelessDriver_Init();
}

int WtGame_WirelessReady(void)
{
    return WirelessDriver_IsReady();
}

void WtGame_WirelessShutdown(void)
{
    WirelessDriver_Shutdown();
}

int WtGame_WirelessActive(void)
{
    return WirelessDriver_Initialized();
}

void WtGame_WirelessRelease(void)
{
    Overlay_UnloadWFCOverlay();
}

void WtGame_HoldAwake(int hold)
{
    if (hold) SleepLock(SLEEP_TYPE_SAVE_DATA);
    else SleepUnlock(SLEEP_TYPE_SAVE_DATA);
}

void WtGame_SampleInput(WtEntropySample *sample)
{
    sample->frame = gSystem.vblankCounter; sample->keys = gSystem.heldKeysRaw;
    sample->x = gSystem.touchX; sample->y = gSystem.touchY; sample->touching = gSystem.touchHeld;
}
