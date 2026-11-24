#ifndef COMPANION_ADAPTER_ROM_DATA_H
#define COMPANION_ADAPTER_ROM_DATA_H

#include "adapter/platinum_receive.h"

/* The receive effects' game data (WtGame.data) from the player's ROM: `out`
 * reads `game`, which must stay open while it is used. */
void CpAdapter_RomData(CpAdapterData *out, const CpGame *game);

#endif
