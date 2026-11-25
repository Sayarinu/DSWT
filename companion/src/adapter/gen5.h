#ifndef COMPANION_ADAPTER_GEN5_H
#define COMPANION_ADAPTER_GEN5_H

#include "adapter/platinum.h"

/* The companion's Generation V game adapter: Black/White and Black 2/White 2
 * saves (CpSaveLayout_BlackWhite, _Black2White2), behind the same
 * WtGame_Inventory, WtGame_CanLeave and WtGame_CanHold as the Generation IV
 * adapter (adapter/platinum.c hands a Generation V image to these). Each step
 * changes the save exactly as the patched game's client/src/wonder_trade/bw/
 * and bw2/ game_save.c do, under the same rules as adapter/platinum.h:
 * - the party (block 0x1A: capacity, count, six 220-byte Pokémon) gives the
 *   offer; the PC (24 trays of 30, blocks 1..24) and the Battle Box (block
 *   0x31) must then hold no copy of it;
 * - a delivery goes to the end of the party, or the first empty PC slot,
 *   with the receive effects of adapter/gen5_receive.h (the Pokédex as the
 *   retail games' trades register it);
 * - an older save is judged by its one save count, which the journal records
 *   in all three counters;
 * - between companion sessions the stock game may have saved: REMOVE and
 *   RECEIVE then decide by the Pokémon's exact image (LUNE-899). A delivery
 *   the stock game moved to the Battle Box is recorded there (journal locator
 *   kind 3), as the game's WtDelivery_Found records it; nothing is ever
 *   delivered to the Battle Box, and an offer moved there is refused.
 *
 * The journal banks lie in the musical distribution save
 * (save/musical_area.h), released before the journal's first write. */

int CpGen5_Inventory(WtGame *, enum WtInventoryStep, WtJournalEntry *, unsigned slot);
int CpGen5_CanLeave(WtGame *, const WtJournalEntry *);
int CpGen5_CanHold(const WtGame *, const unsigned char pokemon[WT_POKEMON_SIZE]);
int CpGen5_Eligible(const CpSaveImage *, unsigned slot);
int CpGen5_Unique(CpSaveImage *, unsigned slot);

/* Why the offer screen refuses a party slot, in the game's order. */
enum { CP_GEN5_OK = 0, CP_GEN5_EGG, CP_GEN5_MAIL, CP_GEN5_LAST_USABLE, CP_GEN5_OTHER };
int CpGen5_Reason(const CpSaveImage *, unsigned slot);

/* The patched game's WtGameSave_ReleaseMusicalArea, before the journal is
 * created or used: zero once the save the game loads marks no musical as
 * saved and the journal banks hold the journal or nothing. A marker left by a
 * failed musical load is cleared in both copies first (a checkpoint), then a
 * musical's leftovers are erased. CP_GEN5_MUSICAL_SAVED when the save holds a
 * downloaded musical the game would still load: the player would lose it,
 * so the companion refuses the save and changes nothing. -1 on any failure. */
enum { CP_GEN5_MUSICAL_SAVED = 1 };
int CpGen5_ReleaseMusical(WtGame *);
/* Nonzero when the loaded save still marks a loadable musical as saved. */
int CpGen5_MusicalSaved(const CpSaveImage *);

/* A save of either Generation V layout. */
int CpGen5_Is(const CpSaveImage *);

#endif
