#include "wonder_trade/seed.h"

#include <string.h>
#include <mbedtls/platform_util.h>
#include <psa/crypto.h>

#define WT_SEED_OFFSET 1444
#define WT_HASH_OFFSET 4060

static uint64_t WtSeed_Generation(const unsigned char *record)
{
    uint64_t value = 0;
    int i;
    for (i = 7; i >= 0; i--) {
        value = (value << 8) | record[16 + i];
    }
    return value;
}

static int WtSeed_Hash(unsigned char *record, unsigned char digest[32])
{
    size_t size;
    return psa_hash_compute(PSA_ALG_SHA_256, record, WT_HASH_OFFSET,
        digest, 32, &size) == PSA_SUCCESS && size == 32;
}

static int WtSeed_Valid(unsigned char *record)
{
    unsigned char digest[32];
    size_t i;
    unsigned char nonzero = 0;
    if (memcmp(record, "PWTJ\1\0\0\0\0\20\0\0\0\0\0\0", 16) != 0
        || memcmp(record + 1440, "\1\0\40\0", 4) != 0
        || memcmp(record + 4092, "CMIT", 4) != 0
        || !WtSeed_Hash(record, digest) || memcmp(record + WT_HASH_OFFSET, digest, 32) != 0) {
        return 0;
    }
    for (i = 24; i < WT_HASH_OFFSET; i++) {
        if (i >= WT_SEED_OFFSET && i < WT_SEED_OFFSET + WT_SEED_SIZE) {
            nonzero |= record[i];
        } else if ((i < 1440 || i >= 1444) && record[i] != 0) {
            return 0;
        }
    }
    return nonzero != 0;
}

static int WtSeed_Unknown(const unsigned char *record)
{
    size_t i;
    if (memcmp(record, "PWTJ", 4) == 0) {
        return record[4] != 1 || record[5] != 0 || record[6] != 0 || record[7] != 0;
    }
    if (record[0] != 0 && record[0] != 255) {
        return 1;
    }
    for (i = 1; i < WT_JOURNAL_SIZE; i++) {
        if (record[i] != record[0]) {
            return 1;
        }
    }
    return 0;
}

int WtSeed_Open(WtSeed *store, void *arg, WtSeedReadBank read, WtSeedWriteBank write)
{
    unsigned char other[WT_JOURNAL_SIZE];
    int firstValid;
    int secondValid;
    int result = -1;

    memset(store, 0, sizeof(*store));
    store->arg = arg;
    store->write = write;
    if (read(arg, 0, store->record) != 0 || read(arg, 1, other) != 0) {
        goto done;
    }
    firstValid = WtSeed_Valid(store->record);
    secondValid = WtSeed_Valid(other);
    /* An older connectivity build must never overwrite a newer trade journal. */
    if (WtSeed_Unknown(store->record) || WtSeed_Unknown(other)) {
        goto done;
    }
    if (!firstValid && !secondValid) {
        goto done;
    }
    if (firstValid && secondValid
        && WtSeed_Generation(store->record) == WtSeed_Generation(other)
        && memcmp(store->record, other, WT_JOURNAL_SIZE) != 0) {
        goto done;
    }
    if (!firstValid || (secondValid && WtSeed_Generation(other) > WtSeed_Generation(store->record))) {
        memcpy(store->record, other, WT_JOURNAL_SIZE);
        store->selected = 1;
    }
    store->usable = 1;
    result = 0;
done:
    mbedtls_platform_zeroize(other, sizeof(other));
    if (result != 0) {
        mbedtls_platform_zeroize(store->record, sizeof(store->record));
    }
    return result;
}

int WtSeed_Read(WtSeed *store, unsigned char *seed, size_t size)
{
    if (!store->usable || size != WT_SEED_SIZE) {
        return -1;
    }
    memcpy(seed, store->record + WT_SEED_OFFSET, size);
    return (int)size;
}

int WtSeed_Write(WtSeed *store, const unsigned char *seed, size_t size)
{
    unsigned char digest[32];
    uint64_t generation = WtSeed_Generation(store->record);
    int i;
    unsigned char nonzero = 0;

    if (!store->usable || size != WT_SEED_SIZE || generation == UINT64_MAX
        || memcmp(seed, store->record + WT_SEED_OFFSET, size) == 0) {
        return -1;
    }
    for (i = 0; i < WT_SEED_SIZE; i++) {
        nonzero |= seed[i];
    }
    if (nonzero == 0) {
        return -1;
    }
    store->usable = 0;
    generation++;
    for (i = 0; i < 8; i++) {
        store->record[16 + i] = (unsigned char)(generation >> (8 * i));
    }
    memcpy(store->record + WT_SEED_OFFSET, seed, size);
    if (!WtSeed_Hash(store->record, digest)) {
        return -1;
    }
    memcpy(store->record + WT_HASH_OFFSET, digest, sizeof(digest));
    /* Never retire the selected valid bank before its replacement is committed. */
    if (store->write(store->arg, store->selected ^ 1, store->record) != 0
        || store->write(store->arg, store->selected, store->record) != 0) {
        return -1;
    }
    store->usable = 1;
    return (int)size;
}
