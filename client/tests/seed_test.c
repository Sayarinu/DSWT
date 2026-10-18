#include "wonder_trade/seed.h"

#include <assert.h>
#include <string.h>
#include <psa/crypto.h>

static unsigned char banks[2][4096];
static unsigned char initial[4096];
static int failAt, cutAt, writes;

static void hash(unsigned char *record)
{
    size_t size;
    assert(psa_hash_compute(PSA_ALG_SHA_256, record, 4060, record + 4060, 32, &size) == PSA_SUCCESS);
    assert(size == 32);
}

static int readBank(void *arg, int bank, unsigned char *record)
{
    (void)arg;
    memcpy(record, banks[bank], 4096);
    return 0;
}

static int writeBank(void *arg, int bank, const unsigned char *record)
{
    int count;
    int step;
    (void)arg;
    writes++;
    count = writes == failAt ? cutAt : 4100;
    /* Power cuts while invalidating the old tag, replacing body, then committing. */
    for (step = 0; step < count; step++) {
        if (step < 4) {
            banks[bank][4092 + step] = 0;
        } else {
            banks[bank][step - 4] = record[step - 4];
        }
    }
    return writes == failAt ? -1 : 0;
}

int main(void)
{
    WtSeed store, recovered;
    unsigned char seed[32], output[32];
    int failure, cut;

    memcpy(initial, "PWTJ\1\0\0\0\0\20\0\0\0\0\0\0", 16);
    initial[16] = 1;
    memcpy(initial + 1440, "\1\0\40\0", 4);
    memset(initial + 1444, 1, 32);
    memcpy(initial + 4092, "CMIT", 4);
    hash(initial);
    memset(seed, 2, 32);
    /* Cut every byte of either mirror overwrite, including both commit tags. */
    for (failure = 1; failure <= 2; failure++) {
        for (cut = 0; cut <= 4100; cut++) {
            memcpy(banks[0], initial, 4096);
            memcpy(banks[1], initial, 4096);
            failAt = failure;
            cutAt = cut;
            writes = 0;
            assert(WtSeed_Open(&store, NULL, readBank, writeBank) == 0);
            assert(WtSeed_Write(&store, seed, 32) == -1);
            assert(WtSeed_Read(&store, output, 32) == -1);
            assert(WtSeed_Open(&recovered, NULL, readBank, writeBank) == 0);
            assert(WtSeed_Read(&recovered, output, 32) == 32);
            assert(output[0] == (failure == 2 || cut == 4100 ? 2 : 1));
            failAt = 0;
            memset(seed, 3, 32);
            assert(WtSeed_Write(&recovered, seed, 32) == 32);
            assert(memcmp(banks[0], banks[1], 4096) == 0);
            memset(seed, 2, 32);
        }
    }
    memcpy(banks[0], initial, 4096);
    memcpy(banks[1], initial, 4096);
    banks[1][1444] = 4;
    hash(banks[1]);
    assert(WtSeed_Open(&store, NULL, readBank, writeBank) != 0);
    memcpy(banks[1], initial, 4096);
    banks[1][1440] = 2;
    hash(banks[1]);
    assert(WtSeed_Open(&store, NULL, readBank, writeBank) != 0);
    memcpy(banks[1], initial, 4096);
    banks[1][1442] = 64;
    hash(banks[1]);
    assert(WtSeed_Open(&store, NULL, readBank, writeBank) != 0);
    memset(banks, 255, sizeof(banks));
    assert(WtSeed_Open(&store, NULL, readBank, writeBank) != 0);
    memcpy(banks[0], initial, 4096);
    memcpy(banks[1], initial, 4096);
    banks[1][6] = 3;
    hash(banks[1]);
    assert(WtSeed_Open(&store, NULL, readBank, writeBank) != 0);
    memcpy(banks[1], initial, 4096);
    memset(banks[0] + 16, 255, 8);
    hash(banks[0]);
    assert(WtSeed_Open(&store, NULL, readBank, writeBank) == 0);
    assert(WtSeed_Write(&store, seed, 32) == -1);
    return 0;
}
