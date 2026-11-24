#ifndef COMPANION_GAME_NITROFS_H
#define COMPANION_GAME_NITROFS_H

#include "game/reader.h"

/* NitroFS, the file system inside a DS ROM, and NARC, the archives the games
 * keep most data in. Lookups read only the table entries they walk, a few
 * bytes at a time, so nothing loads a whole ROM or archive into memory. */

/* Find a file by its NitroFS path, such as "poketool/personal/evo.narc". */
int CpNitroFs_Open(const CpReader *rom, const char *path, CpRomFile *out);

/* A NARC: its member table (BTAF) and data (GMIF) within the file. */
typedef struct CpNarc {
    CpRomFile file;
    uint32_t fatOffset, dataOffset, dataSize;
    uint16_t count;
} CpNarc;

int CpNarc_Open(const CpRomFile *file, CpNarc *out);

/* Member `index` as a byte range of the ROM. */
int CpNarc_Member(const CpNarc *, uint32_t index, CpRomFile *out);

/* Find a NitroFS file and open it as a NARC. */
int CpNarc_OpenPath(const CpReader *rom, const char *path, CpNarc *out);

#endif
