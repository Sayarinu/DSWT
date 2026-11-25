#include "save_image.h"

#include <string.h>

#include "sha256.h"

/* Diamond/Pearl: src/save.c and save_arrays.c of pokediamond; the PC
 * (PCStorage, current box first) is the box block's only array. */
const CpSaveLayout CpSaveLayout_DiamondPearl = {
    CP_SAVE_FOOTER_BLOCK_COUNTERS, 20, { 0, 0xC100 }, { 0xC100, 0x121E0 },
    0x90, 4, 30 * 136, 0, 0, 0
};

const CpSaveLayout CpSaveLayout_Platinum = {
    CP_SAVE_FOOTER_BLOCK_COUNTERS, CP_SAVE_FOOTER_SIZE, { CP_SAVE_GENERAL_OFFSET, CP_SAVE_BOX_OFFSET },
    { CP_SAVE_GENERAL_SIZE, CP_SAVE_BOX_SIZE },
    CP_SAVE_PARTY_CAPACITY_OFFSET, CP_SAVE_BOX_SLOTS_OFFSET - CP_SAVE_BOX_OFFSET, 30 * CP_SAVE_BOX_STRIDE, 0, 0, 0
};

/* HeartGold/SoulSilver: src/save.c of pokeheartgold. PCStorage starts with
 * its 18 boxes of 0x1000 bytes (30 Pokémon and 16 spare), then the current
 * box and boxModifiedFlag (BOX_ALL_MODIFIED_FLAG once every box is marked). */
const CpSaveLayout CpSaveLayout_HeartGoldSoulSilver = {
    CP_SAVE_FOOTER_SHARED_COUNTER, 16, { 0, 0xF700 }, { 0xF628, 0x12310 },
    0x90, 0, 0x1000, 0x12004, 4, 0x3FFFF
};

enum { HGSS_BOXES = 18, HGSS_BOX_SIZE = 0x1000 };

static uint16_t Read16(const unsigned char *p)
{
    return (uint16_t)(p[0] | ((unsigned)p[1] << 8));
}

static uint32_t Read32(const unsigned char *p)
{
    return Read16(p) | ((uint32_t)Read16(p + 2) << 16);
}

