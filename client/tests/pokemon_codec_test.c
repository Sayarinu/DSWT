#include "wonder_trade/pokemon_codec.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>

int main(void)
{
    unsigned char canonical[136], encrypted[136], decoded[136], physical[136];
    unsigned permutation, i, sum;
    uint32_t seed;
    for (permutation = 0; permutation < 32; ++permutation) {
        memset(canonical, 0, sizeof(canonical));
        canonical[1] = (unsigned char)(permutation << 5); canonical[2] = (unsigned char)(permutation >> 3);
        sum = 0;
        for (i = 8; i < 136; i += 2) { canonical[i] = (unsigned char)i; sum += i; }
        canonical[6] = (unsigned char)sum; canonical[7] = (unsigned char)(sum >> 8);
        assert(WtPokemon_Encode(canonical, encrypted) == 0);
        assert(WtPokemon_Decode(encrypted, decoded) == 0 && memcmp(decoded, canonical, 136) == 0);
        memcpy(physical, encrypted, 136); seed = sum;
        for (i = 8; i < 136; i += 2) {
            seed = seed * UINT32_C(0x41c64e6d) + UINT32_C(0x6073);
            physical[i] ^= (unsigned char)(seed >> 16); physical[i + 1] ^= (unsigned char)(seed >> 24);
        }
        /* Fixed independent examples distinguish physical order from inverse
         * block locators (permutation 3 is A,C,D,B, not A,D,B,C). */
        if (permutation == 3 || permutation == 27) {
            assert(physical[8] == 8 && physical[40] == 72 && physical[72] == 104 && physical[104] == 40);
        }
        if (permutation == 23) assert(physical[8] == 104 && physical[40] == 72 && physical[72] == 40 && physical[104] == 8);
        encrypted[22] ^= 1; assert(WtPokemon_Decode(encrypted, decoded) != 0);
        canonical[4] = 1; assert(WtPokemon_Encode(canonical, encrypted) != 0);
        canonical[4] = 0; canonical[6] ^= 1; assert(WtPokemon_Encode(canonical, encrypted) != 0);
    }
    return 0;
}
