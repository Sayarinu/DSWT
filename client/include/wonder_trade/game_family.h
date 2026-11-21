#ifndef WONDER_TRADE_GAME_FAMILY_H
#define WONDER_TRADE_GAME_FAMILY_H

#include "wonder_trade/pokemon_codec.h"

/* The game family one build of the core serves. Each ROM build compiles the
 * core for a single family; Generation IV builds need not define WT_GAME_FAMILY.
 * Save values follow protocol/save-layout*.json. */
#define WT_FAMILY_GEN4 4 /* Diamond, Pearl, Platinum, HeartGold and SoulSilver */
#define WT_FAMILY_BW 5   /* Black and White */
#define WT_FAMILY_B2W2 6 /* Black 2 and White 2 */

#ifndef WT_GAME_FAMILY
#define WT_GAME_FAMILY WT_FAMILY_GEN4
#endif

/* WT_GAME_PARTY_SIZE: bytes of one party Pokémon, the box image and its battle stats.
 * WT_GAME_BOX_COUNT: PC boxes of WT_GAME_BOX_SLOTS each.
 * WT_GAME_SAVE_MAGIC: the international save footer magic.
 * WT_GAME_BANK_RESERVED_SIZE: flash reserved from each journal bank's start;
 * every byte after the 4 KiB record must stay blank. */
#if WT_GAME_FAMILY == WT_FAMILY_GEN4
#define WT_GAME_PARTY_SIZE WT_POKEMON_PK4_PARTY_SIZE
#define WT_GAME_BOX_COUNT 18
#define WT_GAME_SAVE_MAGIC 0x20060623
#define WT_GAME_BANK_RESERVED_SIZE 0x10000
#ifndef WT_PERSISTENCE_ADDRESS
#define WT_PERSISTENCE_ADDRESS WT_PERSISTENCE_ADDRESS_GEN4
#endif
#elif WT_GAME_FAMILY == WT_FAMILY_BW || WT_GAME_FAMILY == WT_FAMILY_B2W2
#define WT_GAME_PARTY_SIZE WT_POKEMON_PK5_PARTY_SIZE
#define WT_GAME_BOX_COUNT 24
#define WT_GAME_SAVE_MAGIC 0x31053527
/* The two banks are adjacent 4 KiB in the musical distribution area. */
#define WT_GAME_BANK_RESERVED_SIZE 0x1000
#ifndef WT_PERSISTENCE_ADDRESS
#if WT_GAME_FAMILY == WT_FAMILY_BW
#define WT_PERSISTENCE_ADDRESS WT_PERSISTENCE_ADDRESS_BW
#else
#define WT_PERSISTENCE_ADDRESS WT_PERSISTENCE_ADDRESS_B2W2
#endif
#endif
#else
#error "WT_GAME_FAMILY must be WT_FAMILY_GEN4, WT_FAMILY_BW or WT_FAMILY_B2W2"
#endif
#define WT_GAME_BOX_SLOTS 30

#include "wonder_trade/persistence.h"

#endif