static void Write16(unsigned char *p, unsigned value)
{
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
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

static uint32_t BodySize(const CpSaveLayout *layout, unsigned block)
{
    return layout->sizes[block] - layout->footerSize;
}

/* The counter fields of a footer: global and block counter, or the shared one twice. */
static uint32_t GlobalOf(const CpSaveLayout *layout, const unsigned char *footer)
{
    (void)layout;
    return Read32(footer);
}

static uint32_t CounterOf(const CpSaveLayout *layout, const unsigned char *footer)
{
    return Read32(footer + (layout->footer == CP_SAVE_FOOTER_SHARED_COUNTER ? 0 : 4));
}

static int FooterValid(const CpSaveLayout *layout, const unsigned char *footer, unsigned block, uint32_t magic,
                       uint16_t crc)
{
    if (layout->footer == CP_SAVE_FOOTER_SHARED_COUNTER)
        return Read32(footer + 4) == layout->sizes[block] && Read32(footer + 8) == magic
            && Read16(footer + 12) == block && Read16(footer + 14) == crc;
    return Read32(footer + 8) == layout->sizes[block] && Read32(footer + 12) == magic
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
    return image->bytes + image->currentHalf[block] * CP_SAVE_HALF_SIZE + image->layout->offsets[block];
}

/* Body bytes [offset, offset + size) of a block into the hash, the layout's
 * mask read as zero. */
static void HashBody(CpSaveSha256 *hash, const CpSaveLayout *layout, unsigned block, uint32_t offset,
                     const unsigned char *bytes, size_t size)
{
    static const unsigned char zero = 0;
    uint32_t end = offset + (uint32_t)size, at;
    if (block != 1 || layout->maskSize == 0 || end <= layout->maskOffset
        || offset >= layout->maskOffset + layout->maskSize) {
        CpSaveSha256_Update(hash, bytes, size);
        return;
    }
    for (at = offset; at < end; ++at) {
        if (at >= layout->maskOffset && at < layout->maskOffset + layout->maskSize)
            CpSaveSha256_Update(hash, &zero, 1);
        else
            CpSaveSha256_Update(hash, bytes + (at - offset), 1);
    }
}

static int FileSize(const CpFile *file)
{
    uint64_t size;
    return file != NULL && file->size != NULL && file->readAt != NULL
        && file->size(file->context, &size) == 0 && size == CP_SAVE_FILE_SIZE;
}

/* HeartGold/SoulSilver's Save_GetSaveFilesStatus: the newer half when its
 * general and box counters agree, else the older half when they agree there.
 * An invalid footer counts as 0 there; the game would then refuse to load the
 * half it chose, so a chosen half must hold two valid blocks. */
static CpSaveResult SelectShared(CpSaveImage *image, const unsigned valid[2], uint32_t counts[2][2])
{
    unsigned block, half, newer[2], older = 2, good[2];
    for (block = 0; block < 2; ++block) {
        for (half = 0; half < 2; ++half) if (!(valid[block] >> half & 1)) counts[block][half] = 0;
        good[block] = (valid[block] & 1) + (valid[block] >> 1);
        newer[block] = good[block] == 2 ? Compare(counts[block][0], counts[block][1]) < 0 : valid[block] == 2;
    }
    if (good[0] == 2) older = !newer[0];
    if (good[0] == 1 && good[1] == 1) {
        half = newer[0] == newer[1] ? newer[0] : 2;
    } else if (counts[0][newer[0]] == counts[1][newer[0]]) {
        half = newer[0];
    } else {
        /* SLOT_FAIL: the older half, which only two good general copies have. */
        half = older != 2 && counts[0][older] == counts[1][older] ? older : 2;
    }
    if (half == 2 || (valid[0] & valid[1] & (1u << half)) == 0 || counts[0][half] != counts[1][half])
        return CP_SAVE_NEEDS_GAME_SAVE;
    image->currentHalf[0] = image->currentHalf[1] = half;
    image->firstBlock = 0;
    image->globalCounter = image->blockCounters[0] = image->blockCounters[1] = counts[0][half];
    return CP_SAVE_OK;
}

CpSaveResult CpSaveImage_LoadLayout(CpSaveImage *image, const CpFile *file, const CpSaveLayout *layout,
                                  uint32_t magic)
{
    unsigned block, half, valid[2] = { 0, 0 };
    uint32_t globals[2][2], counters[2][2];
    int inconsistent = 0;
    if (image == NULL) return CP_SAVE_INVALID;
    image->loaded = 0;
    if (layout == NULL || (magic != CP_SAVE_MAGIC_INTERNATIONAL && magic != CP_SAVE_MAGIC_KOREAN))
        return CP_SAVE_INVALID;
    image->layout = layout;
    if (!FileSize(file)) return CP_SAVE_IO_ERROR;
    if (file->readAt(file->context, 0, image->bytes, sizeof(image->bytes)) != 0)
        return CP_SAVE_IO_ERROR;
    for (block = 0; block < 2; ++block) {
        for (half = 0; half < 2; ++half) {
            const unsigned char *body = image->bytes + half * CP_SAVE_HALF_SIZE + layout->offsets[block];
            const unsigned char *footer = body + BodySize(layout, block);
            globals[block][half] = GlobalOf(layout, footer);
            counters[block][half] = CounterOf(layout, footer);
            if (FooterValid(layout, footer, block, magic, Crc(0xffff, body, BodySize(layout, block))))
                valid[block] |= 1u << half;
        }
        if (valid[block] == 0) return CP_SAVE_INVALID;
    }
    image->magic = magic;
    if (layout->footer == CP_SAVE_FOOTER_SHARED_COUNTER) {
        CpSaveResult result = SelectShared(image, valid, globals);
        if (result == CP_SAVE_OK) image->loaded = 1;
        return result;
    }
    for (block = 0; block < 2; ++block) {
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
    if (inconsistent) return CP_SAVE_NEEDS_GAME_SAVE;
    image->firstBlock = 0;
    if (globals[0][image->currentHalf[0]] != globals[1][image->currentHalf[1]]) {
        /* LoadCheck falls back to stale general first, or stale boxes if general is partial. */
        if (valid[0] == 3) image->currentHalf[0] ^= 1;
        else if (valid[1] == 3) {
            image->currentHalf[1] ^= 1;
            image->firstBlock = 1;
        }
    }
    if (globals[0][image->currentHalf[0]] != globals[1][image->currentHalf[1]])
        return CP_SAVE_NEEDS_GAME_SAVE;
    image->globalCounter = globals[0][image->currentHalf[0]];
    for (block = 0; block < 2; ++block)
        image->blockCounters[block] = counters[block][image->currentHalf[block]];
    image->loaded = 1;
    return CP_SAVE_OK;
}

CpSaveResult CpSaveImage_Load(CpSaveImage *image, const CpFile *file, uint32_t magic)
{
    return CpSaveImage_LoadLayout(image, file, &CpSaveLayout_Platinum, magic);
}

CpSaveResult CpSaveImage_LoadAny(CpSaveImage *image, const CpFile *file)
{
    static const CpSaveLayout *const layouts[3] = {
        &CpSaveLayout_Platinum, &CpSaveLayout_DiamondPearl, &CpSaveLayout_HeartGoldSoulSilver
    };
    static const uint32_t magics[2] = { CP_SAVE_MAGIC_INTERNATIONAL, CP_SAVE_MAGIC_KOREAN };
    CpSaveResult result, best = CP_SAVE_INVALID;
    unsigned i, j;
    for (i = 0; i < 3; ++i) for (j = 0; j < 2; ++j) {
        result = CpSaveImage_LoadLayout(image, file, layouts[i], magics[j]);
        if (result == CP_SAVE_OK || result == CP_SAVE_IO_ERROR) return result;
        if (result != CP_SAVE_INVALID) best = result;
    }
    return best;
}

unsigned char *CpSaveImage_Block(CpSaveImage *image, unsigned block)
{
    if (image == NULL || !image->loaded || block >= 2) return NULL;
    return image->bytes + image->currentHalf[block] * CP_SAVE_HALF_SIZE + image->layout->offsets[block];
}

CpSaveResult CpSaveImage_Hash(const CpSaveImage *image, unsigned char digest[CP_SAVE_HASH_SIZE])
{
    CpSaveSha256 hash;
    unsigned block;
    if (image == NULL || !image->loaded || digest == NULL) return CP_SAVE_INVALID;
    CpSaveSha256_Init(&hash);
    for (block = 0; block < 2; ++block)
        HashBody(&hash, image->layout, block, 0, Block(image, block), BodySize(image->layout, block));
    CpSaveSha256_Finish(&hash, digest);
    return CP_SAVE_OK;
}

CpSaveResult CpSaveImage_Verify(const CpSaveImage *image, const CpFile *file,
                              const unsigned char expected[CP_SAVE_HASH_SIZE])
{
    unsigned half, block;
    unsigned char page[256], footer[CP_SAVE_FOOTER_SIZE], digest[CP_SAVE_HASH_SIZE];
    const CpSaveLayout *layout;
    if (image == NULL || !image->loaded || expected == NULL) return CP_SAVE_INVALID;
    layout = image->layout;
    if (!FileSize(file)) return CP_SAVE_IO_ERROR;
    for (half = 0; half < 2; ++half) {
        CpSaveSha256 hash;
        uint32_t generalCounter = 0;
        CpSaveSha256_Init(&hash);
        for (block = 0; block < 2; ++block) {
            uint32_t offset = 0, bodySize = BodySize(layout, block);
            uint32_t address = half * CP_SAVE_HALF_SIZE + layout->offsets[block];
            uint16_t crc = 0xffff;
            while (offset < bodySize) {
                size_t count = bodySize - offset;
                if (count > sizeof(page)) count = sizeof(page);
                if (file->readAt(file->context, address + offset, page, count) != 0)
                    return CP_SAVE_IO_ERROR;
                crc = Crc(crc, page, count);
                HashBody(&hash, layout, block, offset, page, count);
                offset += (uint32_t)count;
            }
            if (file->readAt(file->context, address + bodySize, footer, layout->footerSize) != 0)
                return CP_SAVE_IO_ERROR;
            if (!FooterValid(layout, footer, block, image->magic, crc)
                || (GlobalOf(layout, footer) != image->globalCounter
                    && GlobalOf(layout, footer) != (uint32_t)(image->globalCounter - 1))
                || (CounterOf(layout, footer) != image->blockCounters[block]
                    && CounterOf(layout, footer) != (uint32_t)(image->blockCounters[block] - 1))
                || (layout->footer == CP_SAVE_FOOTER_SHARED_COUNTER && block == 1
                    && CounterOf(layout, footer) != generalCounter))
                return CP_SAVE_VERIFY_FAILED;
            generalCounter = CounterOf(layout, footer);
        }
        CpSaveSha256_Finish(&hash, digest);
        if (memcmp(digest, expected, sizeof(digest)) != 0) return CP_SAVE_VERIFY_FAILED;
    }
    return CP_SAVE_OK;
}

static int WriteSync(const CpFile *file, uint32_t address, const unsigned char *bytes, size_t size)
{
    return file->writeAt(file->context, address, bytes, size) != 0 || file->sync(file->context) != 0 ? -1 : 0;
}

/* One block into the other half, written in its game's order. */
static int WriteBlock(const CpFile *file, const CpSaveLayout *layout, unsigned block, uint32_t address,
                      const unsigned char *body, const unsigned char *footer)
{
    uint32_t bodySize = BodySize(layout, block), offset;
    if (layout->footer == CP_SAVE_FOOTER_SHARED_COUNTER) {
        /* Save_WritePCBoxes: every modified box, then Save_WritePCFooter: the
         * rest of PCStorage; the general block is written whole. */
        if (block == 1) {
            for (offset = 0; offset < HGSS_BOXES * HGSS_BOX_SIZE; offset += HGSS_BOX_SIZE)
                if (WriteSync(file, address + offset, body + offset, HGSS_BOX_SIZE) != 0) return -1;
            if (WriteSync(file, address + offset, body + offset, bodySize - offset) != 0) return -1;
        } else if (WriteSync(file, address, body, bodySize) != 0) {
            return -1;
        }
        return WriteSync(file, address + bodySize, footer, layout->footerSize);
    }
    /* Retail publishes the CRC and counters together in the full footer. */
    return WriteSync(file, address, body, bodySize) != 0
        || WriteSync(file, address + bodySize + 8, footer + 8, 8) != 0
        || WriteSync(file, address + bodySize, footer, layout->footerSize) != 0 ? -1 : 0;
}

CpSaveResult CpSaveImage_CommitBoth(CpSaveImage *image, const CpFile *file,
                                  const unsigned char expected[CP_SAVE_HASH_SIZE])
{
    unsigned pass, index;
    unsigned char digest[CP_SAVE_HASH_SIZE];
    const CpSaveLayout *layout;
    CpSaveResult result = CpSaveImage_Hash(image, digest);
    if (result != CP_SAVE_OK || expected == NULL) return CP_SAVE_INVALID;
    if (memcmp(digest, expected, sizeof(digest)) != 0) return CP_SAVE_VERIFY_FAILED;
    if (!FileSize(file) || file->writeAt == NULL || file->sync == NULL) return CP_SAVE_IO_ERROR;
    layout = image->layout;
    for (pass = 0; pass < 2; ++pass) {
        uint32_t global = image->globalCounter + 1;
        /* Overwrite the discarded newer block before its counter can match an edit. */
        for (index = 0; index < 2; ++index) {
            unsigned block = image->firstBlock ^ index;
            unsigned target = 1 - image->currentHalf[block];
            uint32_t address = target * CP_SAVE_HALF_SIZE + layout->offsets[block];
            uint32_t bodySize = BodySize(layout, block);
            unsigned char *body = image->bytes + image->currentHalf[block] * CP_SAVE_HALF_SIZE + layout->offsets[block];
            unsigned char footer[CP_SAVE_FOOTER_SIZE];
            uint16_t crc;
            /* Save_SetAllPCBoxesModified before each save: hashed as zero. */
            if (block == 1 && layout->maskSize == 4) Write32(body + layout->maskOffset, layout->maskCommit);
            crc = Crc(0xffff, body, bodySize);
            /* Retain the ignored padding byte just as retail does. */
            memcpy(footer, body + bodySize, layout->footerSize);
            if (layout->footer == CP_SAVE_FOOTER_SHARED_COUNTER) {
                Write32(footer, global);
                Write32(footer + 4, layout->sizes[block]);
                Write32(footer + 8, image->magic);
                Write16(footer + 12, block);
                Write16(footer + 14, crc);
            } else {
                Write32(footer, global);
                Write32(footer + 4, image->blockCounters[block] + 1);
                Write32(footer + 8, layout->sizes[block]);
                Write32(footer + 12, image->magic);
                footer[16] = (unsigned char)block;
                Write16(footer + 18, crc);
            }
            if (WriteBlock(file, layout, block, address, body, footer) != 0) {
                image->loaded = 0;
                return CP_SAVE_IO_ERROR;
            }
            memcpy(image->bytes + address, body, bodySize);
            memcpy(image->bytes + address + bodySize, footer, layout->footerSize);
            image->currentHalf[block] = target;
            image->blockCounters[block] = layout->footer == CP_SAVE_FOOTER_SHARED_COUNTER
                ? global : image->blockCounters[block] + 1;
        }
        image->globalCounter = global;
        image->firstBlock = 0;
    }
    result = CpSaveImage_Verify(image, file, expected);
    if (result != CP_SAVE_OK) image->loaded = 0;
    return result;
}
