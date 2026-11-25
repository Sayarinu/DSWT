#ifndef COMPANION_APP_MODEL_H
#define COMPANION_APP_MODEL_H

#include "adapter/platinum.h"
#include "adapter/rom_data.h"
#include "app/platform.h"
#include "game/gamedata.h"
#include "game/pairing.h"
#include "game/text.h"
#include "platform/journal_io.h"
#include "save/save_image.h"
#include "ui/model.h"
#include "wonder_trade/engine.h"

/* The screens' real model: CpUiOps over the saves on the SD card, the
 * player's ROMs, the Platinum adapter and the visit driver (session/visit.h).
 * Every op returns at once; scans, loads and visits run on the worker and
 * update() publishes their results, as WonderTradeNetwork does for the
 * patched game's application.c.
 *
 * Safety rules it keeps:
 * - the save is edited only by the adapter, through the engine or the boot
 *   check, with both retail copies written and verified;
 * - the first backup of a session is taken after enrollment and before the
 *   first step that may edit the save (backup_save.h); if it fails nothing
 *   is edited and the visit stops with CP_APP_ERROR_BACKUP;
 * - a matched delivery is applied with the game's receive effects, from the
 *   paired ROM's data (adapter/rom_data.h); one this game cannot hold is
 *   refused before anything is recorded and stays pending on the server. */

/* Support codes of the companion's own failures, outside the visit's. */
enum CpAppError {
    CP_APP_ERROR_WIFI = -20,      /* no saved connection worked */
    CP_APP_ERROR_WIFI_OFF = -21,  /* Wi-Fi did not start */
    CP_APP_ERROR_WORKER = -22,    /* the worker was busy or failed to start */
    CP_APP_ERROR_SAVE = -23,      /* the save could not be read or written again */
    CP_APP_ERROR_ENTROPY = -24,   /* the entropy pool failed */
    CP_APP_ERROR_BACKUP = -90     /* the backup failed: nothing was edited */
};

typedef struct CpAppSave {
    char path[CP_PATH_MAX], rom[CP_PATH_MAX];
    CpRomInfo info;
} CpAppSave;

typedef struct CpApp {
    CpUiModel model;
    const CpAppPlatform *platform;

    /* What the scan found, for model.saves. */
    CpAppSave saves[CP_UI_SAVES_MAX];

    /* The loaded save. */
    int loaded, saveOpen, romOpen, hasFont;
    unsigned index;
    CpReader romReader;
    CpPlatformFile platformFile, journalFile;
    CpFile file;
    CpJournalStore store;
    CpGame game;
    CpFont font;
    WtGame adapter;
    CpAdapterData romData;
    int needsProvision, backedUp;
    /* The last proven leave status (boot check or visit): kept across
     * reconnects, so only a later check can grant leave again. */
    int leaveProven;

    /* The session. */
    int job, connecting, closing;
    int slot;
    unsigned action;
    WtEngine engine;
    CpVisit visit;
    int visitOpened, visitCanLeave, needsEntropy;

    /* Work in progress on the worker, published by update(). */
    struct {
        unsigned saveCount;
        CpUiSave saves[CP_UI_SAVES_MAX];
        int loadMessage, recovery;
        unsigned phase, partyCount;
        CpUiPokemon party[CP_UI_PARTY_MAX];
        int hasReceived;
        CpUiReceived received;
    } result;
} CpApp;

/* Kept in static storage: the image alone is 512 KiB. */
extern CpSaveImage gCpAppImage;

void CpApp_Init(CpApp *, const CpAppPlatform *, CompanionLanguage systemLanguage);

/* The save's part of its backup folder name (<stem>-<id>): the file name
 * without .sav, cut to at most CP_APP_BACKUP_STEM_MAX bytes at a UTF-8
 * character boundary, characters FAT forbids (\ / : * ? " < > | and controls)
 * as '_', no trailing dots or spaces, "save" when nothing is left. Returns
 * its length. */
#define CP_APP_BACKUP_STEM_MAX 40
size_t CpApp_BackupStem(const char *name, char out[CP_APP_BACKUP_STEM_MAX + 1]);

#endif
