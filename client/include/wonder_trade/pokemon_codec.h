#ifndef WONDER_TRADE_POKEMON_CODEC_H
#define WONDER_TRADE_POKEMON_CODEC_H

#include <stddef.h>

/* Encrypted, shuffled retail box image <-> canonical unencrypted PK4.
 * Neither function repairs a checksum, sanity flag or malformed input. */
int WtPokemon_Decode(const unsigned char[136], unsigned char[136]);
int WtPokemon_Encode(const unsigned char[136], unsigned char[136]);

#endif
