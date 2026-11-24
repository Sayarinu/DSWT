#include "platform/nds/sd_file.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <nds.h>
#include <fat.h>
#include <nds/arm9/dldi.h>
#include <nds/disc_io.h>

/* A direct FAT12/16/32 reader, enough to map a file's offsets to card
 * sectors without FatFs. One card is mounted at a time. */
static struct {
    const DISC_INTERFACE *io;
    uint32_t fatStart, rootStart, rootSectors, dataStart, rootCluster, sectorsPerCluster;
    uint32_t fatCached;
    int type;
} sFs;
static uint8_t sSector[512] __attribute__((aligned(32)));
static uint8_t sFat[1024] __attribute__((aligned(32)));

static uint16_t Le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t Le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static int RawRead(uint32_t sector, void *destination)
{
    bool ok;
    DC_FlushRange(destination, 512);
    ok = sFs.io->readSectors(sector, 1, destination);
    DC_InvalidateRange(destination, 512);
    return ok ? 0 : -1;
}

static int Mount(const char *path)
{
    uint32_t volume = 0, reserved, fats, rootEntries, total, fatSize, clusters;
    int i;
    memset(&sFs, 0, sizeof(sFs));
    if (strncmp(path, "sd:/", 4) == 0) sFs.io = get_io_dsisd();
    else if (strncmp(path, "fat:/", 5) == 0) sFs.io = dldiGetInternal();
    if (!sFs.io || RawRead(0, sSector) != 0) return -1;
    if (!((sSector[0] == 0xEB || sSector[0] == 0xE9) && Le16(sSector + 11) == 512)) {
        /* A partition table: FatFs mounts the first partition. */
        for (i = 0; i < 4 && !volume; i++)
            if (sSector[446 + 16 * i + 4]) volume = Le32(sSector + 446 + 16 * i + 8);
        if (!volume || RawRead(volume, sSector) != 0) return -1;
    }
    if (memcmp(sSector + 3, "EXFAT   ", 8) == 0 || Le16(sSector + 11) != 512 || !sSector[13]) return -1;
    sFs.sectorsPerCluster = sSector[13];
    reserved = Le16(sSector + 14); fats = sSector[16]; rootEntries = Le16(sSector + 17);
    total = Le16(sSector + 19) ? Le16(sSector + 19) : Le32(sSector + 32);
    fatSize = Le16(sSector + 22) ? Le16(sSector + 22) : Le32(sSector + 36);
    sFs.fatStart = volume + reserved;
    sFs.rootStart = sFs.fatStart + fats * fatSize;
    sFs.rootSectors = (rootEntries * 32 + 511) / 512;
    sFs.dataStart = sFs.rootStart + sFs.rootSectors;
    if (total < sFs.dataStart - volume) return -1;
    clusters = (total - (sFs.dataStart - volume)) / sFs.sectorsPerCluster;
    sFs.type = clusters < 4085 ? 12 : clusters < 65525 ? 16 : 32;
    sFs.rootCluster = sFs.type == 32 ? Le32(sSector + 44) : 0;
    sFs.fatCached = 0xFFFFFFFF;
    return 0;
}

#define END_OF_CHAIN 0x0FFFFFFFu

static uint32_t NextCluster(uint32_t cluster)
{
    uint32_t offset = sFs.type == 12 ? cluster + cluster / 2 : sFs.type == 16 ? cluster * 2 : cluster * 4;
    uint32_t sector = sFs.fatStart + offset / 512, at = offset % 512, value;
    if (sFs.fatCached != sector) {
        /* Two sectors, for a FAT12 entry that spans a boundary. */
        if (RawRead(sector, sFat) != 0 || RawRead(sector + 1, sFat + 512) != 0) {
            sFs.fatCached = 0xFFFFFFFF;
            return END_OF_CHAIN;
        }
        sFs.fatCached = sector;
    }
    if (sFs.type == 12) {
        value = Le16(sFat + at);
        value = cluster & 1 ? value >> 4 : value & 0xFFF;
        return value >= 0xFF8 ? END_OF_CHAIN : value;
    }
    if (sFs.type == 16) {
        value = Le16(sFat + at);
        return value >= 0xFFF8 ? END_OF_CHAIN : value;
    }
    value = Le32(sFat + at) & 0x0FFFFFFF;
    return value >= 0x0FFFFFF8 ? END_OF_CHAIN : value;
}

