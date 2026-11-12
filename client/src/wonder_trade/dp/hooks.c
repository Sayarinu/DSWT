#include "wonder_trade/game_adapter.h"

#include <nitro.h>

#include "global.h"

#include "PAD_pad.h"

#include "constants/sndseq.h"

#include "field_system.h"
#include "game_init.h"
#include "heap.h"
#include "main.h"
#include "party.h"
#include "start_menu.h"
#include "task.h"
#include "unk_020051F4.h"
#include "wonder_trade/dp/application.h"
#include "wonder_trade/dp/game_save.h"

/* asm/unk_02088858.s: load and unload overlay 4, which holds the Wi-Fi
 * Connection libraries, exactly as the GTS (overlay 80) does. SOC, SSL and
 * WCM are in the static binary; the ARM7 binary carries the wireless driver,
 * so nothing else needs starting or stopping. */
extern void sub_02088858(void);
extern void sub_02088868(void);
/* overlays/05/asm/ov05_021D74E0.s: start the field's fade to black (0) or
 * back in (1), as every start-menu action does before leaving the field. */
extern void ov05_021D7CA4(int direction);

/* The sleep-disable bit the game's own save code holds while writing flash
 * (src/save.c: sub_02016444(1) and sub_02016454(1)). */
#define SLEEP_SAVE_DATA 1

/* Start-menu states (sub_020352E4 in asm/unk_02035068.s): wait for the fade,
 * then run the exit task; and reopen the menu once the field is back. The
 * Options action (sub_02036684) uses the same two. */
#define START_MENU_STATE_FADE_THEN_EXIT 2
#define START_MENU_STATE_REOPEN 12
/* sub_02035270 and friends in asm/unk_02035068.s: menu entries that are
 * hidden here; bit 1 is Pokémon and bit 4 is Save. */
#define START_MENU_HIDE_POKEMON (1 << 1)
#define START_MENU_HIDE_SAVE (1 << 4)

/* Family 1 (Diamond and Pearl), the game's version ID (10 Diamond, 11 Pearl),
 * English (2), revision 5 (protocol/README.md, "Game variants"). */
#if defined(PEARL)
#define WT_VARIANT 0x010B0205
#else
#define WT_VARIANT 0x010A0205
#endif

/* Diamond and Pearl never use the 64 KiB flash sectors at 0x30000 and 0x70000
 * (protocol/save-layout-dp.json). SaveDetectFlash in src/save.c accepts a
 * 4 Mbit or 2 Mbit flash; only the 4 Mbit one has room for the journal. The
 * DMA channel (2) and power mode (1) are those the GTS starts Wi-Fi with
 * (ov80_02236870 calling ov04_021DDEE0). */
const WtGameConfig gWtGameConfig = {
    { 0x30000, 0x70000 },
    2, 1,
    { CARD_BACKUP_TYPE_FLASH_4MBITS, CARD_BACKUP_TYPE_FLASH_4MBITS },
    WT_VARIANT,
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
    sub_02088858();
}

int WtGame_WirelessReady(void)
{
    return TRUE;
}

void WtGame_WirelessShutdown(void)
{
}

int WtGame_WirelessActive(void)
{
    return FALSE;
}

void WtGame_WirelessRelease(void)
{
    sub_02088868();
}

void WtGame_HoldAwake(int hold)
{
    /* The mask is shared with the main thread; update it atomically. */
    OSIntrMode interrupts = OS_DisableInterrupts();
    if (hold) sub_02016444(SLEEP_SAVE_DATA);
    else sub_02016454(SLEEP_SAVE_DATA);
    OS_RestoreInterrupts(interrupts);
}

void WtGame_SampleInput(WtEntropySample *sample)
{
    /* gSystem.unk2C counts main-loop frames (src/main.c). */
    sample->frame = gSystem.unk2C; sample->keys = gSystem.heldKeysRaw;
    sample->x = gSystem.touchX; sample->y = gSystem.touchY; sample->touching = gSystem.touchHeld;
}

static BOOL WonderTrade_StartMenuReturn(TaskManager *taskManager)
{
    FieldSystem *fieldSystem = TaskManager_GetFieldSystem(taskManager);
    StartMenuTaskData *startMenu = TaskManager_GetEnvironment(taskManager);
    FieldSystem_LoadFieldOverlay(fieldSystem);
    startMenu->state = START_MENU_STATE_REOPEN;
    return FALSE;
}

/* sub_020373D4 starts an application while the field is suspended; the
 * start menu's own actions launch theirs the same way. */
static BOOL WonderTrade_StartMenuLaunch(TaskManager *taskManager)
{
    FieldSystem *fieldSystem = TaskManager_GetFieldSystem(taskManager);
    StartMenuTaskData *startMenu = TaskManager_GetEnvironment(taskManager);
    sub_020373D4(fieldSystem, (struct OverlayManagerTemplate *)&gWonderTradeAppTemplate, fieldSystem);
    startMenu->exitTaskFunc = WonderTrade_StartMenuReturn;
    return FALSE;
}

BOOL WonderTrade_StartMenuInput(TaskManager *taskManager)
{
    FieldSystem *fieldSystem = TaskManager_GetFieldSystem(taskManager);
    StartMenuTaskData *startMenu = TaskManager_GetEnvironment(taskManager);
    /* Not where saving is (Safari Zone, Pal Park, Union Room and similar), nor
     * before the player has Pokémon. */
    if (!(gSystem.newKeys & PAD_BUTTON_SELECT)
        || (startMenu->hiddenEntries & (START_MENU_HIDE_SAVE | START_MENU_HIDE_POKEMON))
        || Party_GetCount(SaveArray_Party_Get(fieldSystem->saveData)) == 0) return FALSE;
    PlaySE(SEQ_SE_DP_SELECT);
    ov05_021D7CA4(0);
    startMenu->exitTaskFunc = WonderTrade_StartMenuLaunch;
    startMenu->state = START_MENU_STATE_FADE_THEN_EXIT;
    return TRUE;
}
