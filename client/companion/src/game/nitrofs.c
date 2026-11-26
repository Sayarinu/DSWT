#include "game/nitrofs.h"

#include <string.h>

enum {
    NDS_FS_OFFSET = 0x40, NDS_FS_SIZE = 16, FNT_SIZE = 4, FAT_OFFSET = 8, FAT_SIZE = 12,
    DIR_ENTRY_SIZE = 8, DIR_FIRST_FILE = 4, DIR_PARENT = 6, DIR_BASE = 0xF000,
    DIR_INDEX_MASK = 0xFFF, DIR_FLAG = 0x80, NAME_LENGTH_MASK = 0x7F,
    FAT_ENTRY_SIZE = 8, FAT_ENTRY_END = 4,
    NARC_HEADER_SIZE = 16, NARC_BYTE_ORDER = 4, NARC_LITTLE_ENDIAN = 0xFFFE,
    NARC_HEADER_LENGTH = 12, NARC_SECTION_HEADER = 8, NARC_SECTION_LENGTH = 4,
    NARC_FAT_HEADER = 12
};

int CpRomFile_Read(const CpRomFile *file, uint32_t offset, void *destination, uint32_t size)
{
    if (offset > file->size || size > file->size - offset) return CP_ERR_IO;
    if (size == 0) return CP_OK;
    return file->reader->readAt(file->reader->context, file->offset + offset, destination, size) == 0
        ? CP_OK : CP_ERR_IO;
}

int CpRomFile_Whole(const CpReader *reader, CpRomFile *out)
{
    out->reader = reader;
    out->offset = 0;
    out->size = reader->size(reader->context);
    return CP_OK;
}

/* A sub-range of a range, checked against its end. */
static int Slice(const CpRomFile *file, uint32_t offset, uint32_t size, CpRomFile *out)
{
    if (offset > file->size || size > file->size - offset) return CP_ERR_FORMAT;
    out->reader = file->reader;
    out->offset = file->offset + offset;
    out->size = size;
    return CP_OK;
}

/* The ROM header's file name table (FNT) and file allocation table (FAT). */
typedef struct NitroFs {
    CpRomFile rom, fnt, fat;
} NitroFs;

static int NitroFs_Init(const CpReader *reader, NitroFs *fs)
{
    uint8_t header[NDS_FS_SIZE];

    CpRomFile_Whole(reader, &fs->rom);
    if (CpRomFile_Read(&fs->rom, NDS_FS_OFFSET, header, sizeof header) != CP_OK) return CP_ERR_IO;
    if (Slice(&fs->rom, CpLe32(header), CpLe32(header + FNT_SIZE), &fs->fnt) != CP_OK ||
        Slice(&fs->rom, CpLe32(header + FAT_OFFSET), CpLe32(header + FAT_SIZE), &fs->fat) != CP_OK)
        return CP_ERR_FORMAT;
    return CP_OK;
}

/* Find `name` (length `length`) in directory `directory`: its file ID, or
 * 0xF000 | the subdirectory index. */
static int NitroFs_Find(const NitroFs *fs, uint16_t directory, const char *name, size_t length, uint16_t *out)
{
    uint8_t entry[DIR_ENTRY_SIZE], kind, text[NAME_LENGTH_MASK];
    uint32_t at;
    uint16_t file, directories;

    if (CpRomFile_Read(&fs->fnt, 0, entry, sizeof entry) != CP_OK) return CP_ERR_FORMAT;
    directories = CpLe16(entry + DIR_PARENT); /* the root entry's parent field counts directories */
    if ((directory & DIR_INDEX_MASK) >= directories) return CP_ERR_FORMAT;
    if (CpRomFile_Read(&fs->fnt, (uint32_t)(directory & DIR_INDEX_MASK) * DIR_ENTRY_SIZE, entry, sizeof entry) != CP_OK) return CP_ERR_FORMAT;
    at = CpLe32(entry);
    file = CpLe16(entry + DIR_FIRST_FILE);
    for (;;) {
        uint8_t size;
        int isDirectory;

        if (CpRomFile_Read(&fs->fnt, at++, &kind, 1) != CP_OK) return CP_ERR_FORMAT;
        if (kind == 0) return CP_ERR_NOT_FOUND;
        if (kind == DIR_FLAG) return CP_ERR_FORMAT; /* reserved */
        isDirectory = kind > DIR_FLAG;
        size = kind & NAME_LENGTH_MASK;
        if (CpRomFile_Read(&fs->fnt, at, text, size) != CP_OK) return CP_ERR_FORMAT;
        at += size;
        if (isDirectory) {
            uint8_t id[2];

            if (CpRomFile_Read(&fs->fnt, at, id, 2) != CP_OK) return CP_ERR_FORMAT;
            at += 2;
            if (size == length && memcmp(text, name, length) == 0) {
                *out = CpLe16(id);
                return CP_OK;
            }
        } else {
            if (size == length && memcmp(text, name, length) == 0) {
                *out = file;
                return CP_OK;
            }
            ++file;
        }
    }
}

