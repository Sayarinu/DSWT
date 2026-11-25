#ifndef WONDER_TRADE_BW_GAME_H
#define WONDER_TRADE_BW_GAME_H

/* The Black and White functions and structures the adapter uses, under
 * aletheia's names (src/game/arm9 and the overlays named below). The port
 * resolves each name to its address in the cartridge from aletheia's link.
 * Structures the game keeps private to one file are declared here only as far
 * as the adapter reads them; their offsets are the ones aletheia's sources use. */

#include <nitro.h>

struct SaveControl;
struct BackupSaveData;
struct SaveDataTable;
struct GameSystem;
struct GameData;
struct GameEvent;
struct FieldMap;
struct GameComm;
struct Strbuf;
struct GfFont;
struct Bmp;
struct MusicalSave;

/* arm9/procsys.h: a process's init, main and end, each returning 1 when done. */
typedef u32 (*WtProcFunc)(void *proc, u32 *seq, void *procWork, void *work);
typedef struct WtProcData {
    WtProcFunc init, main, end;
} WtProcData;
#define WT_OVERLAY_NONE 0xFFFFFFFF

/* arm9/game_event.h: 0 continue, 1 done. */
typedef int (*WtEventFunc)(struct GameEvent *event, int *seq, void *work);
#define WT_EVENT_CONTINUE 0
#define WT_EVENT_DONE 1

/* Heaps (arm9/gfl_heap.h). Heap 1 is the application heap. */
#define WT_HEAP_APP 1
void *GFL_HEAP_AllocMemory(u32 heapId, u32 size);
u32 GFL_HEAP_FreeMemory(void *memory);
u32 GFL_HEAP_GetAllocatableSize(u32 heapId);
int GFL_HEAP_CreateChildHeapImpl(u32 parentHeapId, u32 newHeapId, u32 size);
void GFL_HEAP_DeleteChecked(u32 heapId);
u32 GFL_HEAP_Free(void *block);

/* Overlays (arm9/gf_overlay.h). */
int GF_OVERLAY_Load(s32 id);
void GF_OVERLAY_Unload(s32 id);

/* Input and sleep (arm9/key.h, arm9/touchpanel.h, arm9/ui.h). */
u32 KEY_GetCont(void);
u32 KEY_GetTrg(void);
u32 TOUCHPANEL_GetPointCont(u32 *x, u32 *y);
void UI_SetKeepAwakeFlagsGlobal(u8 flags);
void UI_ClearKeepAwakeFlagsGlobal(u8 flags);
#define PAD_BUTTON_A 0x0001
#define PAD_BUTTON_B 0x0002
#define PAD_KEY_RIGHT 0x0010
#define PAD_KEY_LEFT 0x0020
#define PAD_KEY_UP 0x0040
#define PAD_KEY_DOWN 0x0080
#define PAD_BUTTON_X 0x0400

/* Sound (arm9/gfl_use.h). */
u16 GFL_USE_GetBgmNo(void);
void GFL_USE_CancelChangeAndPlayBgm(u32 bgmNo, u32 trackMask);
void GFL_USE_FadeInBgmFull(u32 frames);
void GFL_USE_FadeOutBgm(u32 frames);

/* Text (arm9/strbuf.h, arm9/gf_font.h, arm9/printsys.h). */
struct Strbuf *STRBUF_Create(u32 capacity, u32 heapId);
void STRBUF_Delete(struct Strbuf *buffer);
void STRBUF_SetString(struct Strbuf *buffer, const u16 *text);
struct GfFont *GF_FONT_Create(int archiveId, int fileId, int format, int isFixedWidth, u32 heapId);
u32 GF_FONT_Delete(struct GfFont *font);
void PRINTSYS_PrintColor(struct Bmp *bmp, u32 x, s16 y, struct Strbuf *text, struct GfFont *font, u16 color);
#define WT_PRINT_COLOR(letter, shadow, background) ((((letter) & 0x1f) << 10) | (((shadow) & 0x1f) << 5) | ((background) & 0x1f))

/* The master brightness (the SDK's GX). */
void GXx_SetMasterBrightness_(volatile u16 *reg, int brightness);

