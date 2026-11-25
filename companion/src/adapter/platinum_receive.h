#ifndef COMPANION_ADAPTER_PLATINUM_RECEIVE_H
#define COMPANION_ADAPTER_PLATINUM_RECEIVE_H

#include <stddef.h>
#include <stdint.h>

#include "game/gamedata.h"

/* A matched Wonder Trade delivery arriving in a Generation IV save, as the
 * patched game's ReceiveEffects and BuildReceived (client/src/wonder_trade/
 * platinum/, dp/ and hgss/ game_save.c) apply it with the game's own Pokémon,
 * Pokédex, records, Pokétch and journal code. Every effect here writes the
 * bytes that code writes, so the companion's save and the game's agree byte
 * for byte. The CP_RECEIVE_*_OFFSET without a game are Platinum's. */

/* What those effects read from the player's ROM. Each returns CP_OK, or
 * CP_ERR_NOT_FOUND where noted; anything else refuses the delivery.
 * CpAdapter_RomData (adapter/rom_data.h) reads them from a CpGame. */
typedef struct CpAdapterData {
    void *arg;
    int (*personal)(void *arg, uint16_t species, uint8_t form, CpPersonal *out);
    int (*expTable)(void *arg, uint8_t growthRate, uint32_t out[CP_MAX_LEVEL + 1]);
    /* CP_ERR_NOT_FOUND when trading does not evolve the species holding heldItem. */
    int (*tradeEvolution)(void *arg, uint16_t species, uint16_t heldItem, CpTradeEvolution *out);
    /* CP_ERR_NOT_FOUND for an item without a record (and item 0): hold effect 0. */
    int (*holdEffect)(void *arg, uint16_t item, uint8_t *out);
    int (*movePp)(void *arg, uint16_t move, uint8_t *out);
    /* As CpGame_SpeciesName: capacity counts the terminator. */
    int (*speciesName)(void *arg, uint16_t species, uint16_t *out, size_t capacity, size_t *length);
    /* The cartridge's language (gGameLanguage), which new mail records. */
    uint8_t language;
} CpAdapterData;

enum { CP_RECEIVE_PARTY = 1, CP_RECEIVE_BOX = 2 };

/* The game whose receive effects apply. Diamond/Pearl use the GTS's
 * (client/src/wonder_trade/dp/game_save.c ReceiveEffects: catch records,
 * Pokédex and Pokétch history, nothing else); HeartGold/SoulSilver theirs
 * (hgss/game_save.c: Pokédex, game stats, the Pokéwalker's Jirachi course). */
typedef enum {
    CP_RECEIVE_PLATINUM = 0,
    CP_RECEIVE_DIAMOND_PEARL,
    CP_RECEIVE_HEARTGOLD_SOULSILVER
} CpReceiveGame;

/* Offsets in the general block body (the game's save table). */
enum {
    CP_RECEIVE_VARS_OFFSET = 0x0DAC,
    CP_RECEIVE_POKETCH_OFFSET = 0x1160,
    CP_RECEIVE_POKEDEX_OFFSET = 0x1328,
    CP_RECEIVE_JOURNAL_OFFSET = 0x5720,
    CP_RECEIVE_RECORDS_OFFSET = 0x61B0
};

/* Build the received Pokémon from the canonical PK4 `incoming`, apply its
 * effects to `general` (the general block body) and write its encrypted image:
 * 236 bytes for CP_RECEIVE_PARTY, the first 136 for CP_RECEIVE_BOX.
 * `keptSentences` is NULL when preparing; when the delivery is applied it holds
 * the three mail sentence IDs of the prepared image, which the game keeps
 * because its mail constructor leaves them unset. On -1 `general` may be
 * partly changed: the caller puts it back. */
int CpReceive_Apply(const CpAdapterData *, unsigned char *general, const unsigned char incoming[136],
                    int destination, const uint16_t *keptSentences, unsigned char image[236]);
/* The same for any game. version is the cartridge's (gGameVersion), which
 * new mail records; Platinum's is always 12. Diamond/Pearl's mail has no
 * unset sentence IDs: keptSentences is ignored there. */
int CpReceive_ApplyGame(CpReceiveGame, uint8_t version, const CpAdapterData *, unsigned char *general,
                        const unsigned char incoming[136], int destination, const uint16_t *keptSentences,
                        unsigned char image[236]);

/* Offsets in the Diamond/Pearl general block body (pokediamond save_arrays.c). */
enum {
    CP_RECEIVE_DP_POKETCH_OFFSET = 0x114C,
    CP_RECEIVE_DP_POKEDEX_OFFSET = 0x12DC,
    CP_RECEIVE_DP_STATS_OFFSET = 0x5F08
};

/* Offsets in the HeartGold/SoulSilver general block body (pokeheartgold
 * save_arrays.c). */
enum {
    CP_RECEIVE_HGSS_POKEDEX_OFFSET = 0x12B8,
    CP_RECEIVE_HGSS_STATS_OFFSET = 0x4B3C,
    CP_RECEIVE_HGSS_WALKER_OFFSET = 0xE5DC
};

/* Pieces of the above, for tests. `box` is a canonical PK4, `party` its
 * decrypted 100-byte party extension. */

/* Pokemon_CalcLevelAndStats: the level from the experience, then the stats,
 * with the current HP moved by the change in maximum HP. */
int CpReceive_LevelAndStats(const CpAdapterData *, const unsigned char box[136], unsigned char party[100]);
/* SpeciesData_GetGenderOf: 0 male, 1 female, 2 genderless. */
uint8_t CpReceive_Gender(uint8_t genderRatio, uint32_t personality);
/* Pokedex_Capture on the Pokédex save block. */
void CpReceive_DexCapture(unsigned char *pokedex, uint16_t species, uint8_t form, uint8_t gender,
                          uint32_t personality, uint8_t language);

#endif
