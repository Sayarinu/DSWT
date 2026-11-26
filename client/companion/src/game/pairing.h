#ifndef COMPANION_GAME_PAIRING_H
#define COMPANION_GAME_PAIRING_H

#include <stddef.h>

/* The ROM that goes with a save: TWiLight Menu++ keeps X.sav for X.nds in a
 * saves folder beside the ROM (roms/nds/saves/X.sav for roms/nds/X.nds), or
 * next to the ROM itself. */
#define CP_PAIR_CANDIDATES 2
#define CP_PATH_MAX 256

/* Write the ROM paths to try for a .sav, most likely first. Returns how many
 * were written, 0 when the path does not end in .sav or a path is too long. */
size_t CpPair_RomCandidates(const char *savePath, char out[CP_PAIR_CANDIDATES][CP_PATH_MAX]);

#endif
