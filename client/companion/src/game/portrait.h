#ifndef COMPANION_GAME_PORTRAIT_H
#define COMPANION_GAME_PORTRAIT_H

#include "game/gamedata.h"

enum { CP_PORTRAIT_SIDE = 80, CP_PORTRAIT_TILES = 100 };
typedef struct CpPortrait {
    uint32_t ready;
    uint16_t palette[16];
    uint8_t tiles[CP_PORTRAIT_TILES * 32];
} CpPortrait;

int CpPortrait_Load(const CpGame *, unsigned species, unsigned form, int female,
                    int shiny, int egg, CpPortrait *out);

#endif