/* The screen as the Global Trade Station builds its own (ov135/worldtrade.c,
 * ov135/worldtrade_title.c): the BG system and its layers 0 to 3 on the main
 * engine and 4 to 7 on the sub engine (arm9/bg_sys.h, arm9/bg_sys_types.h),
 * windows in them (arm9/bmp_win.h, arm9/bmp_menuwork.h for the game's window
 * frame), graphics from archives (arm9/arc_tool.h, arm9/arc_util.h), a print
 * queue (arm9/printsys.h) and the task menu buttons (arm9/app_taskmenu.h).
 * The system's VBlank task runs BG_SYS_Update whenever the BG system exists
 * (GFL_USE_RunMainTasks). */
struct BmpWin;
struct PrintQueue;
struct TaskMenuRes;
struct TaskMenuWork;
typedef struct WtDispConfig {
    u32 dispMode, bgMode, subBgMode, bg0As;
} WtDispConfig;
typedef struct WtDispVram {
    u32 mainBg, mainBgExtPltt, subBg, subBgExtPltt, mainObj, mainObjExtPltt, subObj, subObjExtPltt, texImage,
        texPltt, mainObjMapping, subObjMapping;
} WtDispVram;
typedef struct WtBgTemplate {
    u32 scrollX, scrollY, bufSize, baseTile;
    u8 screenSize, colorMode, screenBase, charBase;
    u32 tileDataSize;
    u8 extPltt, priority, areaOver, pad;
    u32 mosaic;
} WtBgTemplate;
typedef char WtBgTemplateCheck[sizeof(WtBgTemplate) == 0x20 && sizeof(WtDispVram) == 0x30 ? 1 : -1];
#define WT_BG_SCREEN_256 1
#define WT_GX_VRAM_A 0x01
#define WT_GX_VRAM_C 0x04
#define WT_GX_OBJ_1D_32K 0x10
/* Palette kinds of ARC_UTIL_UploadPalette: the main and sub engines' BG palettes. */
#define WT_PALETTE_MAIN_BG 0
#define WT_PALETTE_SUB_BG 4
void BG_SYS_Create(u16 heapId);
void BG_SYS_Delete(void);
void BG_SYS_SetVramBanks(const WtDispVram *banks);
void BG_SYS_SetGraphicsMode(WtDispConfig *config);
void BG_SYS_InitBgLayer(u32 layer, const WtBgTemplate *bg, u32 type);
void BG_SYS_ClearSlot(u8 layer);
void BG_SYS_InitRun(u8 layer, u32 size, u32 offset, u32 heapId);
void BG_SYS_SetPlaneVisible(u8 layer, u8 visible);
void BG_SYS_SetMainVisiblePlanes(u32 planes);
void BG_SYS_SetSubVisiblePlanes(u32 planes);
void BG_SYS_PushTile(u32 layer);
void BG_SYS_MarkTileDirty(u32 layer);
void BMP_WIN_Init(u32 heapId);
void BMP_WIN_Exit(void);
struct BmpWin *BMP_WIN_Create(u32 layer, u32 x, u32 y, u8 width, u8 height, u8 palette, u8 allocDir);
u32 BMP_WIN_Delete(struct BmpWin *win);
void BMP_WIN_TransferChars(struct BmpWin *win);
void BMP_WIN_MakeScreen(struct BmpWin *win);
struct Bmp *BMP_WIN_GetBmp(struct BmpWin *win);
void BMP_Clear(struct Bmp *bmp, u8 value);
/* The window frame: its characters and palette, and drawn around a window (0 transfers the screen at once). */
void BMP_MENUWORK_LoadCharAndPltt(u32 layer, u32 tileOffset, u32 palette, u32 frameId, u16 heapId);
void BMP_MENUWORK_DrawWindow(struct BmpWin *win, int commitMode, u16 tileBase, u32 palette);
void *ARC_TOOL_CreateHandle(u32 archive, unsigned int heapId);
void ARC_TOOL_DeleteHandle(void *handle);
u32 ARC_UTIL_UploadBgCharFromHandle(void *handle, u32 member, u32 layer, u32 tileOffset, u32 size, u32 compressed,
                                    u16 heapId);
/* Despite aletheia's name, this loads a screen (NNS_G2dGetUnpackedScreenData) into a layer and transfers it. */
void ARC_UTIL_UploadBgPaletteFromHandle(void *handle, u32 member, u32 layer, u32 offset, u32 size, u32 compressed,
                                        u16 heapId);
