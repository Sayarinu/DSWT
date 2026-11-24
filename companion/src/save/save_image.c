#include "save_image.h"

#include <string.h>

#include "sha256.h"

static const uint32_t offsets[2] = { CP_SAVE_GENERAL_OFFSET, CP_SAVE_BOX_OFFSET };
static const uint32_t sizes[2] = { CP_SAVE_GENERAL_SIZE, CP_SAVE_BOX_SIZE };

static uint16_t Read16(const unsigned char *p)
{
    return (uint16_t)(p[0] | ((unsigned)p[1] << 8));
}

static uint32_t Read32(const unsigned char *p)
{
    return Read16(p) | ((uint32_t)Read16(p + 2) << 16);
}

static void Write32(unsigned char *p, uint32_t value)
{
    unsigned i;
    for (i = 0; i < 4; ++i) p[i] = (unsigned char)(value >> (i * 8));
}

static uint16_t Crc(uint16_t crc, const unsigned char *data, size_t size)
{
    size_t i;
    unsigned bit;
    for (i = 0; i < size; ++i) {
        crc ^= (uint16_t)((unsigned)data[i] << 8);
        for (bit = 0; bit < 8; ++bit)
            crc = (uint16_t)((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0));
    }
    return crc;
}

static int FooterValid(const unsigned char *footer, unsigned block, uint32_t magic, uint16_t crc)
{
    return Read32(footer + 8) == sizes[block] && Read32(footer + 12) == magic
        && footer[16] == block && Read16(footer + 18) == crc;
}

/* Retail uses numeric ordering except for the single wrap boundary. */
static int Compare(uint32_t a, uint32_t b)
{
    if (a == UINT32_MAX && b == 0) return -1;
    if (a == 0 && b == UINT32_MAX) return 1;
    return (a > b) - (a < b);
}

static const unsigned char *Block(const CpSaveImage *image, unsigned block)
{
    return image->bytes + image->currentHalf[block] * CP_SAVE_HALF_SIZE + offsets[block];
}

static int FileSize(const CpFile *file)
{
    uint64_t size;
    return file != NULL && file->size != NULL && file->readAt != NULL
        && file->size(file->context, &size) == 0 && size == CP_SAVE_FILE_SIZE;
}

CpSaveResult CpSaveImage_Load(CpSaveImage *image, const CpFile *file, uint32_t magic)
{
    unsigned block, half, valid[2] = { 0, 0 };
    uint32_t globals[2][2], counters[2][2];
    int inconsistent = 0;
    if (image == NULL) return CP_SAVE_INVALID;
    image->loaded = 0;
    if (magic != CP_SAVE_MAGIC_INTERNATIONAL && magic != CP_SAVE_MAGIC_KOREAN)
        return CP_SAVE_INVALID;
    if (!FileSize(file)) return CP_SAVE_IO_ERROR;
    if (file->readAt(file->context, 0, image->bytes, sizeof(image->bytes)) != 0)
        return CP_SAVE_IO_ERROR;
    for (block = 0; block < 2; ++block) {
        for (half = 0; half < 2; ++half) {
            const unsigned char *body = image->bytes + half * CP_SAVE_HALF_SIZE + offsets[block];
            const unsigned char *footer = body + sizes[block] - CP_SAVE_FOOTER_SIZE;
            globals[block][half] = Read32(footer);
            counters[block][half] = Read32(footer + 4);
            if (FooterValid(footer, block, magic, Crc(0xffff, body, sizes[block] - CP_SAVE_FOOTER_SIZE)))
                valid[block] |= 1u << half;
        }
        if (valid[block] == 0) return CP_SAVE_INVALID;
        if (valid[block] == 3) {
            int global = Compare(globals[block][0], globals[block][1]);
            int counter = Compare(counters[block][0], counters[block][1]);
            /* The retail loader asserts that full saves advance both counters. */
            if (global != 0 && global != counter) inconsistent = 1;
            image->currentHalf[block] = (global != 0 ? global : counter) < 0;
        } else {
            image->currentHalf[block] = valid[block] == 2;
        }
    }
    if (inconsistent
        || globals[0][image->currentHalf[0]] != globals[1][image->currentHalf[1]]
        || (valid[0] != 3 && valid[1] == 3)
        || (valid[0] != 3 && valid[1] != 3 && image->currentHalf[0] != image->currentHalf[1]))
        return CP_SAVE_NEEDS_GAME_SAVE;
    image->globalCounter = globals[0][image->currentHalf[0]];
    for (block = 0; block < 2; ++block)
        image->blockCounters[block] = counters[block][image->currentHalf[block]];
    image->magic = magic;
    image->loaded = 1;
    return CP_SAVE_OK;
}

unsigned char *CpSaveImage_Block(CpSaveImage *image, unsigned block)
{
    if (image == NULL || !image->loaded || block >= 2) return NULL;
    return image->bytes + image->currentHalf[block] * CP_SAVE_HALF_SIZE + offsets[block];
}