static uint32_t ClusterSector(uint32_t cluster) { return sFs.dataStart + (cluster - 2) * sFs.sectorsPerCluster; }

/* Find an 8.3 name in a directory; cluster 0 is the FAT12/16 root. */
static int FindEntry(uint32_t cluster, const char name[11], uint8_t entry[32])
{
    unsigned guard;
    for (guard = 0; guard < 65536; guard++) {
        uint32_t sector = cluster ? ClusterSector(cluster) : sFs.rootStart;
        uint32_t count = cluster ? sFs.sectorsPerCluster : sFs.rootSectors, s;
        int e;
        for (s = 0; s < count; s++) {
            if (RawRead(sector + s, sSector) != 0) return -1;
            for (e = 0; e < 16; e++) {
                const uint8_t *d = sSector + e * 32;
                if (d[0] == 0) return -1;
                if (d[0] == 0xE5 || d[11] == 0x0F) continue;
                if (memcmp(d, name, 11) == 0) { memcpy(entry, d, 32); return 0; }
            }
        }
        if (!cluster) return -1;
        cluster = NextCluster(cluster);
        if (cluster < 2 || cluster == END_OF_CHAIN) return -1;
    }
    return -1;
}

static void ShortName(const char *name, char out[11])
{
    const char *dot = strrchr(name, '.');
    size_t length = dot ? (size_t)(dot - name) : strlen(name), i;
    memset(out, ' ', 11);
    for (i = 0; i < length && i < 8; i++) out[i] = name[i];
    if (dot) for (i = 0; dot[1 + i] && i < 3; i++) out[8 + i] = dot[1 + i];
}

/* Locate the file from the FAT, from scratch, and map its cluster chain. */
static int Locate(CpSdFile *file, uint32_t *size)
{
    char prefix[CP_SD_PATH_MAX], name[FAT_SHORT_FILE_NAME_MAX + 1], shortName[11];
    const char *start = strchr(file->path, '/'), *end;
    uint32_t cluster = sFs.type == 32 ? sFs.rootCluster : 0, length = 0;
    uint8_t entry[32];
    if (!start) return -1;
    for (;;) {
        while (*start == '/') start++;
        if (!*start) return -1;
        end = strchr(start, '/');
        if (!end) end = start + strlen(start);
        if ((size_t)(end - file->path) >= sizeof(prefix)) return -1;
        memcpy(prefix, file->path, (size_t)(end - file->path));
        prefix[end - file->path] = 0;
        if (!FAT_getShortNameFor(prefix, name)) return -1;
        ShortName(name, shortName);
        if (FindEntry(cluster, shortName, entry) != 0) return -1;
        cluster = Le16(entry + 26) | (uint32_t)Le16(entry + 20) << 16;
        if (!*end) break;
        if (!(entry[11] & 0x10)) return -1;
        start = end;
    }
    if (entry[11] & 0x10) return -1;
    *size = Le32(entry + 28);
    if (file->firstCluster && cluster != file->firstCluster) return -1;
    file->firstCluster = cluster;
    for (file->chainLength = 0; cluster >= 2 && cluster != END_OF_CHAIN && length < CP_SD_CHAIN_MAX; length++) {
        file->chain[length] = cluster;
        cluster = NextCluster(cluster);
    }
    file->chainLength = length;
    return (uint64_t)length * sFs.sectorsPerCluster * 512 >= *size ? 0 : -1;
}

static int PositionedRead(int fd, uint32_t offset, void *bytes, size_t size)
{
    unsigned char *p = bytes;
    if (fd < 0 || lseek(fd, (off_t)offset, SEEK_SET) != (off_t)offset) return -1;
    while (size) {
        ssize_t count = read(fd, p, size);
        if (count <= 0) return -1;
        p += count; size -= (size_t)count;
    }
    return 0;
}

