#ifndef COMPANION_ADAPTER_HOLDABLE_H
#define COMPANION_ADAPTER_HOLDABLE_H

/* For WtGame.holdable (adapter/platinum.h): the item data of the player's ROM,
 * arg a const CpGame *. As the game's bag rule, an item it will not let be
 * tossed (key items, HMs) cannot be held. IDs without a data record are
 * refused, where the game would read item 0's record. */
int CpAdapter_RomHoldable(void *game, unsigned item);

#endif
