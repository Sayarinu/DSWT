#include "game/pairing.h"

#include <ctype.h>
#include <string.h>

/* `directory` (with its trailing slash, or empty) + `stem` + ".nds". */
static int Join(char out[CP_PATH_MAX], const char *directory, size_t directoryLength, const char *stem,
    size_t stemLength)
{
    if (directoryLength + stemLength + sizeof ".nds" > CP_PATH_MAX) return 0;
    memcpy(out, directory, directoryLength);
    memcpy(out + directoryLength, stem, stemLength);
    memcpy(out + directoryLength + stemLength, ".nds", sizeof ".nds");
    return 1;
}

static int SameFolded(const char *a, const char *b, size_t length)
{
    size_t i;

    for (i = 0; i < length; ++i)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return 0;
    return 1;
}

size_t CpPair_RomCandidates(const char *savePath, char out[CP_PAIR_CANDIDATES][CP_PATH_MAX])
{
    size_t length = strlen(savePath), stemStart, folderStart, found = 0;
    const char *slash;

    if (length <= 4 || !SameFolded(savePath + length - 4, ".sav", 4)) return 0;
    slash = strrchr(savePath, '/');
    stemStart = slash ? (size_t)(slash - savePath) + 1 : 0;
    if (stemStart == length - 4) return 0; /* ".sav" alone */

    /* In a saves folder: the ROM is in that folder's parent. */
    if (stemStart > 0) {
        folderStart = stemStart - 1;
        while (folderStart > 0 && savePath[folderStart - 1] != '/') --folderStart;
        if (stemStart - 1 - folderStart == 5 && SameFolded(savePath + folderStart, "saves", 5)) {
            if (!Join(out[found], savePath, folderStart, savePath + stemStart, length - 4 - stemStart)) return 0;
            ++found;
        }
    }
    if (!Join(out[found], savePath, stemStart, savePath + stemStart, length - 4 - stemStart)) return 0;
    return found + 1;
}
