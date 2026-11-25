#include "gen5_save.h"

#include <string.h>

#include "sha256.h"

/* Block sizes in ID order, as SAVEDATA_BuildLayout lays them out: read from
 * the CRC tables of retail saves and checked against the decompilations'
 * block tables (party 0x534, Battle Box 0x35C, Pokédex 0x4D4 and 0x4DC,
 * trainer data 0x68 and 0xB0, musical 0x2A4). Each ends at 0x23F00 and
 * 0x25F00, where the CRC tables begin. */
static const uint16_t kBlackWhiteBlocks[69] = {
    0x3E0, 0xFF0, 0xFF0, 0xFF0, 0xFF0, 0xFF0, 0xFF0, 0xFF0, 0xFF0, 0xFF0, 0xFF0, 0xFF0, 0xFF0,
    0xFF0, 0xFF0, 0xFF0, 0xFF0, 0xFF0, 0xFF0, 0xFF0, 0xFF0, 0xFF0, 0xFF0, 0xFF0, 0xFF0, 0x9C0,
    0x534, 0x68, 0x9C, 0x1338, 0x7C4, 0xD54, 0x2C, 0x658, 0xA94, 0x1AC, 0x3EC, 0x5C, 0x1E0, 0xA8,
    0x460, 0x1400, 0x2A4, 0x2DC, 0x34C, 0x3EC, 0xF8, 0x2FC, 0x94, 0x35C, 0x1CC, 0x168, 0xEC, 0x1B0,
    0x1C, 0x4D4, 0x34, 0x3C, 0x1AC, 0xB90, 0x9C, 0x850, 0x28, 0x284, 0x10, 0x5C, 0x16C, 0x40, 0xFC
};

static const uint16_t kBlack2White2Blocks[73] = {
    0x3E0, 0xFF0, 0xFF0, 0xFF0, 0xFF0, 0xFF0, 0xFF0, 0xFF0, 0xFF0, 0xFF0, 0xFF0, 0xFF0, 0xFF0,
    0xFF0, 0xFF0, 0xFF0, 0xFF0, 0xFF0, 0xFF0, 0xFF0, 0xFF0, 0xFF0, 0xFF0, 0xFF0, 0xFF0, 0x9EC,
    0x534, 0xB0, 0xA8, 0x1338, 0x7C4, 0xD54, 0x94, 0x658, 0xA94, 0x1AC, 0x3EC, 0x5C, 0x1E0, 0xA8,
    0x460, 0x1400, 0x2A4, 0xE0, 0x34C, 0x4E0, 0xF8, 0x2FC, 0x94, 0x35C, 0x1D4, 0x1E0, 0xF0, 0x1B4,
    0x4DC, 0x34, 0x3C, 0x1AC, 0xB90, 0xAC, 0x850, 0x284, 0x10, 0xA8, 0x16C, 0x80, 0xFC, 0x16A8,
    0x498, 0x60, 0xFC, 0x3E4, 0xF0
};

/* protocol/save-layout-bw.json and -b2w2.json; the block IDs are aletheia's
 * and pokebw2's (include/save/save_control.h SAVE_BLOCK_*). The trainer block
 * (SystemData, TrainerDataSave) holds the intro flag at 0x2C and the markers
 * from 0x30: 10 in Black/White, then their counts; 22 in Black 2/White 2. The
 * musical block's hasDistData is at 0x29E in both (MusicalSave). */
static const CpSaveGen5 kBlackWhite = {
    0x24000, 0x23F9C, 69, kBlackWhiteBlocks,
    0x1A, 1, 0x31, 0x37, 0x1B, 0x2A,
    0x2C, 0x30, 0x44, 0x29E,
    0x56000, 0x1FD14, 0x1FC00
};

static const CpSaveGen5 kBlack2White2 = {
    0x26000, 0x25FA4, 73, kBlack2White2Blocks,
    0x1A, 1, 0x31, 0x36, 0x1B, 0x2A,
    0x2C, 0x30, 0x5C, 0x29E,
    0x55800, 0x17D14, 0x17C00
};

