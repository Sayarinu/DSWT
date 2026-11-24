#define _POSIX_C_SOURCE 200809L
#include "backup_save.h"
#include "journal_save.h"
#include "save_image.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { PATH_SIZE = 1024, SAVE_SIZE = 0x80000 };

static const char *Root(const CpBackupConfig *config)
{
    return config && config->root ? config->root : CP_BACKUP_DEFAULT_ROOT;
}

static int Directory(char path[PATH_SIZE], const CpBackupConfig *config, const char *name)
{
    size_t length;
    int n;
    if (!name || !*name || !strcmp(name, ".") || !strcmp(name, "..")
        || strpbrk(name, "/\\:") != NULL) return -1;
    length = strlen(name);
    if (length >= PATH_SIZE) return -1;
    if (length > 4 && name[length - 4] == '.'
        && tolower((unsigned char)name[length - 3]) == 's'
        && tolower((unsigned char)name[length - 2]) == 'a'
        && tolower((unsigned char)name[length - 1]) == 'v') length -= 4;
    n = snprintf(path, PATH_SIZE, "%s/%.*s", Root(config), (int)length, name);
    return n < 0 || n >= PATH_SIZE ? -1 : 0;
}

static int Mkdir(const char *path)
{
    struct stat info;
    if (mkdir(path, 0700) == 0) return 0;
    return errno == EEXIST && stat(path, &info) == 0 && S_ISDIR(info.st_mode) ? 0 : -1;
}

static int Mkdirs(const char *path)
{
    char copy[PATH_SIZE];
    char *p;
    strcpy(copy, path);
    for (p = copy + 1; *p; ++p) {
        if (*p != '/' || p[-1] == ':') continue;
        *p = 0;
        if (Mkdir(copy) != 0) return -1;
        *p = '/';
    }
    return Mkdir(copy);
}

static int Join(char path[PATH_SIZE], const char *directory, const char *name)
{
    int n = snprintf(path, PATH_SIZE, "%s/%s", directory, name);
    return n < 0 || n >= PATH_SIZE ? -1 : 0;
}

static int Counter(const char *name, uint32_t *counter)
{
    char canonical[32], *end;
    unsigned long value;
    if (*name < '0' || *name > '9') return 0;
    errno = 0;
    value = strtoul(name, &end, 10);
    if (errno || value > UINT32_MAX || strcmp(end, ".sav")) return 0;
    snprintf(canonical, sizeof(canonical), "%" PRIu32 ".sav", (uint32_t)value);
    if (strcmp(name, canonical)) return 0;
    *counter = (uint32_t)value;
    return 1;
}

static int SyncDirectory(const char *directory)
{
#ifdef __NDS__
    /* BlocksDS FatFs rename/unlink call sync_fs before returning; directories
     * cannot be opened as file descriptors for a separate fsync. */
    (void)directory;
    return 0;
#else
    int fd = open(directory, O_RDONLY);
    int result;
    if (fd < 0) return -1;
    result = fsync(fd);
    if (close(fd) != 0) result = -1;
    return result;
#endif
}

/* Only canonical staging names belong to this module. Never remove a .sav
 * merely because it is malformed: it may share a FAT chain with a good copy. */
static int CleanTemps(const char *directory)
{
    DIR *dir = opendir(directory);
    struct dirent *entry;
    char name[32], path[PATH_SIZE];
    uint32_t counter;
    int result = 0;
    if (!dir) return -1;
    for (;;) {
        size_t length;
        errno = 0;
        entry = readdir(dir);
        if (!entry) { if (errno) result = -1; break; }
        length = strlen(entry->d_name);
        if (length <= 4 || length >= sizeof(name) || strcmp(entry->d_name + length - 4, ".tmp")) continue;
        memcpy(name, entry->d_name, length - 4); name[length - 4] = 0;
        if (!Counter(name, &counter)) continue;
        if (Join(path, directory, entry->d_name) != 0 || unlink(path) != 0) { result = -1; break; }
    }
    if (closedir(dir) != 0 || SyncDirectory(directory) != 0) result = -1;
    return result;
}

typedef struct {
    const CpBackupConfig *config;
    const char *path;
    int fd, error;
} BackupReader;

static int BackupSize(void *context, uint64_t *size)
{
    BackupReader *reader = context;
    struct stat info;
    if (fstat(reader->fd, &info) != 0) { reader->error = 1; return -1; }
    if (!S_ISREG(info.st_mode) || info.st_size < 0) return -1;
    *size = (uint64_t)info.st_size;
    return 0;
}

