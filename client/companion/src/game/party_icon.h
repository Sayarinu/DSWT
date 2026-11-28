#ifndef COMPANION_GAME_PARTY_ICON_H
#define COMPANION_GAME_PARTY_ICON_H
#include "game/gamedata.h"

typedef struct CpPartyIcons {
    CpNarc archive;
    uint8_t paletteIndex[768];
    unsigned count;
} CpPartyIcons;
typedef struct CpPartyIcon {
    uint16_t palette[16];
    uint8_t tiles[512]; /* first frame, sixteen 4bpp tiles */
    int ready;
} CpPartyIcon;

void CpPartyIcons_Open(CpPartyIcons *, const CpGame *);
int CpPartyIcon_Load(const CpPartyIcons *, const CpGame *, unsigned species, unsigned form,
                     int female, int egg, CpPartyIcon *);
#endif
