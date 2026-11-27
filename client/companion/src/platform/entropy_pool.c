#include "platform/entropy_pool.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

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

int CpEntropy_SeedFromFile(const char *path, WtJournalHash hash, unsigned char output[32])
{
    int result = CpEntropy_Seed(output);
    if (result != WT_ENTROPY_WAIT || !path || !hash) return result;
    FILE *file = fopen(path, "r+b");
    if (!file) return WT_ENTROPY_WAIT;
    unsigned char record[64], digest[32], zero[64] = {0};
    /* Each independent seed has a checksum. A torn consumption makes just
     * that record unusable. Commit its erasure before deriving any output. */
    int consumed = 0;
    if (fseek(file, 0, SEEK_END) == 0 && ftell(file) == 32 * 64 && fseek(file, 0, SEEK_SET) == 0) {
        while (fread(record, 1, sizeof(record), file) == sizeof(record)) {
            if (!memcmp(record, zero, 32) || hash(record, 32, digest) != 0 ||
                memcmp(record + 32, digest, 32)) continue;
            long at = ftell(file) - (long)sizeof(record);
            consumed = at >= 0 && fseek(file, at, SEEK_SET) == 0 &&
                fwrite(zero, 1, sizeof(zero), file) == sizeof(zero) &&
                fflush(file) == 0 && fsync(fileno(file)) == 0;
            break;
        }
    }
    if (fclose(file) == 0 && consumed)
        result = WtEntropy_Mix(&sPool, record, output);
    Clear(record, sizeof(record));
    Clear(digest, sizeof(digest));
    return result;
}
