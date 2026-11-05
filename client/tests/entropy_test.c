#include "wonder_trade/entropy.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void Ready(WtEntropyPool *pool)
{
    WtEntropySample sample;
    unsigned char seed[32];
    unsigned change, frame;
    memset(&sample, 0, sizeof(sample));
    for (change = 0; change < WT_ENTROPY_REQUIRED_CHANGES; change++) {
        assert(WtEntropy_Seed(pool, seed) == WT_ENTROPY_WAIT);
        assert(memcmp(seed, (unsigned char[32]){0}, 32) == 0);
        for (frame = 0; frame < 4; frame++) {
            sample.frame++;
            sample.low[0] = (unsigned char)sample.frame;
            sample.keys = frame == 3 ? 1u << (change % 12) : 0;
            assert(WtEntropy_Frame(pool, &sample) == 0);
        }
        assert(pool->credit == change + 1);
    }
    assert(WtEntropy_Seed(pool, seed) == 0);
}

int main(void)
{
    WtEntropyPool pool, replay, different, invalid;
    WtEntropySample sample;
    unsigned char boot[64] = {0}, first[32], second[32], stored[32], alias[32];
    unsigned frame;
    assert(WtEntropy_Init(&pool, boot) == 0);
    memset(&sample, 0, sizeof(sample));
    /* Device/timing variability is mixed but must never unlock initialization. */
    for (frame = 0; frame < 1000; frame++) {
        sample.frame = frame; sample.low[0] = (unsigned char)frame;
        assert(WtEntropy_Frame(&pool, &sample) == 0);
    }
    assert(pool.credit == 0);
    sample.keys = 1; assert(WtEntropy_Frame(&pool, &sample) == 0);
    assert(pool.credit == 1);
    for (frame = 0; frame < 100; frame++) assert(WtEntropy_Frame(&pool, &sample) == 0);
    assert(pool.credit == 1);
    sample.keys = 0; assert(WtEntropy_Frame(&pool, &sample) == 0);
    assert(pool.credit == 1);
    sample.touching = 1; sample.x = 16; sample.y = 24;
    assert(WtEntropy_Frame(&pool, &sample) == 0 && pool.credit == 2);
    for (frame = 0; frame < 100; frame++) {
        sample.x = 16 + frame % 8; sample.y = 24 + frame % 8;
        assert(WtEntropy_Frame(&pool, &sample) == 0);
    }
    assert(pool.credit == 2);
    sample.x = 32; assert(WtEntropy_Frame(&pool, &sample) == 0 && pool.credit == 3);
    /* Rapid additional changes are absorbed without additional credit. */
    sample.x = 48; assert(WtEntropy_Frame(&pool, &sample) == 0 && pool.credit == 3);
    sample.touching = 0; assert(WtEntropy_Frame(&pool, &sample) == 0 && pool.credit == 3);

    /* One-pixel jitter across a former cell boundary must not unlock readiness. */
    assert(WtEntropy_Init(&pool, boot) == 0);
    memset(&sample, 0, sizeof(sample));
    for (frame = 0; frame < 4; frame++) assert(WtEntropy_Frame(&pool, &sample) == 0);
    sample.touching = 1; sample.x = 7;
    assert(WtEntropy_Frame(&pool, &sample) == 0 && pool.credit == 1);
    for (frame = 0; frame < 512; frame++) {
        sample.x = 7 + frame % 2;
        assert(WtEntropy_Frame(&pool, &sample) == 0);
    }
    assert(pool.credit == 1 && WtEntropy_Seed(&pool, first) == WT_ENTROPY_WAIT);
    sample.x = 15;
    assert(WtEntropy_Frame(&pool, &sample) == 0 && pool.credit == 2);

    assert(WtEntropy_Init(&pool, boot) == 0); Ready(&pool);
    assert(WtEntropy_Init(&replay, boot) == 0); Ready(&replay);
    assert(WtEntropy_Seed(&pool, first) == 0 && WtEntropy_Seed(&replay, second) == 0);
    assert(memcmp(first, second, 32) == 0); /* Explicitly accepted exact-replay limit. */
    /* Independent hashlib SHA-256 vector fixes domain, byte order and field sizes. */
    assert(memcmp(first, (unsigned char[32]){0x96, 0x70, 0x79, 0x54, 0x63, 0x24, 0x80, 0x63, 0x4d, 0x32, 0xb9, 0x05, 0x71, 0x46, 0xe3, 0x5f, 0x35, 0x0d, 0xad, 0x5a, 0x9d, 0x4b, 0x21, 0xac, 0xcd, 0x8c, 0x82, 0xe5, 0x4a, 0x74, 0xa4, 0x43}, 32) == 0);
    boot[32] = 1; assert(WtEntropy_Init(&different, boot) == 0); Ready(&different);
    assert(WtEntropy_Seed(&different, second) == 0 && memcmp(first, second, 32) != 0);
    memset(stored, 9, 32); memcpy(alias, stored, 32);
    assert(WtEntropy_Mix(&pool, stored, first) == 0);
    assert(WtEntropy_Mix(&pool, alias, alias) == 0 && memcmp(first, alias, 32) == 0);
    for (frame = 0; frame < 32; frame++) assert(stored[frame] == 9);
    assert(WtEntropy_Mix(&different, stored, second) == 0 && memcmp(first, second, 32) != 0);
    assert(WtEntropy_Frame(&pool, &sample) == 0);
    assert(WtEntropy_Mix(&pool, stored, second) == 0 && memcmp(first, second, 32) != 0);
    memset(&invalid, 0, sizeof(invalid)); memset(first, 7, 32);
    assert(WtEntropy_Seed(&invalid, first) == -1 && memcmp(first, (unsigned char[32]){0}, 32) == 0);
    pool.frames = UINT64_MAX;
    assert(WtEntropy_Frame(&pool, &sample) == -1 && !pool.valid);
    puts("Entropy credit, domain mixing, clone divergence and replay limit passed");
    return 0;
}