static int BackupRead(void *context, uint32_t offset, void *bytes, size_t size)
{
    BackupReader *reader = context;
    unsigned char *out = bytes;
    if (reader->config && reader->config->readDurable) {
        int result = reader->config->readDurable(reader->config->context, reader->path, offset, bytes, size);
        if (result != 0) reader->error = 1;
        return result;
    }
    if (lseek(reader->fd, offset, SEEK_SET) < 0) { reader->error = 1; return -1; }
    while (size) {
        ssize_t count = read(reader->fd, out, size);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { reader->error = 1; return -1; }
        out += count;
        size -= (size_t)count;
    }
    return 0;
}

static int ReadBank(void *context, int bank, unsigned char *record)
{
    return BackupRead(context, bank ? 0x70000 : 0x30000, record, WT_JOURNAL_RECORD_SIZE);
}

static int NoWrite(void *context, int bank, const unsigned char *record)
{
    (void)context; (void)bank; (void)record;
    return -1;
}

/* A plausible filename/size alone cannot establish a retained backup. */
static int ValidBackup(const CpBackupConfig *config, const char *path, const unsigned char *saveID)
{
    BackupReader reader = { config, path, -1, 0 };
    CpFile file = { &reader, BackupRead, NULL, NULL, BackupSize };
    WtJournalIO io = { &reader, ReadBank, NoWrite, CpSaveJournal_Hash };
    WtJournal journal;
    WtJournalEntry state;
    CpSaveImage *image;
    CpSaveResult loaded;
    int result = 0;
    reader.fd = open(path, O_RDONLY);
    if (reader.fd < 0) return -1;
    {
        uint64_t size;
        if (BackupSize(&reader, &size) != 0 || size != SAVE_SIZE) {
            int failed = reader.error;
            if (close(reader.fd) != 0) failed = 1;
            return failed ? -1 : 0;
        }
    }
    image = malloc(sizeof(*image));
    if (!image) { close(reader.fd); return -1; }
    loaded = CpSaveImage_Load(image, &file, CP_SAVE_MAGIC_INTERNATIONAL);
    if (loaded == CP_SAVE_INVALID) loaded = CpSaveImage_Load(image, &file, CP_SAVE_MAGIC_KOREAN);
    free(image);
    if (loaded == CP_SAVE_IO_ERROR) result = -1;
    if (loaded == CP_SAVE_OK && CpSaveJournal_Detect(&file) == CP_SAVE_JOURNAL_HAS_JOURNAL
        && WtJournal_Open(&journal, io) == 0) {
        result = WtJournal_Read(&journal, &state) == 0 && state.phase != WT_JOURNAL_EMPTY
            && (!saveID || !memcmp(state.saveID, saveID, 16));
        WtJournal_Close(&journal);
        memset(&state, 0, sizeof(state));
    }
    if (close(reader.fd) != 0 || reader.error) result = -1;
    return result;
}

/* 0 identical, 1 different, -1 I/O failure. */
static int CompareBackup(BackupReader *reader, const CpFile *save)
{
    unsigned char bytes[4096], readback[4096];
    uint64_t size;
    uint32_t offset;
    if (BackupSize(reader, &size) != 0) return -1;
    if (size != SAVE_SIZE) return 1;
    for (offset = 0; offset < SAVE_SIZE; offset += sizeof(bytes)) {
        if (save->readAt(save->context, offset, bytes, sizeof(bytes)) != 0
            || BackupRead(reader, offset, readback, sizeof(readback)) != 0) return -1;
        if (memcmp(bytes, readback, sizeof(bytes))) return 1;
    }
    return 0;
}

static int Rotate(const CpBackupConfig *config, const char *directory, uint32_t current)
{
    if (CleanTemps(directory) != 0) return -1;
    for (;;) {
        DIR *dir = opendir(directory);
        struct dirent *entry;
        struct stat info;
        char path[PATH_SIZE], oldestPath[PATH_SIZE] = "";
        uint32_t counter, oldestAge = 0;
        unsigned count = 0;
        int failed = 0;
        if (!dir) return -1;
        for (;;) {
            errno = 0;
            entry = readdir(dir);
            if (!entry) { if (errno) failed = 1; break; }
            if (!Counter(entry->d_name, &counter)) continue;
            if (Join(path, directory, entry->d_name) != 0 || stat(path, &info) != 0) { failed = 1; break; }
            if (!S_ISREG(info.st_mode) || info.st_size != SAVE_SIZE) continue;
            {
                int valid = ValidBackup(config, path, NULL);
                if (valid < 0) { failed = 1; break; }
                if (!valid) continue;
            }
            ++count;
            if (!*oldestPath || current - counter > oldestAge) {
                oldestAge = current - counter;
                strcpy(oldestPath, path);
            }
        }
        if (closedir(dir) != 0) failed = 1;
        if (failed) return -1;
        if (count <= 3) return 0;
        if (unlink(oldestPath) != 0 || SyncDirectory(directory) != 0) return -1;
    }
}