const CpSaveLayout CpSaveLayout_BlackWhite = {
    CP_SAVE_FOOTER_GEN5, 16, { 0, 0 }, { 0, 0 }, 0, 0, 0, 0, 0, 0, &kBlackWhite
};

const CpSaveLayout CpSaveLayout_Black2White2 = {
    CP_SAVE_FOOTER_GEN5, 16, { 0, 0 }, { 0, 0 }, 0, 0, 0, 0, 0, 0, &kBlack2White2
};

enum {
    FOOTER_SIZE = 16,
    /* Where the load reads the backup copy: the image's second half. */
    SCRATCH = CP_SAVE_HALF_SIZE,
    AREA_NONE = 0, AREA_MAIN = 1, AREA_BACKUP = 2,
    /* BACKUP_SYSTEM_CheckFooter's answers. */
    FOOTER_GOOD = 0, FOOTER_CRC = 1, FOOTER_NONE = 2
};

static uint16_t Read16(const unsigned char *p) { return (uint16_t)(p[0] | ((unsigned)p[1] << 8)); }
static uint32_t Read32(const unsigned char *p) { return Read16(p) | ((uint32_t)Read16(p + 2) << 16); }
static void Write16(unsigned char *p, unsigned value) { p[0] = (unsigned char)value; p[1] = (unsigned char)(value >> 8); }

static void Write32(unsigned char *p, uint32_t value)
{
    Write16(p, value & 0xFFFF);
    Write16(p + 2, value >> 16);
}

uint16_t CpGen5Save_Crc(uint16_t crc, const unsigned char *data, size_t size)
{
    size_t i;
    unsigned bit;
    for (i = 0; i < size; ++i) {
        crc ^= (uint16_t)((unsigned)data[i] << 8);
        for (bit = 0; bit < 8; ++bit) crc = (uint16_t)((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0));
    }
    return crc;
}

static uint32_t CrcTableSize(const CpSaveGen5 *g) { return g->blockCount * 2u + (g->blockCount & 1u) * 2u; }
static uint32_t CrcTableAt(const CpSaveGen5 *g) { return g->dataSize - FOOTER_SIZE - CrcTableSize(g); }
static uint32_t CopyAddress(const CpSaveGen5 *g, unsigned area) { return area == AREA_BACKUP ? g->backupOffset : 0; }

static void Offsets(CpSaveImage *image, const CpSaveGen5 *g)
{
    uint32_t total = 0, end;
    unsigned id;
    for (id = 0; id < g->blockCount; ++id) {
        image->blockOffsets[id] = total;
        end = total + g->blockSizes[id] + 4;
        total = (end + 0xFF) & ~UINT32_C(0xFF);
    }
}

static int FileSize(const CpFile *file)
{
    uint64_t size;
    return file != NULL && file->size != NULL && file->readAt != NULL
        && file->size(file->context, &size) == 0 && size == CP_SAVE_FILE_SIZE;
}

/* BACKUP_SYSTEM_CheckFooter over one copy's bytes. */
static unsigned CheckFooter(const CpSaveGen5 *g, const unsigned char *data)
{
    const unsigned char *footer = data + g->dataSize - FOOTER_SIZE;
    if (Read32(footer + 4) != g->dataSize || Read32(footer + 8) != CP_SAVE_MAGIC_GEN5) return FOOTER_NONE;
    return Read16(footer + 14) == CpGen5Save_Crc(0xFFFF, data + CrcTableAt(g), CrcTableSize(g)) ? FOOTER_GOOD : FOOTER_CRC;
}

static void Fill(CpSaveImage *image, unsigned area)
{
    memset(image->areas, area, sizeof(image->areas));
}

