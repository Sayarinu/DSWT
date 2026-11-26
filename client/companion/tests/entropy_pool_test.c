/* The companion's entropy plumbing, against a recording stand-in for the
 * core's pool (wonder_trade/entropy.h): what reaches WtEntropy_Init, and that
 * frames, seeds and mixes all use the one pool. */
#include "platform/entropy_pool.h"
#include "test_sha256.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned char gBoot[64];
static int gInits, gFrames, gFailFrame, gFailHash;
static WtEntropyPool *gPool;
static WtEntropySample gSample;

int WtEntropy_Init(WtEntropyPool *pool, const unsigned char boot[64])
{
    gInits++;
    gPool = pool;
    memcpy(gBoot, boot, 64);
    memset(pool, 0, sizeof(*pool));
    memcpy(pool->digest, boot, 32);
    pool->valid = 1;
    return 0;
}

int WtEntropy_Frame(WtEntropyPool *pool, const WtEntropySample *sample)
{
    assert(pool == gPool);
    gFrames++;
    gSample = *sample;
    if (gFailFrame) { memset(pool, 0, sizeof(*pool)); return -1; }
    pool->frames++;
    pool->digest[0] ^= (unsigned char)sample->keys;
    return 0;
}

int WtEntropy_Seed(const WtEntropyPool *pool, unsigned char output[32])
{
    assert(pool == gPool);
    if (!pool->valid) { memset(output, 0, 32); return -1; }
    memcpy(output, pool->digest, 32);
    return 0;
}

int WtEntropy_Mix(const WtEntropyPool *pool, const unsigned char stored[32], unsigned char output[32])
{
    unsigned i;
    assert(pool == gPool);
    if (!pool->valid) { memset(output, 0, 32); return -1; }
    for (i = 0; i < 32; i++) output[i] = (unsigned char)(stored[i] ^ pool->digest[i] ^ (unsigned char)pool->frames);
    return 0;
}

static int Hash(const unsigned char *bytes, size_t size, unsigned char digest[32])
{
    return gFailHash ? -1 : TestSha256(bytes, size, digest);
}

static void Boot(const unsigned char *bytes, size_t size, unsigned char seed[64])
{
    CpEntropyBoot boot;
    CpEntropyBoot_Start(&boot);
    CpEntropyBoot_Add(&boot, bytes, size);
    assert(CpEntropyBoot_Finish(&boot, Hash) == 0);
    /* The sample is cleared once used. */
    assert(boot.size == 0 && boot.total == 0 && boot.bytes[0] == 0);
    memcpy(seed, gBoot, 64);
}

static void TestBootSample(void)
{
    static unsigned char bytes[3000];
    unsigned char a[64], b[64], expected[1 + CP_ENTROPY_BOOT_MAX + 8];
    unsigned i;
    for (i = 0; i < sizeof(bytes); i++) bytes[i] = (unsigned char)(i * 31 + 7);

    /* WtEntropy_Init gets two domain-separated digests of the sample and its length. */
    Boot(bytes, 100, a);
    expected[0] = 1;
    memcpy(expected + 1, bytes, 100);
    memset(expected + 101, 0, 8);
    expected[101] = 100;
    TestSha256(expected, 109, b);
    assert(memcmp(a, b, 32) == 0);
    expected[0] = 2;
    TestSha256(expected, 109, b);
    assert(memcmp(a + 32, b, 32) == 0);
    assert(CpEntropy_Ready());

    /* Every byte counts, including those folded in past the buffer's size. */
    Boot(bytes, sizeof(bytes), a);
    bytes[sizeof(bytes) - 1] ^= 1;
    Boot(bytes, sizeof(bytes), b);
    assert(memcmp(a, b, 64) != 0);
    Boot(bytes, 10, a);
    Boot(bytes, 11, b);
    assert(memcmp(a, b, 64) != 0);

    /* A hash failure leaves no usable pool. */
    {
        CpEntropyBoot boot;
        unsigned char seed[32];
        gFailHash = 1;
        CpEntropyBoot_Start(&boot);
        CpEntropyBoot_Add(&boot, bytes, 10);
        assert(CpEntropyBoot_Finish(&boot, Hash) != 0 && !CpEntropy_Ready());
        assert(CpEntropy_Seed(seed) != 0);
        gFailHash = 0;
    }
}

static void TestFrames(void)
{
    WtEntropySample sample;
    unsigned char seed[32], stored[32], mixed[32];
    unsigned char bytes[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    unsigned char boot[64];
    Boot(bytes, sizeof(bytes), boot);
    memset(&sample, 0, sizeof(sample));
    sample.keys = 0x41; sample.x = 12; sample.y = 34; sample.touching = 1; sample.frame = 9;
    assert(CpEntropy_Frame(&sample) == 0 && gFrames == 1);
    assert(gSample.keys == 0x41 && gSample.x == 12 && gSample.y == 34 && gSample.touching == 1 && gSample.frame == 9);
    assert(CpEntropy_Seed(seed) == 0 && seed[0] == (boot[0] ^ 0x41));
    /* Mix works in place on the stored seed and depends on the pool. */
    memset(stored, 0x33, sizeof(stored));
    memcpy(mixed, stored, sizeof(mixed));
    assert(CpEntropy_Mix(mixed) == 0 && memcmp(mixed, stored, sizeof(mixed)) != 0);
    /* A failed frame clears the pool: every later seed or mix fails closed. */
    gFailFrame = 1;
    assert(CpEntropy_Frame(&sample) != 0 && !CpEntropy_Ready());
    gFailFrame = 0;
    assert(CpEntropy_Seed(seed) != 0 && CpEntropy_Mix(mixed) != 0);
}

int main(void)
{
    TestBootSample();
    TestFrames();
    assert(gInits > 0);
    printf("entropy_pool_test: ok\n");
    return 0;
}
