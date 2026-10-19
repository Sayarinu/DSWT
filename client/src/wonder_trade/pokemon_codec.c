#include "wonder_trade/pokemon_codec.h"

#include <stdint.h>
#include <string.h>

static unsigned Read16(const unsigned char *p) { return p[0] | ((unsigned)p[1] << 8); }
static uint32_t Read32(const unsigned char *p) { return Read16(p) | ((uint32_t)Read16(p + 2) << 16); }

static void Order(unsigned pid, unsigned char order[4])
{
    unsigned char available[4] = { 0, 1, 2, 3 };
    const unsigned factorial[4] = { 6, 2, 1, 1 };
    unsigned rank = ((pid >> 13) & 31) % 24, i, j, chosen;
    for (i = 0; i < 4; ++i) {
        chosen = rank / factorial[i]; rank %= factorial[i];
        order[i] = available[chosen];
        for (j = chosen; j < 3 - i; ++j) available[j] = available[j + 1];
    }
}

static int Valid(const unsigned char *p)
{
    unsigned sum = 0, i;
    if (Read16(p + 4) != 0) return 0;
    for (i = 8; i < 136; i += 2) sum += Read16(p + i);
    return (sum & 65535) == Read16(p + 6);
}

static void Crypt(unsigned char *p, uint32_t seed)
{
    unsigned i;
    for (i = 8; i < 136; i += 2) {
        seed = seed * UINT32_C(0x41c64e6d) + UINT32_C(0x6073);
        p[i] ^= (unsigned char)(seed >> 16); p[i + 1] ^= (unsigned char)(seed >> 24);
    }
}

int WtPokemon_Decode(const unsigned char encrypted[136], unsigned char canonical[136])
{
    unsigned char image[136], order[4];
    unsigned i;
    memcpy(image, encrypted, sizeof(image)); Crypt(image, Read16(image + 6));
    Order(Read32(image), order); memcpy(canonical, image, 8);
    for (i = 0; i < 4; ++i) memcpy(canonical + 8 + 32 * order[i], image + 8 + 32 * i, 32);
    return Valid(canonical) ? 0 : -1;
}

int WtPokemon_Encode(const unsigned char canonical[136], unsigned char encrypted[136])
{
    unsigned char image[136], order[4];
    unsigned i;
    if (!Valid(canonical)) return -1;
    Order(Read32(canonical), order); memcpy(image, canonical, 8);
    for (i = 0; i < 4; ++i) memcpy(image + 8 + 32 * i, canonical + 8 + 32 * order[i], 32);
    Crypt(image, Read16(image + 6)); memcpy(encrypted, image, sizeof(image));
    return 0;
}
