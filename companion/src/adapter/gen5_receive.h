#ifndef COMPANION_ADAPTER_GEN5_RECEIVE_H
#define COMPANION_ADAPTER_GEN5_RECEIVE_H

#include <stdint.h>

#include "adapter/platinum_receive.h"

/* A matched Wonder Trade delivery arriving in a Black/White or Black 2/White 2
 * save, as the patched game's ReceiveEffects (client/src/wonder_trade/bw/ and
 * bw2/ game_save.c) applies it with the game's own code (aletheia arm9
 * poke_tool.c, shinka_check.c, zukan_save.c; pokebw2 src/system poke_tool.c,
 * evolution_check.c, pokedex_savedata.c):
 * - the party Pokémon is built over 84 zero bytes of encrypted battle stats:
 *   its stats are calculated, it is healed, and its friendship becomes its
 *   species' base;
 * - it evolves as the game's trades check it, the Pokémon sent being the
 *   partner for Karrablast and Shelmet: an Everstone (hold effect 64) stops
 *   all but Kadabra; a trade holding an item drops the item; an evolution
 *   takes its new ability and gender, and the species' name (in the
 *   cartridge's language) when it has no nickname, then its stats again;
 * - it is registered in the Pokédex: Black/White register it as caught, with
 *   its language; Black 2/White 2's patched game registers it as seen only;
 * - a Pokémon going to the PC has every move's PP restored.
 * Every effect writes the bytes that code writes, so the companion's save and
 * the game's agree byte for byte. */

typedef enum {
    CP_RECEIVE_BLACK_WHITE = 0,
    CP_RECEIVE_BLACK2_WHITE2 = 1
} CpReceive5Game;

/* The Pokédex block (SAVE_BLOCK_POKEDEX) sizes. */
enum { CP_RECEIVE5_BW_POKEDEX_SIZE = 0x4D4, CP_RECEIVE5_B2W2_POKEDEX_SIZE = 0x4DC };

/* Build the received Pokémon from the canonical PK5 `incoming`, register it
 * in `pokedex` (the Pokédex block body) and write its encrypted image: 220
 * bytes for CP_RECEIVE_PARTY, the first 136 for CP_RECEIVE_BOX. `partner` is
 * the encrypted party image of the Pokémon sent. -1 refuses the delivery
 * (`pokedex` may then be partly changed: the caller puts it back). */
int CpReceive5_Apply(CpReceive5Game, const CpAdapterData *, unsigned char *pokedex,
                     const unsigned char incoming[136], const unsigned char partner[220], int destination,
                     unsigned char image[220]);

/* The forms the game's Pokédex keeps for a species (ZUKAN_SAVE_GetFormCount,
 * getNumberOfForms): 1 for any species it lists none for. */
unsigned CpReceive5_Forms(CpReceive5Game, unsigned species);

/* Pieces of the above, for tests. */
/* An empty party Pokémon (POKE_TOOL_ClearPartyParam): zeros encrypted with seed 0. */
void CpReceive5_Empty(unsigned char image[220]);
/* ZUKAN_SAVE_RegisterCaught / PokeDex_RegistPkm on a canonical PK5. */
int CpReceive5_Register(CpReceive5Game, const CpAdapterData *, unsigned char *pokedex, const unsigned char box[136]);

#endif