CpSaveResult CpSaveImage_Hash(const CpSaveImage *image, unsigned char digest[CP_SAVE_HASH_SIZE])
{
    CpSaveSha256 hash;
    unsigned block;
    if (image == NULL || !image->loaded || digest == NULL) return CP_SAVE_INVALID;
    CpSaveSha256_Init(&hash);
    for (block = 0; block < 2; ++block)
        CpSaveSha256_Update(&hash, Block(image, block), sizes[block] - CP_SAVE_FOOTER_SIZE);
    CpSaveSha256_Finish(&hash, digest);
    return CP_SAVE_OK;
}

CpSaveResult CpSaveImage_Verify(const CpSaveImage *image, const CpFile *file,
                              const unsigned char expected[CP_SAVE_HASH_SIZE])
{
    unsigned half, block;
    unsigned char page[256], footer[CP_SAVE_FOOTER_SIZE], digest[CP_SAVE_HASH_SIZE];
    if (image == NULL || !image->loaded || expected == NULL) return CP_SAVE_INVALID;
    if (!FileSize(file)) return CP_SAVE_IO_ERROR;
    for (half = 0; half < 2; ++half) {
        CpSaveSha256 hash;
        CpSaveSha256_Init(&hash);
        for (block = 0; block < 2; ++block) {
            uint32_t offset = 0, bodySize = sizes[block] - CP_SAVE_FOOTER_SIZE;
            uint32_t address = half * CP_SAVE_HALF_SIZE + offsets[block];
            uint16_t crc = 0xffff;
            while (offset < bodySize) {
                size_t count = bodySize - offset;
                if (count > sizeof(page)) count = sizeof(page);
                if (file->readAt(file->context, address + offset, page, count) != 0)
                    return CP_SAVE_IO_ERROR;
                crc = Crc(crc, page, count);
                CpSaveSha256_Update(&hash, page, count);
                offset += (uint32_t)count;
            }
            if (file->readAt(file->context, address + bodySize, footer, sizeof(footer)) != 0)
                return CP_SAVE_IO_ERROR;
            if (!FooterValid(footer, block, image->magic, crc)
                || (Read32(footer) != image->globalCounter
                    && Read32(footer) != (uint32_t)(image->globalCounter - 1))
                || (Read32(footer + 4) != image->blockCounters[block]
                    && Read32(footer + 4) != (uint32_t)(image->blockCounters[block] - 1)))
                return CP_SAVE_VERIFY_FAILED;
        }
        CpSaveSha256_Finish(&hash, digest);
        if (memcmp(digest, expected, sizeof(digest)) != 0) return CP_SAVE_VERIFY_FAILED;
    }
    return CP_SAVE_OK;
}

CpSaveResult CpSaveImage_CommitBoth(CpSaveImage *image, const CpFile *file,
                                  const unsigned char expected[CP_SAVE_HASH_SIZE])
{
    unsigned pass, block;
    unsigned char digest[CP_SAVE_HASH_SIZE];
    CpSaveResult result = CpSaveImage_Hash(image, digest);
    if (result != CP_SAVE_OK || expected == NULL) return CP_SAVE_INVALID;
    if (memcmp(digest, expected, sizeof(digest)) != 0) return CP_SAVE_VERIFY_FAILED;
    if (!FileSize(file) || file->writeAt == NULL || file->sync == NULL) return CP_SAVE_IO_ERROR;
    for (pass = 0; pass < 2; ++pass) {
        uint32_t global = image->globalCounter + 1;
        for (block = 0; block < 2; ++block) {
            unsigned target = 1 - image->currentHalf[block];
            uint32_t address = target * CP_SAVE_HALF_SIZE + offsets[block];
            uint32_t bodySize = sizes[block] - CP_SAVE_FOOTER_SIZE;
            const unsigned char *body = Block(image, block);
            unsigned char footer[CP_SAVE_FOOTER_SIZE];
            uint16_t crc = Crc(0xffff, body, bodySize);
            /* Retain the ignored padding byte just as retail does. */
            memcpy(footer, body + bodySize, sizeof(footer));
            Write32(footer, global);
            Write32(footer + 4, image->blockCounters[block] + 1);
            Write32(footer + 8, sizes[block]);
            Write32(footer + 12, image->magic);
            footer[16] = (unsigned char)block;
            footer[18] = (unsigned char)crc;
            footer[19] = (unsigned char)(crc >> 8);
            if (file->writeAt(file->context, address, body, bodySize) != 0
                || file->sync(file->context) != 0
                || file->writeAt(file->context, address + bodySize + 8, footer + 8, 12) != 0
                || file->sync(file->context) != 0
                || file->writeAt(file->context, address + bodySize, footer, 8) != 0
                || file->sync(file->context) != 0) {
                image->loaded = 0;
                return CP_SAVE_IO_ERROR;
            }
            memcpy(image->bytes + address, body, bodySize);
            memcpy(image->bytes + address + bodySize, footer, sizeof(footer));
            image->currentHalf[block] = target;
            image->blockCounters[block]++;
        }
        image->globalCounter = global;
    }
    result = CpSaveImage_Verify(image, file, expected);
    if (result != CP_SAVE_OK) image->loaded = 0;
    return result;
}
