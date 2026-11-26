#include "platform/entropy_pool.h"

#include <string.h>

static WtEntropyPool sPool;

static void Clear(void *bytes, size_t size)
{
    volatile unsigned char *p = bytes;
    while (size--) *p++ = 0;
}

void CpEntropyBoot_Start(CpEntropyBoot *boot)
{
    Clear(boot, sizeof(*boot));
}

void CpEntropyBoot_Add(CpEntropyBoot *boot, const void *data, size_t size)
{
    const unsigned char *bytes = data;
    size_t i;
    for (i = 0; i < size; i++) {
        if (boot->size < CP_ENTROPY_BOOT_MAX) boot->bytes[boot->size++] = bytes[i];
        else boot->bytes[boot->total % CP_ENTROPY_BOOT_MAX] ^= bytes[i];
        boot->total++;
    }
}

int CpEntropyBoot_Finish(CpEntropyBoot *boot, WtJournalHash hash)
{
    unsigned char material[1 + CP_ENTROPY_BOOT_MAX + 8], seed[64];
    size_t size = 1 + boot->size, i;
    int result = -1;
    memcpy(material + 1, boot->bytes, boot->size);
    for (i = 0; i < 8; i++) material[size++] = (unsigned char)((unsigned long long)boot->total >> (8 * i));
    /* Two domain-separated digests fill the 64 bytes WtEntropy_Init takes. */
    material[0] = 1;
    if (hash && hash(material, size, seed) == 0) {
        material[0] = 2;
        if (hash(material, size, seed + 32) == 0) result = WtEntropy_Init(&sPool, seed) == 0 ? 0 : -1;
    }
    if (result != 0) Clear(&sPool, sizeof(sPool));
    Clear(material, sizeof(material));
    Clear(seed, sizeof(seed));
    Clear(boot, sizeof(*boot));
    return result;
}

int CpEntropy_Frame(const WtEntropySample *sample)
{
    /* WtEntropy_Frame clears the pool on failure, which keeps every later
     * seed request failing: fail closed. */
    return WtEntropy_Frame(&sPool, sample);
}

int CpEntropy_Seed(unsigned char output[32])
{
    return WtEntropy_Seed(&sPool, output);
}

int CpEntropy_Mix(unsigned char seed[32])
{
    return WtEntropy_Mix(&sPool, seed, seed);
}

int CpEntropy_Ready(void)
{
    return sPool.valid != 0;
}