static int InBounds(uint32_t offset, size_t size)
{
    return offset <= CP_SAVE_SIZE && size <= CP_SAVE_SIZE - offset;
}

static int ReadAt(void *arg, uint32_t offset, void *bytes, size_t size)
{
    CpSdFile *file = arg;
    return InBounds(offset, size) ? PositionedRead(file->fd, offset, bytes, size) : -1;
}

static int WriteAt(void *arg, uint32_t offset, const void *bytes, size_t size)
{
    CpSdFile *file = arg;
    const unsigned char *p = bytes;
    if (!file->writable || file->fd < 0 || !InBounds(offset, size)
        || lseek(file->fd, (off_t)offset, SEEK_SET) != (off_t)offset) return -1;
    while (size) {
        ssize_t count = write(file->fd, p, size);
        if (count <= 0) return -1;
        p += count; size -= (size_t)count;
    }
    return 0;
}

static int Sync(void *arg)
{
    CpSdFile *file = arg;
    return file->fd >= 0 && fsync(file->fd) == 0 ? 0 : -1;
}

static int Size(void *arg, uint32_t *size)
{
    CpSdFile *file = arg;
    struct stat info;
    if (file->fd < 0 || fstat(file->fd, &info) != 0 || info.st_size < 0 || info.st_size > 0x7FFFFFFF) return -1;
    *size = (uint32_t)info.st_size;
    return 0;
}

static int ReadDurable(void *arg, uint32_t offset, void *bytes, size_t size)
{
    CpSdFile *file = arg;
    unsigned char *p = bytes;
    uint32_t clusterBytes, rawSize;
    if (!InBounds(offset, size)) return -1;
    if (file->raw != 0) {
        /* No raw access: close and reopen, so FatFs reads the file again. */
        close(file->fd);
        file->fd = open(file->path, file->writable ? O_RDWR : O_RDONLY);
        return PositionedRead(file->fd, offset, bytes, size);
    }
    if (Locate(file, &rawSize) != 0 || rawSize != CP_SAVE_SIZE) return -1;
    clusterBytes = sFs.sectorsPerCluster * 512;
    while (size) {
        uint32_t index = offset / clusterBytes, within = offset % 512;
        size_t count = 512 - within < size ? 512 - within : size;
        if (index >= file->chainLength
            || RawRead(ClusterSector(file->chain[index]) + (offset % clusterBytes) / 512, sSector) != 0) return -1;
        memcpy(p, sSector + within, count);
        p += count; offset += (uint32_t)count; size -= count;
    }
    return 0;
}

int CpSdFile_Open(CpSdFile *file, const char *path, int writable)
{
    uint32_t size = 0, rawSize = 0;
    memset(file, 0, sizeof(*file));
    file->fd = -1;
    if (!path || strlen(path) >= sizeof(file->path)) return CP_SD_ERROR_PATH;
    strcpy(file->path, path);
    file->writable = writable != 0;
    /* Never O_CREAT or O_TRUNC: only an existing save is opened. */
    file->fd = open(path, writable ? O_RDWR : O_RDONLY);
    if (file->fd < 0) return CP_SD_ERROR_OPEN;
    if (Size(file, &size) != 0 || size != CP_SAVE_SIZE) { CpSdFile_Close(file); return CP_SD_ERROR_SIZE; }
    file->raw = Mount(path) == 0 && Locate(file, &rawSize) == 0 && rawSize == size ? 0 : CP_SD_ERROR_RAW;
    return 0;
}

CpPlatformFile CpSdFile_Platform(CpSdFile *file)
{
    CpPlatformFile platform = { file, ReadAt, NULL, Sync, Size, ReadDurable };
    if (file->writable) platform.writeAt = WriteAt;
    return platform;
}

int CpSdFile_RawStatus(const CpSdFile *file)
{
    return file->raw;
}

void CpSdFile_Close(CpSdFile *file)
{
    if (file->fd >= 0) close(file->fd);
    file->fd = -1;
}
