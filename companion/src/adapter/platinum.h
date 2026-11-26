#ifndef COMPANION_ADAPTER_PLATINUM_H
#define COMPANION_ADAPTER_PLATINUM_H

/* save_image.h first: game/rom.h keeps its save magic definitions. */
#include "save/save_image.h"
#include "adapter/platinum_receive.h"
#include "wonder_trade/game_adapter.h"

/* The companion's Generation IV game adapter: WtGame_Inventory,
 * WtGame_Checkpoint, WtGame_CanLeave and WtGame_CanHold on a save file
 * instead of the game's RAM. Each step changes the save exactly as the
 * patched game's client/src/wonder_trade/<game>/game_save.c does, so a save
 * can move between the companion and the patched game at any point in a trade.
 *
 * One adapter serves Platinum, Diamond/Pearl and HeartGold/SoulSilver: the
 * image's layout (CpSaveImage_LoadLayout) picks the game, and with it what
 * differs between their game_save.c:
 * - where the party and PC are (save_image.h);
 * - Diamond/Pearl and HeartGold/SoulSilver judge an older save by their
 *   counters only (Diamond/Pearl's box counter in the journal is the game's
 *   RAM one, which its flash need not hold yet);
 * - the receive effects (adapter/platinum_receive.h CpReceiveGame);
 * - HeartGold/SoulSilver keep Aprijuice modifiers beside the party, which
 *   move with it, block an offer, and are cleared for a delivery; they never
 *   offer Spiky-eared Pichu; their 0x86 ball must be one they have;
 * - the forms and the last item each game knows (WtGame_CanHold).
 *
 * The loaded image stands in for the game's live save. A step edits it only
 * once its result is proven, then writes both retail copies through the file
 * and verifies them. After a failed write the image is unloaded (save_image.h):
 * load the file again before anything else.
 *
 * A returned offer is put back exactly as the game does. A matched delivery
 * goes where the game puts it, with the game's effects on the Pokémon, the
 * Pokédex, the records, the Pokétch and the journal (adapter/platinum_receive.h),
 * read from the player's ROM through `data`.
 *
 * Between companion sessions the player may load and save the save in the
 * stock game before an interrupted step could resume, so the save matches
 * neither inventory the journal recorded. REMOVE then decides by the
 * offer's exact image: still in the party exactly as offered, it is removed
 * from this save; already gone, it is removed. A Pokémon that shares the
 * identity (personality and trainer ID) without being that image (another
 * Pokémon, or this one trained in the stock game) is never proof either way:
 * the step fails closed and changes nothing.
 *
 * RECEIVE follows the games' rule (client wonder_trade/delivery.h,
 * WtDelivery_Recover; LUNE-906): a delivery or returned offer is never added
 * to a save another game wrote since, because a save where it never arrived
 * and one where the player released it, traded it away or moved it look the
 * same. The save is accepted as it is. The one Pokémon with the delivery's
 * identity, in any state play left it, is recorded where it is (the ACK
 * reports its digest); with none, nothing is added. Two with the identity
 * (a Pokémon with a fixed personality and trainer ID can meet another copy
 * of itself, LUNE-914) refuse the step: it never picks one. The cost, as in
 * the games: a cut after the journal recorded the delivery and before the
 * first retail copy holding it was complete, followed by a stock-game save
 * before the companion resumes, loses that Pokémon.
 *
 * The stock game can also move a Pokémon where no journal locator reaches:
 * the Day Care, the GTS deposit, the HeartGold/SoulSilver Pokéwalker (and
 * Pal Park's migrated Pokémon are held there too). A Pokémon there with the
 * identity a step decides by is never proof either way (LUNE-913): the step
 * fails closed and writes nothing until the player takes it back. Deliveries
 * still go only to the party or the PC.
 *
 * A Black/White or Black 2/White 2 image goes to adapter/gen5.h instead. */
struct WtGame {
    CpSaveImage *image;
    const CpFile *file;
    /* Nonzero when a Pokémon may hold an existing item: CpAdapter_RomHoldable
     * (adapter/holdable.h) with the player's CpGame. NULL refuses every held item. */
    int (*holdable)(void *arg, unsigned item);
    void *holdableArg;
    /* The player's game data for matched deliveries: CpAdapter_RomData
     * (adapter/rom_data.h). NULL refuses them; returned offers need none. */
    const CpAdapterData *data;
    /* The cartridge's version (CpRomInfo.version), which Diamond/Pearl and
     * HeartGold/SoulSilver deliveries' new mail records. */
    uint8_t version;
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
