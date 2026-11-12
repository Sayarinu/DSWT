#ifndef WONDER_TRADE_POKEMON_CODEC_H
#define WONDER_TRADE_POKEMON_CODEC_H

#include <stddef.h>

/* Encrypted, shuffled retail box image <-> canonical unencrypted PK4.
 * Neither function repairs a checksum, sanity flag or malformed input. */
int WtPokemon_Decode(const unsigned char[136], unsigned char[136]);
int WtPokemon_Encode(const unsigned char[136], unsigned char[136]);

/* What one game can hold, as its adapter describes it to WtPokemon_Fits. */
typedef struct WtPokemonLimits {
    unsigned maxSpecies;  /* National Dex numbers 1..maxSpecies exist */
    unsigned maxItem;     /* item IDs 1..maxItem exist */
    unsigned maxBall;     /* D/P/Pt ball byte (0x83) values 1..maxBall exist */
    /* Forms the game has for an existing species; 1 when it has no others. */
    unsigned (*forms)(unsigned species);
    /* Nonzero when a Pokémon in this game may hold an existing item. */
    int (*holdable)(void *arg, unsigned item);
    void *arg;
} WtPokemonLimits;

/* Nonzero when a canonical PK4 is well formed and its species, form, held item
 * and ball all exist in the described game, with the item one it lets be held. */
int WtPokemon_Fits(const unsigned char[136], const WtPokemonLimits *);

#endif
