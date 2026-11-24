#ifndef COMPANION_ADAPTER_PLATINUM_H
#define COMPANION_ADAPTER_PLATINUM_H

#include "save/save_image.h"
#include "wonder_trade/game_adapter.h"

/* The companion's Platinum game adapter: WtGame_Inventory, WtGame_Checkpoint,
 * WtGame_CanLeave and WtGame_CanHold on a save file instead of the game's
 * RAM. Each step changes the save exactly as the patched game's
 * client/src/wonder_trade/platinum/game_save.c does, so a save can move
 * between the companion and the patched game at any point in a trade.
 *
 * The loaded image stands in for the game's live save. A step edits it only
 * once its result is proven, then writes both retail copies through the file
 * and verifies them. After a failed write the image is unloaded (save_image.h):
 * load the file again before anything else.
 *
 * Received deliveries of a matched trade (LUNE-873) are refused for now; a
 * returned offer is put back exactly as the game does. */
struct WtGame {
    CpSaveImage *image;
    const CpFile *file;
    /* Nonzero when a Pokémon may hold an existing item: CpAdapter_RomHoldable
     * (adapter/holdable.h) with the player's CpGame. NULL refuses every held item. */
    int (*holdable)(void *arg, unsigned item);
    void *holdableArg;
};

/* Party layout and limits of the game, as game_save.c uses them. */
enum {
    CP_ADAPTER_PARTY_SIZE = 8 + CP_SAVE_PARTY_CAPACITY * CP_SAVE_PARTY_STRIDE,
    CP_ADAPTER_MAX_SPECIES = 493, /* NATIONAL_DEX_COUNT, Arceus */
    CP_ADAPTER_MAX_ITEM = 467,    /* NUM_ITEMS, the Secret Key */
    CP_ADAPTER_MAX_BALL = 16,     /* ITEM_CHERISH_BALL */
    CP_ADAPTER_FIRST_MAIL = 137,  /* ITEM_GRASS_MAIL */
    CP_ADAPTER_LAST_MAIL = 148    /* ITEM_BRICK_MAIL */
};

/* The offer screen's checks, as the game's WtGameSave_Eligible and
 * WtGameSave_Unique: nonzero when the party slot may be offered, and when
 * no other copy of it would stay in the party or PC. */
int CpAdapter_Eligible(const CpSaveImage *, unsigned slot);
/* Leaves the image as it found it. Nonzero too when the slot cannot be read:
 * the offer's own checks refuse it then. */
int CpAdapter_Unique(CpSaveImage *, unsigned slot);

/* The form count the game keeps for a species (Pokemon_SanitizeFormId). */
unsigned CpAdapter_Forms(unsigned species);

#endif
