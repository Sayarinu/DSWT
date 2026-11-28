#ifndef COMPANION_GAME_GTS_MUSIC_H
#define COMPANION_GAME_GTS_MUSIC_H
#include <stdbool.h>
#include "game/rom.h"

/* The ARM7 references these aligned resources until it acknowledges stop. */
typedef struct CpGtsMusic {
    const uint8_t *sequence, *bank, *waves[4];
    uint32_t bytes;
    const int8_t *menuPcm; /* Menu commands supply a loop; an empty command requests silence. */
} CpGtsMusic;

int CpGtsMusic_Load(const CpReader *, CpFamily, CpGtsMusic *);
void CpGtsMusic_Free(CpGtsMusic *);
#endif
