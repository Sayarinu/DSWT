#include "wonder_trade/game_adapter.h"

#include <nitro.h>

#include "global.h"

#include "heap.h"
#include "poke_overlay.h"
#include "system.h"
#include "unk_02034B0C.h"
#include "wonder_trade/hgss/game_save.h"

FS_EXTERN_OVERLAY(OVY_0);

/* unk_02034B0C.s: the wireless driver's status (0 off, 2 ready) and stop. */
extern BOOL sub_02034DCC(void);
extern void sub_02034DE0(void);

#define SLEEP_SAVE_DATA 1

/* Family 3 (HG/SS), the game's version ID, English (2) and revision 0. Enrollment
 * takes the revision from the running cartridge (WtGame_EnrollVariant). */
#if defined(SOULSILVER)
#define WT_VARIANT 0x03080200
#else
#define WT_VARIANT 0x03070200
#endif

/* HeartGold and SoulSilver never use the 64 KiB flash sectors at 0x30000 and
 * 0x70000 (protocol/save-layout-hgss.json). Their carts' infrared flash
 * identifies as FLASH_4MBITS_EX; the game falls back to FLASH_4MBITS. */
const WtGameConfig gWtGameConfig = {
    { 0x30000, 0x70000 },
    2, 1,
    { CARD_BACKUP_TYPE_FLASH_4MBITS_EX, CARD_BACKUP_TYPE_FLASH_4MBITS },
    WT_VARIANT,
};

/* The European English cartridge runs the same code as the US one, so the
 * revision comes from the running cartridge's header (offset 0x1E). */
uint32_t WtGame_EnrollVariant(void)
{
    return (gWtGameConfig.enrollVariant & ~0xFFu) | *(const u8 *)(HW_ROM_HEADER_BUF + 0x1E);
}

void *WtGame_Alloc(WtGame *game, size_t size)
{
    return Heap_Alloc(game->heapID, size);
}

void WtGame_Free(WtGame *game, void *pointer)
{
    (void)game;
    Heap_Free(pointer);
}

/* The Wi-Fi Connection libraries (DWC base and access-point client) live in
 * overlay 0; SOC, SSL and WCM are in the static binary. */
void WtGame_WirelessStart(void)
{
    HandleLoadOverlay(FS_OVERLAY_ID(OVY_0), OVY_LOAD_ASYNC);
    sub_02034D8C();
}

int WtGame_WirelessReady(void)
{
    return sub_02034DB8();
}

void WtGame_WirelessShutdown(void)
{
    sub_02034DE0();
}

int WtGame_WirelessActive(void)
{
    return sub_02034DCC();
}

void WtGame_WirelessRelease(void)
{
    UnloadOverlayByID(FS_OVERLAY_ID(OVY_0));
}

void WtGame_HoldAwake(int hold)
{
    if (hold) Sys_SetSleepDisableFlag(SLEEP_SAVE_DATA);
    else Sys_ClearSleepDisableFlag(SLEEP_SAVE_DATA);
}

void WtGame_SampleInput(WtEntropySample *sample)
{
    sample->frame = gSystem.vblankCounter; sample->keys = gSystem.heldKeysRaw;
    sample->x = gSystem.touchX; sample->y = gSystem.touchY; sample->touching = gSystem.touchHeld;
}