void ARC_UTIL_UploadPaletteFromHandleWhole(void *handle, u32 member, int kind, u32 dest, int size, u16 heapId);
void ARC_UTIL_UploadPaletteFromArchiveWhole(u32 archive, u32 member, u32 kind, u32 dest, u32 size, u16 heapId);
struct PrintQueue *PRINTSYS_QUE_Create(u32 heapId);
void PRINTSYS_QUE_Delete(struct PrintQueue *queue);
u32 PRINTSYS_QUE_Main(struct PrintQueue *queue);
void PRINTSYS_QUE_Clear(struct PrintQueue *queue);
u32 PRINTSYS_MeasureWidestLine(struct Strbuf *text, struct GfFont *font, u32 spacing);
/* A task menu: one button per item, stacked from (x, y) in characters. */
typedef struct WtTaskMenuItem {
    struct Strbuf *str;
    u16 color;
    u32 type;
} WtTaskMenuItem;
typedef struct WtTaskMenuInit {
    u32 heapId;
    u8 count, pad[3];
    const WtTaskMenuItem *items;
    u32 posType;
    u8 x, y, width, height;
} WtTaskMenuInit;
typedef char WtTaskMenuCheck[sizeof(WtTaskMenuItem) == 0xC && sizeof(WtTaskMenuInit) == 0x14 ? 1 : -1];
/* The item that goes back: the return arrow, and the cancel sound. */
#define WT_TASKMENU_ITEM_RETURN 1
#define WT_TASKMENU_MAX_ITEMS 8
#define WT_SE_CANCEL 0x551
struct TaskMenuRes *APP_TASKMENU_RES_Create(u16 layer, u16 palette, struct GfFont *font, struct PrintQueue *queue,
                                            u16 heapId);
void APP_TASKMENU_RES_Delete(struct TaskMenuRes *res);
struct TaskMenuWork *APP_TASKMENU_OpenMenu(WtTaskMenuInit *init, struct TaskMenuRes *res);
void APP_TASKMENU_CloseMenu(struct TaskMenuWork *menu);
void APP_TASKMENU_UpdateMenu(struct TaskMenuWork *menu);
u32 APP_TASKMENU_IsDecide(struct TaskMenuWork *menu);
u32 APP_TASKMENU_IsFinish(struct TaskMenuWork *menu);
u8 APP_TASKMENU_GetCursorPos(const struct TaskMenuWork *menu);
void GFL_USE_PlaySeDefaultVolume(u32 seqNo);

/* Events, processes and the field (arm9/game_event.h, arm9/gamesystem.h, arm9/game_comm.h,
 * arm9/field_status.h; the fades are in overlay 21, which the field loads). */
struct GameEvent *GAME_EVENT_Create(struct GameSystem *gsys, struct GameEvent *parent, WtEventFunc func, u32 workSize);
void *GAME_EVENT_GetWork(struct GameEvent *event);
void GAME_EVENT_CallEvent(struct GameEvent *parent, struct GameEvent *child);
struct GameComm *GAMESYSTEM_GetGameComm(struct GameSystem *gsys);
u32 GAMESYSTEM_GetProcStatus(const struct GameSystem *gsys);
void GAMESYSTEM_CallProc(struct GameSystem *gsys, u32 overlayId, const WtProcData *proc, void *param);
struct FieldMap *GAMESYSTEM_GetFieldMap(struct GameSystem *gsys);
u32 GAME_COMM_GetKind(const struct GameComm *comm);
void GAME_COMM_RequestExitNoCallback(struct GameComm *comm);
struct GameEvent *FIELD_STATUS_CreateFieldCloseEvent(struct GameSystem *gsys, struct FieldMap *field);
struct GameEvent *FIELD_STATUS_CreateFieldOpenEvent(struct GameSystem *gsys);
struct GameEvent *EVENT_FIELDMAP_CONTROL_StartFadeOut(struct GameSystem *gsys, struct FieldMap *field, int effect,
                                                      int skipWait);
struct GameEvent *EVENT_FIELDMAP_CONTROL_StartFadeIn(struct GameSystem *gsys, struct FieldMap *field, int effect,
                                                     int skipWait, int restoreBg, u8 seasonShown, u8 seasonTarget);