/* BACKUP_SYSTEM_LoadDoubleCopy with both copies read: 1 loaded, 0 none, 3 broken. */
static int LoadDouble(CpSaveImage *image, const CpSaveGen5 *g)
{
    unsigned char *work[2];
    const unsigned char *datas[2], *crcs;
    unsigned check[2], areas[2], pass, id, n = g->blockCount;
    uint32_t count[2], counts[2], table = CrcTableAt(g), tableSize = CrcTableSize(g) + FOOTER_SIZE;
    work[0] = image->bytes;
    work[1] = image->bytes + SCRATCH;
    memset(image->areas, 0, sizeof(image->areas));
    for (pass = 0; pass < 2; ++pass) {
        check[pass] = CheckFooter(g, work[pass]);
        count[pass] = Read32(work[pass] + g->dataSize - FOOTER_SIZE);
    }
    if (check[0] == FOOTER_GOOD) {
        if (check[1] != FOOTER_GOOD) {
            image->areas[n] = image->areas[n + 1] = AREA_BACKUP;
            memcpy(work[1] + table, work[0] + table, tableSize);
        }
    } else if (check[1] == FOOTER_GOOD) {
        image->areas[n] = image->areas[n + 1] = AREA_MAIN;
        memcpy(work[0] + table, work[1] + table, tableSize);
    } else {
        Fill(image, AREA_MAIN);
        return check[0] == FOOTER_CRC || check[1] == FOOTER_CRC ? 3 : 0;
    }
    /* Try the newer copy's CRCs first; the check tests the original answers. */
    if (check[0] == FOOTER_GOOD && ((count[0] == 0 && count[1] == UINT32_MAX) || count[0] >= count[1]
            || check[1] != FOOTER_GOOD)) {
        datas[0] = work[0]; datas[1] = work[1]; areas[0] = AREA_MAIN; areas[1] = AREA_BACKUP;
        counts[0] = count[0]; counts[1] = count[1];
    } else {
        datas[0] = work[1]; datas[1] = work[0]; areas[0] = AREA_BACKUP; areas[1] = AREA_MAIN;
        counts[0] = count[1]; counts[1] = count[0];
    }
    for (pass = 0; pass < 2; ++pass) {
        crcs = datas[pass] + table;
        for (id = 0; id < n; ++id) {
            uint32_t offset = image->blockOffsets[id], size = g->blockSizes[id], end = offset + size;
            uint16_t want = Read16(crcs + 2 * id);
            uint16_t mainCrc = Read16(work[0] + end + 2), backupCrc = Read16(work[1] + end + 2);
            image->areas[id] = AREA_NONE;
            if (mainCrc != want && backupCrc != want) break;
            if (mainCrc != want || mainCrc != CpGen5Save_Crc(0xFFFF, work[0] + offset, size))
                image->areas[id] = AREA_MAIN;
            if (backupCrc != want || backupCrc != CpGen5Save_Crc(0xFFFF, work[1] + offset, size)) {
                if (image->areas[id] == AREA_MAIN) break;
                image->areas[id] = AREA_BACKUP;
            }
            if (image->areas[id] == AREA_NONE) {
                uint16_t chosen = Read16(datas[pass] + end);
                if (Read16(work[0] + end) != chosen) image->areas[id] = AREA_MAIN;
                if (Read16(work[1] + end) != chosen) {
                    if (image->areas[id] == AREA_MAIN) break;
                    image->areas[id] = AREA_BACKUP;
                }
            }
        }
        if (id >= n) {
            image->areas[n] = image->areas[n + 1] = (unsigned char)areas[pass ^ 1];
            break;
        }
    }
    if (pass >= 2) {
        Fill(image, AREA_MAIN);
        return 3;
    }
    memmove(work[0] + table, datas[pass] + table, tableSize);
    for (id = 0; id < n; ++id)
        if (image->areas[id] == AREA_MAIN)
            memcpy(work[0] + image->blockOffsets[id], work[1] + image->blockOffsets[id], g->blockSizes[id] + 4u);
    image->globalCounter = image->blockCounters[0] = image->blockCounters[1] = counts[pass];
    return 1;
}

static unsigned char *BlockOf(CpSaveImage *image, unsigned id)
{
    return image->bytes + image->blockOffsets[id];
}

static int Marked(const CpSaveGen5 *g, const unsigned char *trainer, unsigned slot)
{
    return Read16(trainer + g->markerOffset + 2 * slot) == CP_GEN5_MARKER_SET;
}

