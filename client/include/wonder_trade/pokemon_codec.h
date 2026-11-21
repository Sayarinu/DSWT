#ifndef WONDER_TRADE_POKEMON_CODEC_H
#define WONDER_TRADE_POKEMON_CODEC_H

#include <stddef.h>

/* A stored (box) Pokémon: PK4 in Generation IV, PK5 in Generation V. Both are
 * 136 bytes with the same header, checksum, block shuffle and encryption
 * (PKHeX PokeCrypto: SIZE_4STORED and SIZE_5STORED, DecryptArray45). A party
 * Pokémon adds encrypted battle stats: 236 bytes for PK4, 220 for PK5. */
#define WT_POKEMON_SIZE 136
#define WT_POKEMON_PK4_PARTY_SIZE 236
#define WT_POKEMON_PK5_PARTY_SIZE 220

/* Encrypted, shuffled retail box image <-> canonical unencrypted PK4 or PK5.
 * Neither function repairs a checksum, sanity flag or malformed input. */
int WtPokemon_Decode(const unsigned char[WT_POKEMON_SIZE], unsigned char[WT_POKEMON_SIZE]);
int WtPokemon_Encode(const unsigned char[WT_POKEMON_SIZE], unsigned char[WT_POKEMON_SIZE]);

/* What one game can hold, as its adapter describes it to WtPokemon_Fits. */
typedef struct WtPokemonLimits {
    unsigned maxSpecies;  /* National Dex numbers 1..maxSpecies exist */
    unsigned maxItem;     /* item IDs 1..maxItem exist */
    unsigned maxBall;     /* ball byte (0x83 in D/P/Pt PK4 and in PK5) values 1..maxBall exist */
    /* Forms the game has for an existing species; 1 when it has no others. */
    unsigned (*forms)(unsigned species);
    /* Nonzero when a Pokémon in this game may hold an existing item. */
    int (*holdable)(void *arg, unsigned item);
    void *arg;
} WtPokemonLimits;

/* Nonzero when a canonical PK4 or PK5 is well formed and its species, form,
 * held item and ball all exist in the described game, with the item one it
 * lets be held. Both formats keep these fields at the same offsets. */
int WtPokemon_Fits(const unsigned char[WT_POKEMON_SIZE], const WtPokemonLimits *);

#endif
