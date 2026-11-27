#include "game/twilight.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *Trim(char *text)
{
    while (isspace((unsigned char)*text)) ++text;
    size_t end = strlen(text);
    while (end && isspace((unsigned char)text[end - 1])) text[--end] = 0;
    return text;
}

int CpTwilight_Setting(const char *path, const char *section, const char *key, int fallback)
{
    FILE *file = fopen(path, "r");
    if (!file) return errno == ENOENT ? fallback : -1;
    char line[512];
    int first = 1, match = 0, result = fallback;
    while (fgets(line, sizeof(line), file)) {
        char *start = line;
        if (first && !strncmp(start, "\xEF\xBB\xBF", 3)) start += 3;
        first = 0;
        if (!strchr(line, '\n') && !feof(file)) {
            int c;
            while ((c = fgetc(file)) != EOF && c != '\n') { }
            continue;
        }
        char *value = Trim(start);
        if (*value == '[') {
            if (match) break;
            char *end = strchr(value, ']');
            if (end) *end = 0;
            match = end && !strcmp(value + 1, section);
        } else if (match) {
            char *equals = strchr(value, '=');
            if (!equals) continue;
            *equals++ = 0;
            if (strcmp(Trim(value), key)) continue;
            equals = Trim(equals);
            char *end;
            errno = 0;
            int hex = equals[0] == '0' && (equals[1] == 'x' || equals[1] == 'X');
            long number = strtol(equals, &end, hex ? 16 : 10);
            result = !errno && end != equals && !*Trim(end) && number >= 0 && number <= INT_MAX ? (int)number : -1;
            break;
        }
    }
    if (ferror(file)) result = -1;
    fclose(file);
    return result;
}

int CpTwilight_SavePath(const char *root, const char *directory, const char *rom,
    int location, char path[CP_PATH_MAX])
{
    const char *ext = strrchr(rom, '.');
    int slot = 0;
    if (!ext || ext == rom || location < 0 || location > CP_TWL_MENU_FOLDER) return 0;
    if (location != CP_TWL_GAME_FOLDER) {
        char ini[CP_PATH_MAX];
        int length = snprintf(ini, sizeof(ini), "%s_nds/TWiLightMenu/gamesettings/%s.ini", root, rom);
        if (length < 0 || (size_t)length >= sizeof(ini)) return 0;
        slot = CpTwilight_Setting(ini, "GAMESETTINGS", "SAVE_NUMBER", 0);
        if (slot < 0 || slot > 9) return 0;
    }
    const char *folder = location == CP_TWL_MENU_FOLDER ? "_nds/TWiLightMenu/saves/" :
        location == CP_TWL_SAVES_FOLDER ? "saves/" : "";
    char suffix[2] = { slot ? (char)('0' + slot) : 0, 0 };
    int length = snprintf(path, CP_PATH_MAX, "%s%s%.*s.sav%s",
        location == CP_TWL_MENU_FOLDER ? root : directory, folder, (int)(ext - rom), rom, suffix);
    return length >= 0 && length < CP_PATH_MAX;
}