void FIELDMAP_SetBgMode(void);
void FIELDMAP_ResetBgAffineShowMsg(struct FieldMap *field);

/* The save (arm9/save_control.c, arm9/backup_system.c, arm9/savedata.h). */
struct SaveControl *SAVE_CONTROL_Get(void);
int SAVE_CONTROL_StartSave(struct SaveControl *save);
u32 SAVE_CONTROL_MainSave(struct SaveControl *save);
#define WT_SAVE_CONTINUE 0
#define WT_SAVE_LAST 1
#define WT_SAVE_OK 2
u8 *BACKUP_SYSTEM_GetWork(struct BackupSaveData *sv, u32 *size);
void BACKUP_SYSTEM_FillBlockAreas(struct BackupSaveData *sv, u32 area);
u32 SAVEDATA_GetBlockOffset(struct SaveDataTable *table, u32 id);
u32 SAVEDATA_GetBlockSize(struct SaveDataTable *table, u32 id);
u32 SAVEDATA_GetTableMax(struct SaveDataTable *table);
/* SaveControl's main save (save_control.c) and BackupSaveData's save count and
 * block table (backup_system.c). */
#define WT_SAVE_CONTROL_MAIN_SAVE 0x10
#define WT_BACKUP_SAVE_COUNT 0x28
#define WT_BACKUP_TABLE 0x2C
/* The block written to both copies when a save writes every block (BLOCK_STATUS_WRITE_MAIN). */
#define WT_BLOCK_AREA_MAIN 1

int SYSTEM_DATA_IsMarked(void *save, u32 index);
void SYSTEM_DATA_ClearMark(void *save, u32 index);
struct MusicalSave *MUSICAL_SAVE_GetFromSave(struct SaveControl *save);
void MUSICAL_SAVE_SetHasDistData(struct MusicalSave *musical, u8 has);

/* Pokémon (arm9/poke_tool.h, arm9/poke_tool_ext.h, arm9/pokeparty.h, arm9/shinka_check.h, arm9/zukan_save.h). */
#define WT_PARTY_SIZE 6
typedef struct WtBoxPokemon {
    u32 data[0x22];
} WtBoxPokemon;
typedef struct WtPartyPokemon {
    WtBoxPokemon box;
    u8 battle[0xDC - 0x88];
} WtPartyPokemon;
typedef struct WtParty {
    s32 capacity;
    s32 count;
    WtPartyPokemon members[WT_PARTY_SIZE];
    u8 flags[4];
} WtParty;
typedef char WtPartySizeCheck[sizeof(WtParty) == 0x534 && sizeof(WtPartyPokemon) == 220 ? 1 : -1];

#define WT_PARAM_PID 0x00
#define WT_PARAM_SPECIES 0x05
#define WT_PARAM_ITEM 0x06
#define WT_PARAM_HAPPINESS 0x09
#define WT_PARAM_FORM 0x6F
#define WT_PARAM_NICKNAME_RAW 0x74
#define WT_PARAM_IS_NICKNAMED 0x75
#define WT_PARAM_OT_NAME_RAW 0x8E
#define WT_PARAM_OT_GENDER 0x9A
#define WT_PARAM_HP 0xA0
#define WT_PARAM_EGG_OR_BAD_EGG 0xAA
#define WT_PERSONAL_FRIENDSHIP 22
#define WT_EVO_CHECK_TRADE 1
#define WT_EVO_METHOD_TRADE_WITH_ITEM 6

u32 POKE_TOOL_GetBoxValue(WtBoxPokemon *pokemon, u32 id, u32 arg);
u8 POKE_TOOL_GetBoxGender(WtBoxPokemon *pokemon);
u32 POKE_TOOL_IsBoxShiny(WtBoxPokemon *pokemon);
void POKE_TOOL_RecoverBoxAllPP(WtBoxPokemon *pokemon);
u32 POKE_TOOL_GetPartyValue(WtPartyPokemon *pokemon, u32 id, u32 arg);
void POKE_TOOL_SetPartyValue(WtPartyPokemon *pokemon, u32 id, u32 arg);
void POKE_TOOL_ClearPartyParam(WtPartyPokemon *pokemon);
void POKE_TOOL_RecalcParam(WtPartyPokemon *pokemon);
void POKE_TOOL_HealFull(WtPartyPokemon *pokemon);
void POKE_TOOL_ChangeSpecies(WtPartyPokemon *pokemon, u32 species);
u32 POKE_TOOL_GetPersonalParam(u16 species, u16 form, u32 field);
u32 SHINKA_CHECK_CheckEvolution(WtParty *party, WtPartyPokemon *pokemon, u32 mode, u32 arg, u8 timezone,
                                u32 *method, u16 heapId);
