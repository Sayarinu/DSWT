/* The companion builds the client core in place; this links one module on the
 * host to catch a broken include path or source list early. */
#include "wonder_trade/game_family.h"
#include "wonder_trade/pokemon_codec.h"

#include <assert.h>
#include <string.h>

int main(void)
{
    unsigned char canonical[WT_POKEMON_SIZE], stored[WT_POKEMON_SIZE], decoded[WT_POKEMON_SIZE];
    unsigned i, sum = 0;

    assert(WT_GAME_FAMILY == WT_FAMILY_GEN4 && WT_GAME_PARTY_SIZE == WT_POKEMON_PK4_PARTY_SIZE);
    for (i = 0; i < WT_POKEMON_SIZE; ++i) canonical[i] = (unsigned char)(i * 37 + 11);
    canonical[4] = canonical[5] = 0; /* sanity flags */
    for (i = 8; i < WT_POKEMON_SIZE; i += 2) sum += canonical[i] | (unsigned)canonical[i + 1] << 8;
    canonical[6] = (unsigned char)sum;
    canonical[7] = (unsigned char)(sum >> 8);

    assert(WtPokemon_Encode(canonical, stored) == 0);
    assert(memcmp(stored + 8, canonical + 8, WT_POKEMON_SIZE - 8) != 0);
    assert(WtPokemon_Decode(stored, decoded) == 0);
    assert(memcmp(decoded, canonical, WT_POKEMON_SIZE) == 0);

    canonical[6] ^= 1; /* a bad checksum is refused, not repaired */
    assert(WtPokemon_Encode(canonical, stored) != 0);
    return 0;
}
