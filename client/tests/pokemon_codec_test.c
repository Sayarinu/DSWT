#include "wonder_trade/pokemon_codec.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>

/* A small made-up game: species 1..10, species 3 has two forms, items 1..20
 * exist and item 7 cannot be held, balls 1..4. */
static unsigned Forms(unsigned species) { return species == 3 ? 2 : 1; }
static int Holdable(void *arg, unsigned item) { ++*(unsigned *)arg; return item != 7; }

static void Set(unsigned char pk4[136], unsigned offset, unsigned value)
{
    unsigned i, sum = 0;
    pk4[offset] = (unsigned char)value;
    if (offset == 8 || offset == 10) pk4[offset + 1] = (unsigned char)(value >> 8);
    for (i = 8; i < 136; i += 2) sum += pk4[i] | (unsigned)pk4[i + 1] << 8;
    pk4[6] = (unsigned char)sum; pk4[7] = (unsigned char)(sum >> 8);
}

static int Fits(unsigned species, unsigned form, unsigned item, unsigned ball, unsigned *lookups)
{
    const WtPokemonLimits limits = { 10, 20, 4, Forms, Holdable, lookups };
    unsigned char pk4[136];
    memset(pk4, 0, sizeof(pk4)); pk4[0] = 0x5a;
    Set(pk4, 8, species); Set(pk4, 10, item); Set(pk4, 0x40, form << 3 | 1); Set(pk4, 0x83, ball);
    return WtPokemon_Fits(pk4, &limits);
}

static void Limits(void)
{
    const WtPokemonLimits limits = { 10, 20, 4, Forms, Holdable, NULL };
    unsigned char pk4[136];
    unsigned lookups = 0;
    assert(Fits(1, 0, 0, 1, &lookups) && Fits(10, 0, 20, 4, &lookups) && Fits(3, 1, 1, 2, &lookups));
    assert(lookups == 2);
    assert(!Fits(0, 0, 0, 1, &lookups) && !Fits(11, 0, 0, 1, &lookups));   /* species */
    assert(!Fits(3, 2, 0, 1, &lookups) && !Fits(1, 1, 0, 1, &lookups));    /* form */
    assert(!Fits(1, 0, 21, 1, &lookups) && lookups == 2);                  /* item exists */
    assert(!Fits(1, 0, 7, 1, &lookups) && lookups == 3);                   /* item held */
    assert(!Fits(1, 0, 0, 0, &lookups) && !Fits(1, 0, 0, 5, &lookups));    /* ball */
    memset(pk4, 0, sizeof(pk4)); Set(pk4, 8, 1); Set(pk4, 0x83, 1);
    assert(WtPokemon_Fits(pk4, &limits));
    pk4[6] ^= 1; assert(!WtPokemon_Fits(pk4, &limits));                    /* checksum */
    pk4[6] ^= 1; pk4[4] = 1; assert(!WtPokemon_Fits(pk4, &limits));        /* sanity flags */
}

int main(void)
{
    Limits();
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