CpBackupResult CpBackup_Create(const CpBackupConfig *config, const CpFile *save,
                              const char *saveName, uint32_t globalCounter)
{
    char directory[PATH_SIZE], path[PATH_SIZE], temp[PATH_SIZE], name[32];
    unsigned char bytes[4096];
    uint64_t size, available;
    uint32_t offset;
    BackupReader reader = { config, temp, -1, 0 };
    int fd = -1, n;
    CpBackupResult result = CP_BACKUP_IO_ERROR;
    if (!config || !config->freeSpace || !save || !save->readAt || !save->size
        || Directory(directory, config, saveName) != 0
        || save->size(save->context, &size) != 0 || size != SAVE_SIZE) return CP_BACKUP_INVALID;
    if (Mkdirs(directory) != 0) return CP_BACKUP_IO_ERROR;
    if (CleanTemps(directory) != 0) return CP_BACKUP_IO_ERROR;
    snprintf(name, sizeof(name), "%" PRIu32 ".sav", globalCounter);
    if (Join(path, directory, name) != 0) return CP_BACKUP_INVALID;
    n = snprintf(temp, sizeof(temp), "%s.tmp", path);
    if (n < 0 || n >= (int)sizeof(temp)) return CP_BACKUP_INVALID;
    fd = open(path, O_RDONLY);
    if (fd >= 0) {
        reader.path = path; reader.fd = fd;
        n = CompareBackup(&reader, save);
        if (close(fd) != 0 || n < 0) return CP_BACKUP_IO_ERROR;
        if (n) return CP_BACKUP_CONFLICT;
        return Rotate(config, directory, globalCounter) == 0 ? CP_BACKUP_OK : CP_BACKUP_ROTATION_FAILED;
    }
    if (errno != ENOENT) return CP_BACKUP_IO_ERROR;
    if (config->freeSpace(config->context, Root(config), &available) != 0) return CP_BACKUP_IO_ERROR;
    if (available < SAVE_SIZE) return CP_BACKUP_NO_SPACE;
    fd = open(temp, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) return CP_BACKUP_IO_ERROR;
    for (offset = 0; offset < SAVE_SIZE; offset += sizeof(bytes)) {
        size_t written = 0;
        if (save->readAt(save->context, offset, bytes, sizeof(bytes)) != 0) goto done;
        while (written < sizeof(bytes)) {
            ssize_t count = write(fd, bytes + written, sizeof(bytes) - written);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) goto done;
            written += (size_t)count;
        }
    }
    if (fsync(fd) != 0) goto done;
    reader.fd = fd;
    if (CompareBackup(&reader, save) != 0) goto done;
    n = close(fd); fd = -1;
    if (n != 0) goto done;
    if (rename(temp, path) != 0) goto done;
    if (SyncDirectory(directory) != 0) goto done;
    result = Rotate(config, directory, globalCounter) == 0 ? CP_BACKUP_OK : CP_BACKUP_ROTATION_FAILED;
done:
    if (fd >= 0) close(fd);
    if (result != CP_BACKUP_OK && result != CP_BACKUP_ROTATION_FAILED) unlink(temp);
    return result;
}

int CpBackup_HasIdentity(const CpBackupConfig *config, const char *saveName,
                         const unsigned char saveID[16])
{
    char directory[PATH_SIZE], path[PATH_SIZE];
    DIR *dir;
    struct dirent *entry;
    unsigned i, nonzero = 0;
    uint32_t counter;
    int result = 0;
    if (!saveID || Directory(directory, config, saveName) != 0) return -1;
    for (i = 0; i < 16; ++i) nonzero |= saveID[i];
    if (!nonzero) return -1;
    dir = opendir(directory);
    if (!dir) return errno == ENOENT ? 0 : -1;
    for (;;) {
        int matches;
        errno = 0;
        entry = readdir(dir);
        if (!entry) { if (errno) result = -1; break; }
        if (!Counter(entry->d_name, &counter)) continue;
        if (Join(path, directory, entry->d_name) != 0) { result = -1; break; }
        matches = ValidBackup(config, path, saveID);
        if (matches < 0) { result = -1; break; }
        if (matches) { result = 1; break; }
    }
    if (closedir(dir) != 0) result = -1;
    return result;
}
