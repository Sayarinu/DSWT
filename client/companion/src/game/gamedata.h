#ifndef COMPANION_GAME_GAMEDATA_H
#define COMPANION_GAME_GAMEDATA_H

#include "game/nitrofs.h"
#include "game/rom.h"

/* Generation IV and V game data, read from the player's own ROM. Nothing here
 * ships with the companion: the table below only names where each game keeps it. */

/* Species whose forms have their own personal record: form f (1..count) is
 * record firstIndex + f - 1. Other forms share their species' record. */
typedef struct CpGameForms {
    uint16_t species, firstIndex;
    uint8_t count;
} CpGameForms;

/* Item IDs first..last that have no data record (the game reads item 0's);
 * later IDs shift down over them. */
typedef struct CpItemGap {
    uint16_t first, last;
} CpItemGap;

/* A message bank's number in the archive. Japanese and Korean leave some
 * banks out, so later banks number lower there. */
typedef struct CpMessageBank {
    uint16_t international, japanese, korean;
} CpMessageBank;

/* Where one family keeps its data. */
typedef struct CpGameTable {
    CpFamily family;
    const char *personal, *growth, *evolution, *items, *messages, *font, *moves;
    /* Pearl's own personal archive (Diamond/Pearl), NULL for one shared by
     * the family's versions. */
    const char *personalPearl;
    CpMessageBank speciesNames, itemNames;
    /* itemCount is the highest item ID + 1. */
    uint16_t itemCount;
    const CpItemGap *itemGaps;
    uint8_t itemGapCount;
    const CpGameForms *forms;
    uint8_t formCount;
    /* Generation V: personal records of this many bytes, whose form records
     * the species' own record names (formFile at 0x1C, form count at 0x20);
     * Generation V messages and NFTR fonts; items 0..itemCount - 1 without
     * gaps, holdable by the game's own rule. 0 for Generation IV. */
    uint8_t gen5PersonalSize;
} CpGameTable;

/* The table for a family, NULL for one the companion does not read. */
const CpGameTable *CpGame_Table(CpFamily);

/* One open game: the archives the accessors read, located once. */
typedef struct CpGame {
    CpRomInfo rom;
    const CpGameTable *table;
    CpNarc personal, growth, evolution, items, messages, font, moves;
} CpGame;

/* Identify the ROM and open its archives. CP_ERR_UNSUPPORTED for a game
 * without a table. */
int CpGame_Open(const CpReader *rom, CpGame *out);

/* A message bank's number for the cartridge's language. */
uint16_t CpGame_Bank(const CpGame *, const CpMessageBank *);

/* Per-species, per-form data from the personal archive. */
typedef struct CpPersonal {
    uint8_t baseStats[6]; /* HP, Attack, Defense, Speed, Sp. Atk, Sp. Def */
    uint8_t genderRatio;  /* 0 always male, 254 always female, 255 genderless */
    uint8_t baseFriendship;
    uint8_t growthRate; /* the experience curve, an index for CpGame_ExpTable */
    uint8_t abilities[2];
    uint8_t hiddenAbility; /* Generation V; 0 in Generation IV */
} CpPersonal;

int CpGame_Personal(const CpGame *, uint16_t species, uint8_t form, CpPersonal *out);

/* The total experience for each level 0..100 of a growth rate. */
#define CP_MAX_LEVEL 100
int CpGame_ExpTable(const CpGame *, uint8_t growthRate, uint32_t out[CP_MAX_LEVEL + 1]);

/* The level that `exp` reaches on an experience table, 1..100. */
uint8_t CpExp_Level(const uint32_t table[CP_MAX_LEVEL + 1], uint32_t exp);

/* Evolution methods of the evolution archive that trading triggers. */
enum { CP_EVO_TRADE = 5, CP_EVO_TRADE_WITH_HELD_ITEM = 6 };

typedef struct CpTradeEvolution {
    uint16_t target; /* the species it becomes */
    uint16_t item;   /* the held item it needs, 0 for none */
} CpTradeEvolution;

/* The trade evolution a species holding `heldItem` (0 for none) takes:
 * CP_OK with the result, CP_ERR_NOT_FOUND when trading does not evolve it.
 * Whether an Everstone or similar stops it is the caller's rule. */
int CpGame_TradeEvolution(const CpGame *, uint16_t species, uint16_t heldItem, CpTradeEvolution *out);

/* The evolution archive's record of a species: seven {method, parameter,
 * target} entries; an entry with target 0 is unused. Methods 5 (trade) and
 * 6 (trade holding the parameter item) in both generations, and in
 * Generation V 7 (traded for Karrablast or Shelmet). */
enum { CP_EVO_TRADE_FOR_SPECIES = 7, CP_EVOLUTIONS = 7 };
typedef struct CpEvolution {
    uint16_t method, parameter, target;
} CpEvolution;
int CpGame_Evolutions(const CpGame *, uint16_t species, CpEvolution out[CP_EVOLUTIONS]);

/* What the item data says about an item. */
#define CP_POCKET_KEY_ITEMS 7
typedef struct CpItem {
    uint16_t price;
    uint8_t holdEffect;
    uint8_t fieldPocket; /* the bag pocket; CP_POCKET_KEY_ITEMS for key items */
    uint8_t preventToss;
    uint8_t holdable; /* a Pokémon may hold it: the game lets nothing it refuses to
                       * toss (key items, HMs) be held, as the client's Holdable does;
                       * Black/White: no TM, HM or key item; Black 2/White 2: no ID
                       * their PML_ItemIsUnholdable or PML_ItemIsDummy lists */
} CpItem;

/* CP_ERR_NOT_FOUND for item 0, an unused ID or one past the game's items. */
int CpGame_Item(const CpGame *, uint16_t item, CpItem *out);

/* A move's base PP from the move data (MoveTable_LoadParam's MOVEATTRIBUTE_PP,
 * byte 6; Generation V byte 5). */
int CpGame_MovePp(const CpGame *, uint16_t move, uint8_t *out);

#endif