int CpGen5Save_MusicalLoads(const CpSaveLayout *layout, const CpFile *file, uint32_t count)
{
    const CpSaveGen5 *g = layout ? layout->gen5 : NULL;
    unsigned char page[256], footer[FOOTER_SIZE], table[4], record[4];
    uint32_t offset = 0, size, base;
    uint16_t crc = 0xFFFF;
    if (!g || !FileSize(file)) return -1;
    base = g->musicalAddress;
    /* One block, its footer at blockSize, a 4-byte CRC table, then the footer. */
    if (file->readAt(file->context, base + g->musicalDataSize - FOOTER_SIZE, footer, sizeof(footer)) != 0
        || file->readAt(file->context, base + g->musicalDataSize - FOOTER_SIZE - 4, table, sizeof(table)) != 0
        || file->readAt(file->context, base + g->musicalBlockSize, record, sizeof(record)) != 0) return -1;
    if (Read32(footer + 4) != g->musicalDataSize || Read32(footer + 8) != CP_SAVE_MAGIC_GEN5
        || Read16(footer + 14) != CpGen5Save_Crc(0xFFFF, table, sizeof(table)) || Read32(footer) != count
        || Read16(record + 2) != Read16(table)) return 0;
    while (offset < g->musicalBlockSize) {
        size = g->musicalBlockSize - offset;
        if (size > sizeof(page)) size = sizeof(page);
        if (file->readAt(file->context, base + offset, page, size) != 0) return -1;
        crc = CpGen5Save_Crc(crc, page, size);
        offset += size;
    }
    return crc == Read16(record + 2);
}

CpSaveResult CpGen5Save_Load(CpSaveImage *image, const CpFile *file, const CpSaveLayout *layout)
{
    const CpSaveGen5 *g = layout->gen5;
    unsigned char *trainer, *musical;
    uint32_t musicalCount;
    int result, loads = 0;
    image->loaded = 0;
    image->layout = layout;
    if (!g || g->blockCount > CP_SAVE_GEN5_BLOCKS_MAX) return CP_SAVE_INVALID;
    if (!FileSize(file)) return CP_SAVE_IO_ERROR;
    Offsets(image, g);
    if (file->readAt(file->context, 0, image->bytes, g->dataSize) != 0
        || file->readAt(file->context, g->backupOffset, image->bytes + SCRATCH, g->dataSize) != 0)
        return CP_SAVE_IO_ERROR;
    image->magic = CP_SAVE_MAGIC_GEN5;
    result = LoadDouble(image, g);
    memset(image->bytes + SCRATCH, 0, g->dataSize);
    if (result == 0) return CP_SAVE_INVALID;
    if (result != 1) return CP_SAVE_NEEDS_GAME_SAVE;
    trainer = BlockOf(image, g->trainerBlock);
    /* SAVE_CONTROL_Create: an intro save loads as no save. */
    if (trainer[g->introOffset] != 0) return CP_SAVE_NEEDS_GAME_SAVE;
    musical = BlockOf(image, g->musicalBlock);
    if (Marked(g, trainer, CP_GEN5_MUSICAL_MARKER)) {
        musicalCount = Read32(trainer + g->extraCountOffset + 4 * CP_GEN5_MUSICAL_MARKER);
        loads = CpGen5Save_MusicalLoads(layout, file, musicalCount);
        if (loads < 0) return CP_SAVE_IO_ERROR;
        /* _Reload clears the marker of an extra save Create could not load. */
        if (!loads) Write16(trainer + g->markerOffset + 2 * CP_GEN5_MUSICAL_MARKER, 0);
    }
    musical[g->hasDistOffset] = (unsigned char)(loads ? 1 : 0);
    image->loaded = 1;
    return CP_SAVE_OK;
}

CpSaveResult CpGen5Save_Hash(const CpSaveImage *image, unsigned char digest[CP_SAVE_HASH_SIZE])
{
    const CpSaveGen5 *g = image->layout->gen5;
    CpSaveSha256 hash;
    unsigned id;
    CpSaveSha256_Init(&hash);
    for (id = 0; id < g->blockCount; ++id)
        CpSaveSha256_Update(&hash, image->bytes + image->blockOffsets[id], g->blockSizes[id]);
    CpSaveSha256_Finish(&hash, digest);
    return CP_SAVE_OK;
}

