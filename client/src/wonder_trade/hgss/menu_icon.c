#include "wonder_trade/hgss/menu_icon.h"

#include <nitro.h>
#include <string.h>

#include "global.h"

#include "constants/field/map_load.h"

#include "msgdata/msg.naix"
#include "msgdata/msg/msg_0829.h"

#include "bg_window.h"
#include "filesystem.h"
#include "font.h"
#include "heap.h"
#include "map_header.h"
#include "msgdata.h"
#include "party.h"
#include "save_arrays.h"
#include "save_vars_flags.h"
#include "sprite_transfer.h"
#include "sys_flags.h"
#include "text.h"
#include "unk_02009D48.h"
#include "unk_0200A090.h"
#include "unk_02066EDC.h"

/* Overlay 27 (asm/overlay_27.s) owns the X menu's touch-screen grid. Its work
 * area is not described in C; these are the fields the grid code itself uses. */
#define MENU_FIELD(menu, offset, type) (*(type *)((u8 *)(menu) + (offset)))
#define MENU_TOUCH_INPUT(menu)         MENU_FIELD(menu, 0x0C, u16 *)
#define MENU_FIELD_SYSTEM(menu)        MENU_FIELD(menu, 0x10, FieldSystem *)
#define MENU_CURSOR(menu)              MENU_FIELD(menu, 0x14, int)
#define MENU_SPRITE_LIST(menu)         MENU_FIELD(menu, 0x18, SpriteList *)
#define MENU_RES_MAN(menu, type)       MENU_FIELD(menu, 0x144 + 4 * (type), GF_2DGfxResMan *)
#define MENU_SPRITE(menu, index)       MENU_FIELD(menu, 0x390 + 4 * (index), Sprite *)
#define MENU_LABEL_WINDOW(menu, index) ((Window *)((u8 *)(menu) + 0x3F0 + 0x10 * (index)))
#define MENU_ICON_PALETTES(menu)       ((u8 *)(menu) + 0x4CC)
#define MENU_MODE(menu)                ((MENU_FIELD(menu, 0x51C, u32) >> 1) & 0xF)

#define MENU_MODE_FIELD        0
#define MENU_HEAP_ID           HEAP_ID_8
#define MENU_ICON_CELL_RES_ID  100 /* the grid icons' shared cell and animation bank */
#define MENU_ICON_PALETTE_FILE 14  /* the grid icons' palette in a/0/1/4 */
#define MENU_LABEL_WIDTH       72
#define MENU_LABEL_COLOR       MAKE_TEXT_COLOR(14, 2, 0)

/* The free cell: second column, fourth row, with label window 7 beneath it. */
#define WT_ICON_X           104
#define WT_ICON_Y           142
#define WT_ICON_LABEL       7
#define WT_ICON_RES_ID      0x5754
#define WT_ICON_NARC_MEMBER 0

/* Grid indices: the first column holds 0-3 top to bottom, the second 4-6 and
 * then the Wonder Trade icon. */
enum {
    NAV_UP,
    NAV_DOWN,
    NAV_LEFT,
    NAV_RIGHT,
};

extern int ov27_0225B360(int index, int direction, const u8 *available);
extern void ov27_0225B398(void *menu, int index);
extern int ov27_0225C170(void *menu, int index);
extern int ov27_0225C1AC(void *menu, int selection);

static struct {
    GF_2DGfxResMan *charMan;
    GF_2DGfxResMan *plttMan;
    SpriteResource *charRes;
    SpriteResource *plttRes;
    SpriteResourcesHeader header;
    Sprite *sprite;
    int paletteSlot;
    int shown;
} sWtMenuIcon;

BOOL WtMenuIcon_Available(FieldSystem *fieldSystem) {
    SaveVarsFlags *flags = Save_VarsFlags_Get(fieldSystem->saveData);

    // The start menu forbids saving in these places.
    if (Save_VarsFlags_CheckSafariSysFlag(flags) || Save_VarsFlags_CheckBugContestFlag(flags)
        || Save_VarsFlags_CheckPalParkSysFlag(flags) || FieldSystem_MapIsBattleTowerMultiPartnerSelectRoom(fieldSystem)
        || fieldSystem->mapLoadType == MAP_LOAD_TYPE_UNION || fieldSystem->mapLoadType == MAP_LOAD_TYPE_COLOSSEUM) {
        return FALSE;
    }
    // ...and hides the Pokémon icon without a starter or in Amity Square.
    return CheckGotStarter(flags) && !MapHeader_MapIsAmitySquare(fieldSystem->location->mapId)
        && Party_GetCount(SaveArray_Party_Get(fieldSystem->saveData)) > 0;
}

