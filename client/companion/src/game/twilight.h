#ifndef COMPANION_GAME_TWILIGHT_H
#define COMPANION_GAME_TWILIGHT_H

#include "game/pairing.h"

#define CP_TWL_SETTINGS "_nds/TWiLightMenu/settings.ini"
enum { CP_TWL_SAVES_FOLDER, CP_TWL_GAME_FOLDER, CP_TWL_MENU_FOLDER };

/* Reads an integer from a TWiLight INI. A missing file/key uses fallback.
 * Unreadable files or malformed values return -1 to avoid selecting another save. */
int CpTwilight_Setting(const char *path, const char *section, const char *key, int fallback);

/* directory and root include their trailing slash; rom is the full basename. */
int CpTwilight_SavePath(const char *root, const char *directory, const char *rom,
    int location, char path[CP_PATH_MAX]);

#endif