/* game_save.c Verify: both copies hold the expected blocks, each with the
 * footer the image has and a body with its CRC, and the image's CRC table
 * and footer. The game then loads exactly this save from either copy. */
CpSaveResult CpGen5Save_Verify(const CpSaveImage *image, const CpFile *file,
                               const unsigned char expected[CP_SAVE_HASH_SIZE])
{
    const CpSaveGen5 *g = image->layout->gen5;
    unsigned char page[256], digest[CP_SAVE_HASH_SIZE];
    unsigned copy, id;
    uint32_t start, offset, size, length, base;
    if (!FileSize(file)) return CP_SAVE_IO_ERROR;
    for (copy = 0; copy < 2; ++copy) {
        CpSaveSha256 hash;
        base = copy ? g->backupOffset : 0;
        CpSaveSha256_Init(&hash);
        for (id = 0; id < g->blockCount; ++id) {
            uint16_t crc = 0xFFFF;
            start = image->blockOffsets[id];
            length = g->blockSizes[id];
            for (offset = 0; offset < length; offset += size) {
                size = length - offset < sizeof(page) ? length - offset : (uint32_t)sizeof(page);
                if (file->readAt(file->context, base + start + offset, page, size) != 0) return CP_SAVE_IO_ERROR;
                CpSaveSha256_Update(&hash, page, size);
                crc = CpGen5Save_Crc(crc, page, size);
            }
            if (file->readAt(file->context, base + start + length, page, 4) != 0) return CP_SAVE_IO_ERROR;
            if (Read16(page + 2) != crc || memcmp(page, image->bytes + start + length, 4) != 0)
                return CP_SAVE_VERIFY_FAILED;
        }
        CpSaveSha256_Finish(&hash, digest);
        if (memcmp(digest, expected, sizeof(digest)) != 0) return CP_SAVE_VERIFY_FAILED;
        start = CrcTableAt(g);
        length = CrcTableSize(g) + FOOTER_SIZE;
        for (offset = 0; offset < length; offset += size) {
            size = length - offset < sizeof(page) ? length - offset : (uint32_t)sizeof(page);
            if (file->readAt(file->context, base + start + offset, page, size) != 0) return CP_SAVE_IO_ERROR;
            if (memcmp(page, image->bytes + start + offset, size) != 0) return CP_SAVE_VERIFY_FAILED;
        }
    }
    return CP_SAVE_OK;
}

static int WriteSync(const CpFile *file, uint32_t address, const unsigned char *bytes, size_t size)
{
    return file->writeAt(file->context, address, bytes, size) != 0 || file->sync(file->context) != 0 ? -1 : 0;
}

/* BACKUP_SYSTEM_CompareRecordStep against the main copy: 1 differs. */
static int Differs(const CpSaveImage *image, const CpFile *file, const CpSaveGen5 *g, unsigned id, int *error)
{
    unsigned char page[256];
    uint32_t offset, size, start = image->blockOffsets[id], length = g->blockSizes[id];
    for (offset = 0; offset < length; offset += size) {
        size = length - offset < sizeof(page) ? length - offset : (uint32_t)sizeof(page);
        if (file->readAt(file->context, start + offset, page, size) != 0) { *error = 1; return 1; }
        if (memcmp(page, image->bytes + start + offset, size) != 0) return 1;
    }
    return 0;
}

static void Swap(CpSaveImage *image, unsigned count)
{
    unsigned i;
    for (i = 0; i < count; ++i)
        if (image->areas[i] != AREA_NONE) image->areas[i] = (unsigned char)(AREA_MAIN + AREA_BACKUP - image->areas[i]);
}

