#include "platform/nds/card_key.h"

/* GBATEK, DS Encryption by Gamecode/Idcode (KEY1). The initial tables are
 * supplied at run time and are never part of the companion ROM. */
uint64_t CpCardKey_Crypt(const uint32_t table[CP_CARD_KEY_WORDS], uint64_t value, int decrypt)
{
    uint32_t x = value >> 32, y = (uint32_t)value;
    for (unsigned i = 0; i < 16; ++i) {
        uint32_t z = x ^ table[decrypt ? 17 - i : i];
        uint32_t f = table[18 + (z >> 24)] + table[274 + ((z >> 16) & 255)];
        f ^= table[530 + ((z >> 8) & 255)];
        x = y ^ (f + table[786 + (z & 255)]);
        y = z;
    }
    return ((uint64_t)(y ^ table[decrypt ? 0 : 17]) << 32) | (x ^ table[decrypt ? 1 : 16]);
}

static void Expand(uint32_t *table, uint32_t code[3])
{
    uint64_t pair;
    for (int i = 1; i >= 0; --i) {
        pair = CpCardKey_Crypt(table, ((uint64_t)code[i + 1] << 32) | code[i], 0);
        code[i] = pair;
        code[i + 1] = pair >> 32;
    }
    for (unsigned i = 0; i < 18; ++i) {
        uint32_t word = code[i % 2];
        table[i] ^= (word >> 24) | ((word >> 8) & 0xff00) | ((word << 8) & 0xff0000) | (word << 24);
    }
    pair = 0;
    for (unsigned i = 0; i < CP_CARD_KEY_WORDS; i += 2) {
        pair = CpCardKey_Crypt(table, pair, 0);
        table[i] = pair >> 32;
        table[i + 1] = pair;
    }
}

void CpCardKey_Init(uint32_t table[CP_CARD_KEY_WORDS], uint32_t gameCode, unsigned level)
{
    uint32_t code[] = { gameCode, gameCode >> 1, gameCode << 1 };
    Expand(table, code);
    if (level >= 2) Expand(table, code);
    if (level >= 3) {
        code[1] <<= 1;
        code[2] >>= 1;
        Expand(table, code);
    }
}