int CpNitroFs_Open(const CpReader *reader, const char *path, CpRomFile *out)
{
    NitroFs fs;
    uint16_t node = DIR_BASE; /* the root directory */
    uint8_t span[FAT_ENTRY_SIZE];
    int result;

    if ((result = NitroFs_Init(reader, &fs)) != CP_OK) return result;
    for (;;) {
        const char *slash = strchr(path, '/');
        size_t length = slash ? (size_t)(slash - path) : strlen(path);

        if (length == 0 || length > NAME_LENGTH_MASK) return CP_ERR_NOT_FOUND;
        if ((result = NitroFs_Find(&fs, node, path, length, &node)) != CP_OK) return result;
        if (!slash) break;
        if (node < DIR_BASE) return CP_ERR_NOT_FOUND; /* a file where a directory should be */
        path = slash + 1;
    }
    if (node >= DIR_BASE) return CP_ERR_NOT_FOUND; /* a directory */
    if (CpRomFile_Read(&fs.fat, (uint32_t)node * FAT_ENTRY_SIZE, span, sizeof span) != CP_OK) return CP_ERR_FORMAT;
    if (CpLe32(span + FAT_ENTRY_END) < CpLe32(span)) return CP_ERR_FORMAT;
    return Slice(&fs.rom, CpLe32(span), CpLe32(span + FAT_ENTRY_END) - CpLe32(span), out) == CP_OK ? CP_OK : CP_ERR_FORMAT;
}

/* Find a NARC section by its magic, starting at `at`; `found` is its offset. */
static int Narc_Section(const CpRomFile *file, uint32_t at, const char magic[4], uint32_t *found, uint32_t *size)
{
    uint8_t header[NARC_SECTION_HEADER];

    if (CpRomFile_Read(file, at, header, sizeof header) != CP_OK) return CP_ERR_FORMAT;
    if (memcmp(header, magic, 4) != 0 || CpLe32(header + NARC_SECTION_LENGTH) < NARC_SECTION_HEADER || CpLe32(header + NARC_SECTION_LENGTH) > file->size - at)
        return CP_ERR_FORMAT;
    *found = at;
    *size = CpLe32(header + NARC_SECTION_LENGTH);
    return CP_OK;
}

int CpNarc_Open(const CpRomFile *file, CpNarc *out)
{
    uint8_t header[NARC_HEADER_SIZE], count[2];
    uint32_t fat, fatSize, fnt, fntSize, data, dataSize;

    if (CpRomFile_Read(file, 0, header, sizeof header) != CP_OK) return CP_ERR_FORMAT;
    if (memcmp(header, "NARC", 4) != 0 || CpLe16(header + NARC_BYTE_ORDER) != NARC_LITTLE_ENDIAN || CpLe16(header + NARC_HEADER_LENGTH) < NARC_HEADER_SIZE)
        return CP_ERR_FORMAT;
    if (Narc_Section(file, CpLe16(header + NARC_HEADER_LENGTH), "BTAF", &fat, &fatSize) != CP_OK ||
        Narc_Section(file, fat + fatSize, "BTNF", &fnt, &fntSize) != CP_OK ||
        Narc_Section(file, fnt + fntSize, "GMIF", &data, &dataSize) != CP_OK)
        return CP_ERR_FORMAT;
    if (fatSize < NARC_FAT_HEADER || CpRomFile_Read(file, fat + NARC_SECTION_HEADER, count, 2) != CP_OK) return CP_ERR_FORMAT;
    out->file = *file;
    out->count = CpLe16(count);
    out->fatOffset = fat + NARC_FAT_HEADER;
    out->dataOffset = data + NARC_SECTION_HEADER;
    out->dataSize = dataSize - NARC_SECTION_HEADER;
    return (uint32_t)out->count * FAT_ENTRY_SIZE <= fatSize - NARC_FAT_HEADER ? CP_OK : CP_ERR_FORMAT;
}

int CpNarc_Member(const CpNarc *narc, uint32_t index, CpRomFile *out)
{
    uint8_t span[FAT_ENTRY_SIZE];
    uint32_t start, end;

    if (index >= narc->count) return CP_ERR_NOT_FOUND;
    if (CpRomFile_Read(&narc->file, narc->fatOffset + index * FAT_ENTRY_SIZE, span, sizeof span) != CP_OK) return CP_ERR_FORMAT;
    start = CpLe32(span);
    end = CpLe32(span + FAT_ENTRY_END);
    if (end < start || end > narc->dataSize) return CP_ERR_FORMAT;
    return Slice(&narc->file, narc->dataOffset + start, end - start, out);
}

int CpNarc_OpenPath(const CpReader *reader, const char *path, CpNarc *out)
{
    CpRomFile file;
    int result = CpNitroFs_Open(reader, path, &file);

    return result != CP_OK ? result : CpNarc_Open(&file, out);
}