/* One SAVE_CONTROL_StartSave and SAVE_CONTROL_MainSave run to the end. */
static int Save(CpSaveImage *image, const CpFile *file)
{
    const CpSaveGen5 *g = image->layout->gen5;
    unsigned n = g->blockCount, pass, id, area;
    uint32_t table = CrcTableAt(g), footer = g->dataSize - FOOTER_SIZE;
    unsigned char *trainer = image->bytes + image->blockOffsets[g->trainerBlock];
    int error = 0;
    /* SAVE_CONTROL_StartSave clears the intro flag; a loaded save has none. */
    trainer[g->introOffset] = 0;
    image->globalCounter++;
    for (id = 0; id < n; ++id) {
        unsigned char *count = image->bytes + image->blockOffsets[id] + g->blockSizes[id];
        if (image->areas[id] == AREA_NONE) {
            if (!Differs(image, file, g, id, &error)) continue;
            if (error) return -1;
            image->areas[id] = AREA_MAIN;
        }
        Write16(count, Read16(count) + 1u);
    }
    for (pass = 0; pass < 2; ++pass) {
        for (id = 0; id < n; ++id) {
            uint32_t at = image->blockOffsets[id], size = g->blockSizes[id];
            uint16_t crc;
            if ((area = image->areas[id]) == AREA_NONE) continue;
            crc = CpGen5Save_Crc(0xFFFF, image->bytes + at, size);
            Write16(image->bytes + at + size + 2, crc);
            Write16(image->bytes + table + 2 * id, crc);
            if (WriteSync(file, CopyAddress(g, area) + at, image->bytes + at, size + 4u) != 0) return -1;
        }
        if (image->areas[n] == AREA_NONE) image->areas[n] = AREA_MAIN;
        if (WriteSync(file, CopyAddress(g, image->areas[n]) + table, image->bytes + table, CrcTableSize(g)) != 0)
            return -1;
        if (image->areas[n + 1] == AREA_NONE) image->areas[n + 1] = AREA_MAIN;
        Write32(image->bytes + footer, image->globalCounter);
        Write32(image->bytes + footer + 4, g->dataSize);
        Write32(image->bytes + footer + 8, CP_SAVE_MAGIC_GEN5);
        Write16(image->bytes + footer + 14, CpGen5Save_Crc(0xFFFF, image->bytes + table, CrcTableSize(g)));
        area = image->areas[n + 1];
        if (WriteSync(file, CopyAddress(g, area) + footer, image->bytes + footer, FOOTER_SIZE - 1) != 0
            || WriteSync(file, CopyAddress(g, area) + footer + FOOTER_SIZE - 1, image->bytes + footer + FOOTER_SIZE - 1,
                1) != 0) return -1;
        if (pass == 0) Swap(image, n + 2);
    }
    memset(image->areas, 0, sizeof(image->areas));
    image->blockCounters[0] = image->blockCounters[1] = image->globalCounter;
    return 0;
}

/* game_save.c SaveBoth after its first Verify: the game's own save; if both
 * copies are not then exactly the expected save, a second save with every
 * block marked for the main copy (BACKUP_SYSTEM_FillBlockAreas). */
CpSaveResult CpGen5Save_Commit(CpSaveImage *image, const CpFile *file,
                               const unsigned char expected[CP_SAVE_HASH_SIZE])
{
    unsigned char digest[CP_SAVE_HASH_SIZE];
    CpSaveResult result = CP_SAVE_VERIFY_FAILED;
    unsigned attempt;
    if (CpGen5Save_Hash(image, digest) != CP_SAVE_OK || memcmp(digest, expected, sizeof(digest)) != 0)
        return CP_SAVE_VERIFY_FAILED;
    if (!FileSize(file) || file->writeAt == NULL || file->sync == NULL) return CP_SAVE_IO_ERROR;
    for (attempt = 0; attempt < 2 && result != CP_SAVE_OK; ++attempt) {
        if (attempt) Fill(image, AREA_MAIN);
        if (Save(image, file) != 0) {
            image->loaded = 0;
            return CP_SAVE_IO_ERROR;
        }
        result = CpGen5Save_Verify(image, file, expected);
    }
    if (result != CP_SAVE_OK) image->loaded = 0;
    return result;
}