static void WtMenuIcon_Release(void) {
    if (sWtMenuIcon.sprite != NULL) {
        Sprite_Delete(sWtMenuIcon.sprite);
    }
    if (sWtMenuIcon.charRes != NULL) {
        SpriteTransfer_DeleteCharTransferTask(sWtMenuIcon.charRes);
    }
    if (sWtMenuIcon.plttRes != NULL) {
        SpriteTransfer_DeletePlttTransferTask(sWtMenuIcon.plttRes);
    }
    if (sWtMenuIcon.charMan != NULL) {
        Destroy2DGfxResObjMan(sWtMenuIcon.charMan);
    }
    if (sWtMenuIcon.plttMan != NULL) {
        Destroy2DGfxResObjMan(sWtMenuIcon.plttMan);
    }
    memset(&sWtMenuIcon, 0, sizeof(sWtMenuIcon));
}

void WtMenuIcon_Create(void *menu) {
    memset(&sWtMenuIcon, 0, sizeof(sWtMenuIcon));
    // Safari, Bug-Catching Contest and similar layouts never allow Wonder Trade.
    if (MENU_MODE(menu) != MENU_MODE_FIELD) {
        return;
    }

    sWtMenuIcon.charMan = Create2DGfxResObjMan(1, GF_GFX_RES_TYPE_CHAR, MENU_HEAP_ID);
    sWtMenuIcon.plttMan = Create2DGfxResObjMan(1, GF_GFX_RES_TYPE_PLTT, MENU_HEAP_ID);
    SpriteResource *charRes = AddCharResObjFromNarc(sWtMenuIcon.charMan, NARC_wonder_trade_menu_icon, WT_ICON_NARC_MEMBER, FALSE, WT_ICON_RES_ID, NNS_G2D_VRAM_TYPE_2DSUB, MENU_HEAP_ID);
    SpriteResource *plttRes = AddPlttResObjFromNarc(sWtMenuIcon.plttMan, NARC_a_0_1_4, MENU_ICON_PALETTE_FILE, FALSE, WT_ICON_RES_ID, NNS_G2D_VRAM_TYPE_2DSUB, 1, MENU_HEAP_ID);
    if (!SpriteTransfer_CreateCharTransferTask_AllocAtEnd(charRes)) {
        WtMenuIcon_Release();
        return;
    }
    sWtMenuIcon.charRes = charRes;
    sub_0200A740(charRes);
    if (!SpriteTransfer_CreatePlttTransferTask(plttRes)) {
        WtMenuIcon_Release();
        return;
    }
    sWtMenuIcon.plttRes = plttRes;
    sub_0200A740(plttRes);
    sWtMenuIcon.paletteSlot = SpriteTransfer_GetPlttOffset(plttRes, NNS_G2D_VRAM_TYPE_2DSUB);

    CreateSpriteResourcesHeader(&sWtMenuIcon.header, WT_ICON_RES_ID, WT_ICON_RES_ID, MENU_ICON_CELL_RES_ID, MENU_ICON_CELL_RES_ID, -1, -1, FALSE, 0,
        sWtMenuIcon.charMan, sWtMenuIcon.plttMan, MENU_RES_MAN(menu, GF_GFX_RES_TYPE_CELL), MENU_RES_MAN(menu, GF_GFX_RES_TYPE_ANIM), NULL, NULL);

    SpriteTemplate template;
    template.spriteList = MENU_SPRITE_LIST(menu);
    template.header = &sWtMenuIcon.header;
    template.position.x = FX32_CONST(WT_ICON_X);
    template.position.y = FX32_CONST(WT_ICON_Y) + FX32_CONST(256); // sub-screen surface
    template.position.z = 0;
    template.scale.x = FX32_ONE;
    template.scale.y = FX32_ONE;
    template.scale.z = FX32_ONE;
    template.rotation = 0;
    template.drawPriority = 1;
    template.whichScreen = NNS_G2D_VRAM_TYPE_2DSUB;
    template.heapID = MENU_HEAP_ID;
    sWtMenuIcon.sprite = Sprite_CreateAffine(&template);
    if (sWtMenuIcon.sprite == NULL) {
        WtMenuIcon_Release();
        return;
    }
    Sprite_SetAnimActiveFlag(sWtMenuIcon.sprite, TRUE);
    Sprite_SetPriority(sWtMenuIcon.sprite, 0);
    Sprite_SetAffineOverwriteMode(sWtMenuIcon.sprite, 1);
    Sprite_SetDrawFlag(sWtMenuIcon.sprite, FALSE);
    sWtMenuIcon.shown = -1;
}

void WtMenuIcon_Destroy(void *menu) {
    (void)menu;
    WtMenuIcon_Release();
}

static BOOL WtMenuIcon_Shown(void) {
    return sWtMenuIcon.sprite != NULL && sWtMenuIcon.shown == TRUE;
}