u16 ZUKAN_SAVE_GetFormCount(u32 species);
void ZUKAN_SAVE_RegisterCaught(void *zukan, void *pokemon);
void ZUKAN_SAVE_RegisterSeen(void *zukan, void *pokemon);
WtParty *POKEPARTY_GetSaveData(struct SaveControl *save);

/* The party screen (overlay 91, ov091/pokelist.c) and the part of its
 * parameters (ov091/pokelist_types.h PokelistArg) a caller that picks one
 * Pokémon fills and reads: the party, the mode, and the slot picked. */
typedef struct WtPartyListArg {
    WtParty *party;
    u8 unused04[0x30];
    u32 mode;
    u32 unused38;
    s32 selectedIndex;
    u32 exitCode;
    u8 unused44[0x54];
} WtPartyListArg;
typedef char WtPartyListArgCheck[sizeof(WtPartyListArg) == 0x98 ? 1 : -1];
/* Takes the Pokémon chosen at once, with no menu (POKELIST_ApplySelection). */
#define WT_PARTY_LIST_SELECT 3
#define WT_PARTY_LIST_OVERLAY 91
u32 POKELIST_Init(void *proc, u32 *seq, void *procWork, void *work);
u32 POKELIST_Main(void *proc, u32 *seq, void *procWork, void *work);
u32 POKELIST_Exit(void *proc, u32 *seq, void *procWork, void *work);

/* The trade animation (overlay 119, ov119/pokemontrade_proc.c) as the Global
 * Trade Station plays it (ov135/worldtrade_demo.c), from a process system of
 * its own (arm9/procsys.h), with a partner made like the game's (arm9/mystatus.h). */
struct ProcSys;
struct MyStatus;
struct ProcSys *PROCSYS_Create(int heapId);
u32 PROCSYS_Main(struct ProcSys *sys);
void PROCSYS_Delete(struct ProcSys *sys);
void PROCSYS_Push(struct ProcSys *sys, u32 overlayId, const WtProcData *data, void *procWork);
struct GameData *GAMESYSTEM_GetGameData(struct GameSystem *gsys);
struct MyStatus *GAME_DATA_GetMyStatus(struct GameData *gameData);
struct MyStatus *MYSTATUS_Create(int heapId);
const u16 *MYSTATUS_GetName(const struct MyStatus *status);
void MYSTATUS_CopyName(u16 *dest, const u16 *src);
void MYSTATUS_SetSex(struct MyStatus *status, u32 sex);
u32 POKETRADE_PROC_StartRequest8(void *proc, u32 *seq, void *procWork, void *work);
u32 POKETRADE_PROC_Main(void *proc, u32 *seq, void *procWork, void *work);
u32 POKETRADE_PROC_Exit(void *proc, u32 *seq, void *procWork, void *work);
void GX_ResetBankForOBJ(void);
void GX_ResetBankForBGExtPltt(void);
void GX_ResetBankForOBJExtPltt(void);
void GX_ResetBankForTex(void);
void GX_ResetBankForTexPltt(void);
void GX_ResetBankForClearImage(void);
void GX_ResetBankForSubOBJ(void);
void GX_ResetBankForSubBGExtPltt(void);
void GX_ResetBankForSubOBJExtPltt(void);

/* Items (arm9/file_util.h): parameter 5 is the bag pocket. */
u32 FILE_UTIL_IsMailItem(u16 item);
u32 FILE_UTIL_GetItemParam(u32 item, u32 parameter, u32 heapId);
#define WT_ITEM_PARAM_POCKET 5
#define WT_POCKET_TMHM 2
#define WT_POCKET_KEY_ITEMS 4

#endif
