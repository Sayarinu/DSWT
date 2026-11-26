#include "game/nitrofs.h"

#include <string.h>

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
    uint8_t header[0x10];

    CpRomFile_Whole(reader, &fs->rom);
    if (CpRomFile_Read(&fs->rom, 0x40, header, sizeof header) != CP_OK) return CP_ERR_IO;
    if (Slice(&fs->rom, CpLe32(header), CpLe32(header + 4), &fs->fnt) != CP_OK ||
        Slice(&fs->rom, CpLe32(header + 8), CpLe32(header + 12), &fs->fat) != CP_OK)
        return CP_ERR_FORMAT;
    return CP_OK;
}

/* Find `name` (length `length`) in directory `directory`: its file ID, or
 * 0xF000 | the subdirectory index. */
static int NitroFs_Find(const NitroFs *fs, uint16_t directory, const char *name, size_t length, uint16_t *out)
{
    uint8_t entry[8], kind, text[127];
    uint32_t at;
    uint16_t file, directories;

    if (CpRomFile_Read(&fs->fnt, 0, entry, sizeof entry) != CP_OK) return CP_ERR_FORMAT;
    directories = CpLe16(entry + 6); /* the root entry's parent field counts directories */
    if ((directory & 0xFFF) >= directories) return CP_ERR_FORMAT;
    if (CpRomFile_Read(&fs->fnt, (uint32_t)(directory & 0xFFF) * 8, entry, sizeof entry) != CP_OK) return CP_ERR_FORMAT;
    at = CpLe32(entry);
    file = CpLe16(entry + 4);
    for (;;) {
        uint8_t size;
        int isDirectory;

        if (CpRomFile_Read(&fs->fnt, at++, &kind, 1) != CP_OK) return CP_ERR_FORMAT;
        if (kind == 0) return CP_ERR_NOT_FOUND;
        if (kind == 0x80) return CP_ERR_FORMAT; /* reserved */
        isDirectory = kind > 0x80;
        size = kind & 0x7F;
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
    uint16_t node = 0xF000; /* the root directory */
    uint8_t span[8];
    int result;

    if ((result = NitroFs_Init(reader, &fs)) != CP_OK) return result;
    for (;;) {
        const char *slash = strchr(path, '/');
        size_t length = slash ? (size_t)(slash - path) : strlen(path);

        if (length == 0 || length > 127) return CP_ERR_NOT_FOUND;
        if ((result = NitroFs_Find(&fs, node, path, length, &node)) != CP_OK) return result;
        if (!slash) break;
        if (node < 0xF000) return CP_ERR_NOT_FOUND; /* a file where a directory should be */
        path = slash + 1;
    }
    if (node >= 0xF000) return CP_ERR_NOT_FOUND; /* a directory */
    if (CpRomFile_Read(&fs.fat, (uint32_t)node * 8, span, sizeof span) != CP_OK) return CP_ERR_FORMAT;
    if (CpLe32(span + 4) < CpLe32(span)) return CP_ERR_FORMAT;
    return Slice(&fs.rom, CpLe32(span), CpLe32(span + 4) - CpLe32(span), out) == CP_OK ? CP_OK : CP_ERR_FORMAT;
}

/* Find a NARC section by its magic, starting at `at`; `found` is its offset. */
static int Narc_Section(const CpRomFile *file, uint32_t at, const char magic[4], uint32_t *found, uint32_t *size)
{
    uint8_t header[8];

    if (CpRomFile_Read(file, at, header, sizeof header) != CP_OK) return CP_ERR_FORMAT;
    if (memcmp(header, magic, 4) != 0 || CpLe32(header + 4) < 8 || CpLe32(header + 4) > file->size - at)
        return CP_ERR_FORMAT;
    *found = at;
    *size = CpLe32(header + 4);
    return CP_OK;
}

int CpNarc_Open(const CpRomFile *file, CpNarc *out)
{
    uint8_t header[0x10], count[2];
    uint32_t fat, fatSize, fnt, fntSize, data, dataSize;

    if (CpRomFile_Read(file, 0, header, sizeof header) != CP_OK) return CP_ERR_FORMAT;
    if (memcmp(header, "NARC", 4) != 0 || CpLe16(header + 4) != 0xFFFE || CpLe16(header + 12) < 0x10)
        return CP_ERR_FORMAT;
    if (Narc_Section(file, CpLe16(header + 12), "BTAF", &fat, &fatSize) != CP_OK ||
        Narc_Section(file, fat + fatSize, "BTNF", &fnt, &fntSize) != CP_OK ||
        Narc_Section(file, fnt + fntSize, "GMIF", &data, &dataSize) != CP_OK)
        return CP_ERR_FORMAT;
    if (fatSize < 12 || CpRomFile_Read(file, fat + 8, count, 2) != CP_OK) return CP_ERR_FORMAT;
    out->file = *file;
    out->count = CpLe16(count);
    out->fatOffset = fat + 12;
    out->dataOffset = data + 8;
    out->dataSize = dataSize - 8;
    return (uint32_t)out->count * 8 <= fatSize - 12 ? CP_OK : CP_ERR_FORMAT;
}

int CpNarc_Member(const CpNarc *narc, uint32_t index, CpRomFile *out)
{
    uint8_t span[8];
    uint32_t start, end;

    if (index >= narc->count) return CP_ERR_NOT_FOUND;
    if (CpRomFile_Read(&narc->file, narc->fatOffset + index * 8, span, sizeof span) != CP_OK) return CP_ERR_FORMAT;
    start = CpLe32(span);
    end = CpLe32(span + 4);
    if (end < start || end > narc->dataSize) return CP_ERR_FORMAT;
    return Slice(&narc->file, narc->dataOffset + start, end - start, out);
}

int CpNarc_OpenPath(const CpReader *reader, const char *path, CpNarc *out)
{
    CpRomFile file;
    int result = CpNitroFs_Open(reader, path, &file);

    return result != CP_OK ? result : CpNarc_Open(&file, out);
}
