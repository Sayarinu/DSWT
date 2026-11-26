#include "wonder_trade/pokemon_codec.h"

#include "wonder_trade/wire_layout.h"

#include <stdint.h>
#include <string.h>

enum {
    BLOCK_COUNT = 4, BLOCK_BYTES = 32, PERMUTATION_COUNT = 24,
    PID_SHUFFLE_SHIFT = 13, PID_SHUFFLE_MASK = 31,
    ITEM_OFFSET = 0x0A, FORM_OFFSET = 0x40, FORM_SHIFT = 3, BALL_OFFSET = 0x83
};

#define WT_LCG_MULTIPLIER UINT32_C(0x41c64e6d)
#define WT_LCG_INCREMENT UINT32_C(0x6073)

static unsigned Read16(const unsigned char *p) { return p[0] | ((unsigned)p[1] << 8); }
static uint32_t Read32(const unsigned char *p) { return Read16(p) | ((uint32_t)Read16(p + 2) << 16); }

static void Order(unsigned pid, unsigned char order[BLOCK_COUNT])
{
    unsigned char available[BLOCK_COUNT] = { 0, 1, 2, 3 };
    const unsigned factorial[BLOCK_COUNT] = { 6, 2, 1, 1 };
    unsigned rank = ((pid >> PID_SHUFFLE_SHIFT) & PID_SHUFFLE_MASK) % PERMUTATION_COUNT, i, j, chosen;
    for (i = 0; i < BLOCK_COUNT; ++i) {
        chosen = rank / factorial[i]; rank %= factorial[i];
        order[i] = available[chosen];
        for (j = chosen; j < BLOCK_COUNT - 1 - i; ++j) available[j] = available[j + 1];
    }
}

static int Valid(const unsigned char *p)
{
    unsigned sum = 0, i;
    if (Read16(p + WT_WIRE_PK4_SANITY) != 0) return 0;
    for (i = WT_WIRE_PK4_BLOCKS; i < WT_POKEMON_SIZE; i += 2) sum += Read16(p + i);
    return (sum & UINT16_MAX) == Read16(p + WT_WIRE_PK4_CHECKSUM);
}

static void Crypt(unsigned char *p, uint32_t seed)
{
    unsigned i;
    for (i = WT_WIRE_PK4_BLOCKS; i < WT_POKEMON_SIZE; i += 2) {
        seed = seed * WT_LCG_MULTIPLIER + WT_LCG_INCREMENT;
        p[i] ^= (unsigned char)(seed >> 16); p[i + 1] ^= (unsigned char)(seed >> 24);
    }
}

int WtPokemon_Decode(const unsigned char encrypted[WT_POKEMON_SIZE], unsigned char canonical[WT_POKEMON_SIZE])
{
    unsigned char image[WT_POKEMON_SIZE], order[BLOCK_COUNT];
    unsigned i;
    memcpy(image, encrypted, sizeof(image)); Crypt(image, Read16(image + WT_WIRE_PK4_CHECKSUM));
    Order(Read32(image), order); memcpy(canonical, image, WT_WIRE_PK4_BLOCKS);
    for (i = 0; i < BLOCK_COUNT; ++i) memcpy(canonical + WT_WIRE_PK4_BLOCKS + BLOCK_BYTES * order[i], image + WT_WIRE_PK4_BLOCKS + BLOCK_BYTES * i, BLOCK_BYTES);
    return Valid(canonical) ? 0 : -1;
}

int WtPokemon_Encode(const unsigned char canonical[WT_POKEMON_SIZE], unsigned char encrypted[WT_POKEMON_SIZE])
{
    unsigned char image[WT_POKEMON_SIZE], order[BLOCK_COUNT];
    unsigned i;
    if (!Valid(canonical)) return -1;
    Order(Read32(canonical), order); memcpy(image, canonical, WT_WIRE_PK4_BLOCKS);
    for (i = 0; i < BLOCK_COUNT; ++i) memcpy(image + WT_WIRE_PK4_BLOCKS + BLOCK_BYTES * i, canonical + WT_WIRE_PK4_BLOCKS + BLOCK_BYTES * order[i], BLOCK_BYTES);
    Crypt(image, Read16(image + WT_WIRE_PK4_CHECKSUM)); memcpy(encrypted, image, sizeof(image));
    return 0;
}

int WtPokemon_Fits(const unsigned char canonical[WT_POKEMON_SIZE], const WtPokemonLimits *limits)
{
    /* PK4 and PK5 alike (PKHeX PK4/PK5): block A species 0x08, held item 0x0A;
     * block B form in bits 3-7 of 0x40; block D ball at 0x83 (HG/SS also read 0x86). */
    unsigned species = Read16(canonical + WT_WIRE_PK4_BLOCKS), item = Read16(canonical + ITEM_OFFSET);
    unsigned form = canonical[FORM_OFFSET] >> FORM_SHIFT, ball = canonical[BALL_OFFSET];
    if (!Valid(canonical) || species == 0 || species > limits->maxSpecies
        || form >= limits->forms(species) || ball == 0 || ball > limits->maxBall) return 0;
    return item == 0 || (item <= limits->maxItem && limits->holdable(limits->arg, item));
}
