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

/* PKHeX PokeCrypto.BlockPosition, shared by PK4 and PK5: canonical block b of
 * shuffle value sv = (PID >> 13) & 31 is stored as physical block [sv * 4 + b]. */
static const unsigned char kBlockPosition[32 * 4] = {
    0, 1, 2, 3, 0, 1, 3, 2, 0, 2, 1, 3, 0, 3, 1, 2, 0, 2, 3, 1, 0, 3, 2, 1,
    1, 0, 2, 3, 1, 0, 3, 2, 2, 0, 1, 3, 3, 0, 1, 2, 2, 0, 3, 1, 3, 0, 2, 1,
    1, 2, 0, 3, 1, 3, 0, 2, 2, 1, 0, 3, 3, 1, 0, 2, 2, 3, 0, 1, 3, 2, 0, 1,
    1, 2, 3, 0, 1, 3, 2, 0, 2, 1, 3, 0, 3, 1, 2, 0, 2, 3, 1, 0, 3, 2, 1, 0,
    0, 1, 2, 3, 0, 1, 3, 2, 0, 2, 1, 3, 0, 3, 1, 2, 0, 2, 3, 1, 0, 3, 2, 1,
    1, 0, 2, 3, 1, 0, 3, 2,
};

static void Checksum(unsigned char pokemon[WT_POKEMON_SIZE])
{
    unsigned i, sum = 0;
    for (i = 8; i < WT_POKEMON_SIZE; i += 2) sum += pokemon[i] | (unsigned)pokemon[i + 1] << 8;
    pokemon[6] = (unsigned char)sum; pokemon[7] = (unsigned char)(sum >> 8);
}

static void Put16(unsigned char *bytes, unsigned value)
{
    bytes[0] = (unsigned char)value; bytes[1] = (unsigned char)(value >> 8);
}

/* A made-up PK5, at the offsets of PKHeX PK5.cs: Generation V values that a
 * PK4 cannot hold (Genesect, its fourth form, the Dream Ball). */
static void Pk5(unsigned char pk5[WT_POKEMON_SIZE], uint32_t pid)
{
    unsigned i;
    memset(pk5, 0, WT_POKEMON_SIZE);
    pk5[0] = (unsigned char)pid; pk5[1] = (unsigned char)(pid >> 8);
    pk5[2] = (unsigned char)(pid >> 16); pk5[3] = (unsigned char)(pid >> 24);
    Put16(pk5 + 0x08, 649);                   /* species: Genesect */
    Put16(pk5 + 0x0A, 116);                   /* held item: Douse Drive */
    Put16(pk5 + 0x0C, 12345); Put16(pk5 + 0x0E, 54321); /* TID, SID */
    pk5[0x14] = 70; pk5[0x15] = 133; pk5[0x17] = 2;      /* friendship, ability, language */
    Put16(pk5 + 0x28, 546);                   /* move 1: Techno Blast */
    pk5[0x40] = 4 << 3 | 1;                   /* form 4 (Chill Drive), fateful encounter */
    pk5[0x41] = 10; pk5[0x42] = 1;            /* nature; hidden ability flag */
    for (i = 0; i < 5; ++i) Put16(pk5 + 0x48 + 2 * i, 0x41 + i); /* nickname */
    Put16(pk5 + 0x52, 0xffff);
    pk5[0x5F] = 21;                           /* origin version: Black */
    for (i = 0; i < 4; ++i) Put16(pk5 + 0x68 + 2 * i, 0x50 + i); /* OT name */
    Put16(pk5 + 0x70, 0xffff);
    Put16(pk5 + 0x80, 40002);                 /* met location: an event */
    pk5[0x83] = 25;                           /* ball: Dream Ball */
    pk5[0x84] = 15;                           /* met level, OT male */
    Checksum(pk5);
}

static unsigned Pk5Forms(unsigned species) { return species == 649 ? 5 : 1; }
static int Pk5Holdable(void *arg, unsigned item) { (void)arg; return item != 0; }

static int Pk5Fits(unsigned char pk5[WT_POKEMON_SIZE], unsigned offset, unsigned value, unsigned maxSpecies, unsigned maxBall)
{
    const WtPokemonLimits limits = { maxSpecies, 638, maxBall, Pk5Forms, Pk5Holdable, NULL };
    unsigned char copy[WT_POKEMON_SIZE];
    memcpy(copy, pk5, sizeof(copy));
    if (offset == 0x08 || offset == 0x0A) Put16(copy + offset, value);
    else if (offset) copy[offset] = (unsigned char)value;
    Checksum(copy);
    return WtPokemon_Fits(copy, &limits);
}

static void Pk5Codec(void)
{
    unsigned char canonical[WT_POKEMON_SIZE], physical[WT_POKEMON_SIZE], encrypted[WT_POKEMON_SIZE];
    unsigned char decoded[WT_POKEMON_SIZE];
    unsigned sv, block, i;
    uint32_t seed;
    assert(WT_POKEMON_SIZE == 136 && WT_POKEMON_PK5_PARTY_SIZE == 220 && WT_POKEMON_PK4_PARTY_SIZE == 236);
    for (sv = 0; sv < 32; ++sv) {
        Pk5(canonical, UINT32_C(0x80000005) | (uint32_t)sv << 13);
        /* Independent reference: shuffle by PKHeX's table, then XOR the blocks
         * with the LCG keyed by the checksum. */
        memcpy(physical, canonical, 8);
        for (block = 0; block < 4; ++block)
            memcpy(physical + 8 + 32 * kBlockPosition[sv * 4 + block], canonical + 8 + 32 * block, 32);
        seed = canonical[6] | (uint32_t)canonical[7] << 8;
        for (i = 8; i < WT_POKEMON_SIZE; i += 2) {
            seed = seed * UINT32_C(0x41c64e6d) + UINT32_C(0x6073);
            physical[i] ^= (unsigned char)(seed >> 16); physical[i + 1] ^= (unsigned char)(seed >> 24);
        }
        assert(WtPokemon_Encode(canonical, encrypted) == 0 && memcmp(encrypted, physical, sizeof(physical)) == 0);
        assert(WtPokemon_Decode(physical, decoded) == 0 && memcmp(decoded, canonical, sizeof(canonical)) == 0);
        physical[0x90 - 0x10] ^= 0x80; assert(WtPokemon_Decode(physical, decoded) != 0);
    }
}

static void Pk5Limits(void)
{
    unsigned char pk5[WT_POKEMON_SIZE];
    Pk5(pk5, 0x12345678);
    assert(Pk5Fits(pk5, 0, 0, 649, 25));                                     /* a Black/White game */
    assert(!Pk5Fits(pk5, 0, 0, 493, 25) && !Pk5Fits(pk5, 0, 0, 649, 24));  /* a Generation IV game */
    assert(Pk5Fits(pk5, 0x40, 0 << 3, 649, 25) && !Pk5Fits(pk5, 0x40, 5 << 3, 649, 25));
    assert(!Pk5Fits(pk5, 0x08, 650, 649, 25) && !Pk5Fits(pk5, 0x0A, 639, 649, 25));
    assert(!Pk5Fits(pk5, 0x83, 26, 649, 25) && !Pk5Fits(pk5, 0x83, 0, 649, 25));
    assert(Pk5Fits(pk5, 0x86, 7, 649, 25));   /* PK5 keeps no second ball byte */
    pk5[0x04] = 1; assert(!Pk5Fits(pk5, 0, 0, 649, 25));
}

int main(void)
{
    Limits();
    Pk5Codec();
    Pk5Limits();
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
