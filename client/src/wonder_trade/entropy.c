#include "wonder_trade/entropy.h"

#include <string.h>
/* The pinned builtin hash is stack-only; frame sampling must not initialize
 * PSA or share the TLS worker's global allocator/RNG state. */
#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#include <mbedtls/private/sha256.h>
#include <mbedtls/platform_util.h>

static void Put(unsigned char *out, uint64_t value, unsigned count)
{
    unsigned i;
    for (i = 0; i < count; i++) out[i] = (unsigned char)(value >> (8 * i));
}

static int Fail(WtEntropyPool *pool)
{
    if (pool) mbedtls_platform_zeroize(pool, sizeof(*pool));
    return -1;
}

int WtEntropy_Init(WtEntropyPool *pool, const unsigned char boot[64])
{
    static const unsigned char domain[] = "PWT.entropy.boot.v1";
    unsigned char material[sizeof(domain) + 64];
    int result;
    if (!pool) return -1;
    memset(pool, 0, sizeof(*pool));
    if (!boot) return -1;
    memcpy(material, domain, sizeof(domain));
    memcpy(material + sizeof(domain), boot, 64);
    result = mbedtls_sha256(material, sizeof(material), pool->digest, 0);
    mbedtls_platform_zeroize(material, sizeof(material));
    if (result != 0) return Fail(pool);
    pool->valid = 1;
    return 0;
}

int WtEntropy_Frame(WtEntropyPool *pool, const WtEntropySample *sample)
{
    static const unsigned char domain[] = "PWT.entropy.frame.v1";
    unsigned char material[sizeof(domain) + 85], digest[32], *p = material;
    uint64_t frame;
    unsigned keys, x, y, changed;
    int result;
    if (!pool || !pool->valid || !sample || pool->frames == UINT64_MAX) return Fail(pool);
    frame = pool->frames + 1;
    memcpy(p, domain, sizeof(domain)); p += sizeof(domain);
    memcpy(p, pool->digest, 32); p += 32;
    memcpy(p, sample->low, 32); p += 32;
    Put(p, frame, 8); p += 8;
    Put(p, sample->frame, 4); p += 4;
    Put(p, sample->keys, 4); p += 4;
    Put(p, sample->x, 2); p += 2;
    Put(p, sample->y, 2); p += 2;
    Put(p, sample->touching != 0, 1); p++;
    result = mbedtls_sha256(material, (size_t)(p - material), digest, 0);
    mbedtls_platform_zeroize(material, sizeof(material));
    if (result != 0) { mbedtls_platform_zeroize(digest, sizeof(digest)); return Fail(pool); }
    memcpy(pool->digest, digest, 32);
    mbedtls_platform_zeroize(digest, sizeof(digest));
    keys = sample->keys & 0x0fff;
    x = sample->x >> 3; y = sample->y >> 3;
    changed = (keys & ~pool->keys) != 0
        || (sample->touching && (!pool->touching || x != pool->x || y != pool->y));
    /* Do not credit clocks, held keys, releases or sub-eight-pixel touch jitter.
     * At most one change counts, spaced by at least four sampled frames. */
    if (changed && frame - pool->lastCreditFrame >= 4 && pool->credit < WT_ENTROPY_REQUIRED_CHANGES) {
        pool->credit++;
        pool->lastCreditFrame = frame;
    }
    pool->frames = frame; pool->keys = keys; pool->x = (uint16_t)x; pool->y = (uint16_t)y;
    pool->touching = sample->touching != 0;
    return 0;
}

static int Derive(const WtEntropyPool *pool, const unsigned char *stored, unsigned char output[32])
{
    static const unsigned char seedDomain[] = "PWT.entropy.seed.v1";
    static const unsigned char mixDomain[] = "PWT.entropy.mix.v1";
    unsigned char material[sizeof(seedDomain) + 72], *p = material;
    const unsigned char *domain = stored ? mixDomain : seedDomain;
    size_t domainSize = stored ? sizeof(mixDomain) : sizeof(seedDomain);
    int result;
    if (!output) return -1;
    if (!pool || !pool->valid) { memset(output, 0, 32); return -1; }
    memcpy(p, domain, domainSize); p += domainSize;
    memcpy(p, pool->digest, 32); p += 32;
    Put(p, pool->frames, 8); p += 8;
    if (stored) { memcpy(p, stored, 32); p += 32; }
    result = mbedtls_sha256(material, (size_t)(p - material), output, 0);
    mbedtls_platform_zeroize(material, sizeof(material));
    if (result != 0) memset(output, 0, 32);
    return result == 0 ? 0 : -1;
}

int WtEntropy_Seed(const WtEntropyPool *pool, unsigned char output[32])
{
    if (!output) return -1;
    if (!pool || !pool->valid) { memset(output, 0, 32); return -1; }
    if (pool->credit < WT_ENTROPY_REQUIRED_CHANGES) { memset(output, 0, 32); return WT_ENTROPY_WAIT; }
    return Derive(pool, NULL, output);
}

int WtEntropy_Mix(const WtEntropyPool *pool, const unsigned char stored[32], unsigned char output[32])
{
    if (!stored) { if (output) memset(output, 0, 32); return -1; }
    return Derive(pool, stored, output);
}