static void WtMenuIcon_DrawLabel(void *menu, BOOL shown) {
    Window *window = MENU_LABEL_WINDOW(menu, WT_ICON_LABEL);

    FillWindowPixelBuffer(window, 0);
    if (shown) {
        MsgData *messages = NewMsgDataFromNarc(MSGDATA_LOAD_DIRECT, NARC_msgdata_msg, msg_0829, MENU_HEAP_ID);
        String *label = NewString_ReadMsgData(messages, WonderTrade_Text_Title);
        u32 width = FontID_String_GetWidth(FONT_SYSTEM, label, 0);
        u32 x = width < MENU_LABEL_WIDTH ? (MENU_LABEL_WIDTH - width) / 2 : 0;

        AddTextPrinterParameterizedWithColor(window, FONT_SYSTEM, label, x, 0, TEXT_SPEED_NOTRANSFER, MENU_LABEL_COLOR, NULL);
        String_Delete(label);
        DestroyMsgData(messages);
    }
    CopyWindowToVram(window);
}

void WtMenuIcon_Update(void *menu) {
    if (sWtMenuIcon.sprite == NULL) {
        return;
    }

    FieldSystem *fieldSystem = MENU_FIELD_SYSTEM(menu);
    BOOL shown = WtMenuIcon_Available(fieldSystem);
    if (shown != sWtMenuIcon.shown) {
        sWtMenuIcon.shown = shown;
        Sprite_SetDrawFlag(sWtMenuIcon.sprite, shown);
        WtMenuIcon_DrawLabel(menu, shown);
        if (!shown && MENU_CURSOR(menu) == WT_MENU_ICON_INDEX) {
            fieldSystem->unkD3 = 0;
            MENU_CURSOR(menu) = ov27_0225C1AC(menu, 0);
        }
        ov27_0225B398(menu, MENU_CURSOR(menu));
    }
    // As the grid does for its icons: dim all but the selected icon while the menu is open.
    if (!fieldSystem->unkD2_7) {
        BOOL selected = fieldSystem->unkD2_0 != 0 && MENU_CURSOR(menu) == WT_MENU_ICON_INDEX;
        Sprite_SetOamMode(sWtMenuIcon.sprite, selected ? GX_OAM_MODE_NORMAL : GX_OAM_MODE_XLU);
    }
}

Sprite *WtMenuIcon_GetSprite(void *menu, int index) {
    if (index == WT_MENU_ICON_INDEX) {
        return sWtMenuIcon.sprite;
    }
    return MENU_SPRITE(menu, index);
}

void WtMenuIcon_Highlight(void *menu, int index) {
    if (sWtMenuIcon.sprite == NULL || sWtMenuIcon.paletteSlot < 0) {
        return;
    }

    // The grid keeps its normal palette followed by its highlighted one.
    u8 *palette = MENU_ICON_PALETTES(menu) + (index == WT_MENU_ICON_INDEX ? 0x20 : 0);
    DC_FlushRange(palette, 0x20);
    GXS_LoadOBJPltt(palette, sWtMenuIcon.paletteSlot * 0x20, 0x20);
}

int WtMenuIcon_Navigate(int index, int direction, const u8 *available) {
    if (!WtMenuIcon_Shown()) {
        return ov27_0225B360(index, direction, available);
    }

    if (index == WT_MENU_ICON_INDEX) {
        // Vertical moves wrap within the second column; sideways moves reach the first.
        static const u8 sMoves[4][3] = {
            [NAV_UP] = { 6, 5, 4 },
            [NAV_DOWN] = { 4, 5, 6 },
            [NAV_LEFT] = { 3, 2, 1 },
            [NAV_RIGHT] = { 3, 2, 1 },
        };
        for (int i = 0; i < 3; ++i) {
            if (available[sMoves[direction][i] * 8]) {
                return sMoves[direction][i];
            }
        }
        return -1;
    }
    if ((index == 3 && (direction == NAV_LEFT || direction == NAV_RIGHT))
        || (index == 6 && direction == NAV_DOWN)
        || (index == 4 && direction == NAV_UP)) {
        return WT_MENU_ICON_INDEX;
    }
    return ov27_0225B360(index, direction, available);
}

int WtMenuIcon_SelectionForIndex(void *menu, int index) {
    if (index == WT_MENU_ICON_INDEX) {
        return WT_MENU_ICON_SELECTION;
    }
    return ov27_0225C170(menu, index);
}

BOOL WtMenuIcon_Touch(void *menu) {
    if (!WtMenuIcon_Shown()) {
        return FALSE;
    }

    MENU_CURSOR(menu) = WT_MENU_ICON_INDEX;
    MENU_FIELD_SYSTEM(menu)->unkD3 = WT_MENU_ICON_SELECTION;
    ov27_0225B398(menu, WT_MENU_ICON_INDEX);
    *MENU_TOUCH_INPUT(menu) = WT_MENU_ICON_SELECTION + 2;
    return TRUE;
}
