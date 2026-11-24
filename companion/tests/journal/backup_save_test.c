#define _DARWIN_C_SOURCE

#define _POSIX_C_SOURCE 200809L
#include "save/backup_save.h"
#include "save/journal_save.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned char bytes[0x80000], snapshot[0x80000];
static int reads, failRead, queryFailure;
static uint64_t available = sizeof(bytes);
static char sabotage[1024];
static int Read(void *arg, uint32_t offset, void *data, size_t size)
{
    (void)arg;
    if (++reads == failRead) return -1;
    if (*sabotage && reads == 128) assert(mkdir(sabotage, 0700) == 0);
    memcpy(data, bytes + offset, size);
    return 0;
}
static int Size(void *arg, uint64_t *size) { (void)arg; *size = sizeof(bytes); return 0; }
static int FreeSpace(void *arg, const char *path, uint64_t *size)
{
    struct stat info;
    (void)arg;
    assert(stat(path, &info) == 0 && S_ISDIR(info.st_mode));
    *size = available;
    return queryFailure ? -1 : 0;
}
static CpFile file = { NULL, Read, NULL, NULL, Size };
static const unsigned char identity[16] = { 1, 2, 3 };

static void Enrolled(void)
{
    WtJournalEntry entry;
    unsigned char record[4096];
    memset(&entry, 0, sizeof(entry));
    entry.phase = WT_JOURNAL_IDLE; entry.generation = 3; entry.nextCounter = 1;
    memset(entry.seed, 7, 32);
    memcpy(entry.saveID, identity, 16);
    memset(entry.credential, 8, 32); memset(entry.enrollmentID, 9, 16);
    assert(WtJournal_Encode(&entry, CpJournal_Hash, record) == 0);
    memset(bytes, 0, sizeof(bytes));
    memcpy(bytes + 0x30000, record, sizeof(record));
    memcpy(bytes + 0x70000, record, sizeof(record));
    memcpy(snapshot, bytes, sizeof(bytes));
}
static int Exists(const char *root, const char *name)
{
    char path[1024];
    struct stat info;
    assert(snprintf(path, sizeof(path), "%s/Platinum/%s", root, name) < (int)sizeof(path));
    return stat(path, &info) == 0;
}
static void VerifyCopy(const char *root, const char *name)
{
    char path[1024];
    unsigned char actual[4096];
    size_t offset;
    FILE *input;
    snprintf(path, sizeof(path), "%s/Platinum/%s", root, name);
    input = fopen(path, "rb"); assert(input);
    for (offset = 0; offset < sizeof(bytes); offset += sizeof(actual)) {
        assert(fread(actual, 1, sizeof(actual), input) == sizeof(actual));
        assert(!memcmp(actual, bytes + offset, sizeof(actual)));
    }
    assert(fgetc(input) == EOF && !ferror(input));
    assert(fclose(input) == 0);
}
static void Cleanup(const char *root)
{
    char directory[1024], path[1024];
    struct dirent *entry;
    DIR *dir;
    snprintf(directory, sizeof(directory), "%s/Platinum", root);
    dir = opendir(directory); assert(dir);
    while ((entry = readdir(dir)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        assert(snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name) < (int)sizeof(path));
        assert(unlink(path) == 0);
    }
    assert(closedir(dir) == 0 && rmdir(directory) == 0 && rmdir(root) == 0);
}

int main(void)
{
    char root[] = "/tmp/dswt-backup-XXXXXX";
    char path[1024];
    unsigned char other[16] = { 42 }, zero[16] = { 0 };
    CpBackupConfig config = { root, NULL, FreeSpace };
    FILE *output;
    unsigned counter;
    assert(mkdtemp(root));
    Enrolled();
    assert(CpBackup_HasIdentity(&config, "Platinum.sav", identity) == 0);
    assert(CpBackup_HasIdentity(&config, "Platinum.sav", zero) == -1);
    assert(CpBackup_Create(&config, &file, "../Platinum.sav", 1) == CP_BACKUP_INVALID);
    assert(CpBackup_Create(&config, &file, "..", 1) == CP_BACKUP_INVALID);
    assert(CpBackup_Create(&config, &file, "sd:/Platinum.sav", 1) == CP_BACKUP_INVALID);
    assert(CpBackup_Create(&config, &file, "Platinum.sav", 10) == CP_BACKUP_OK);
    VerifyCopy(root, "10.sav");
    assert(CpBackup_HasIdentity(&config, "Platinum.sav", identity) == 1);
    assert(CpBackup_HasIdentity(&config, "Platinum.sav", other) == 0);
    assert(CpBackup_Create(&config, &file, "Platinum.sav", 10) == CP_BACKUP_EXISTS);
    VerifyCopy(root, "10.sav");
    assert(Exists(root, "10.sav") && !Exists(root, "10.sav.tmp"));
    available--;
    assert(CpBackup_Create(&config, &file, "Platinum.sav", 11) == CP_BACKUP_NO_SPACE);
    assert(Exists(root, "10.sav") && !Exists(root, "11.sav.tmp"));
    available++;
    queryFailure = 1;
    assert(CpBackup_Create(&config, &file, "Platinum.sav", 11) == CP_BACKUP_IO_ERROR);
    queryFailure = 0;
    /* A stale temp belongs to an earlier attempt and is never truncated. */
    snprintf(path, sizeof(path), "%s/Platinum/11.sav.tmp", root);
    output = fopen(path, "wb"); assert(output && fputs("keep", output) >= 0 && fclose(output) == 0);
    assert(CpBackup_Create(&config, &file, "Platinum.sav", 11) == CP_BACKUP_IO_ERROR);
    assert(Exists(root, "11.sav.tmp") && unlink(path) == 0);
    reads = 0; failRead = 2;
    assert(CpBackup_Create(&config, &file, "Platinum.sav", 11) == CP_BACKUP_IO_ERROR);
    assert(!Exists(root, "11.sav") && !Exists(root, "11.sav.tmp") && Exists(root, "10.sav"));
    reads = 0; failRead = 130; /* Read-back phase fails after all bytes were written. */
    assert(CpBackup_Create(&config, &file, "Platinum.sav", 11) == CP_BACKUP_IO_ERROR);
    assert(!Exists(root, "11.sav") && !Exists(root, "11.sav.tmp"));
    failRead = 0; reads = 0;
    snprintf(sabotage, sizeof(sabotage), "%s/Platinum/11.sav", root);
    assert(CpBackup_Create(&config, &file, "Platinum.sav", 11) == CP_BACKUP_IO_ERROR);
    assert(!Exists(root, "11.sav.tmp") && Exists(root, "10.sav"));
    assert(rmdir(sabotage) == 0); *sabotage = 0;
    for (counter = 11; counter <= 14; ++counter)
        assert(CpBackup_Create(&config, &file, "Platinum.sav", counter) == CP_BACKUP_OK);
    assert(!Exists(root, "10.sav") && !Exists(root, "11.sav"));
    assert(Exists(root, "12.sav") && Exists(root, "13.sav") && Exists(root, "14.sav"));
    assert(!memcmp(snapshot, bytes, sizeof(bytes)));
    /* Temporary and malformed files cannot establish a backup for an identity. */
    snprintf(path, sizeof(path), "%s/Platinum/999.sav.tmp", root);
    output = fopen(path, "wb"); assert(output);
    assert(fwrite(bytes, 1, sizeof(bytes), output) == sizeof(bytes) && fclose(output) == 0);
    for (counter = 12; counter <= 14; ++counter) {
        snprintf(path, sizeof(path), "%s/Platinum/%u.sav", root, counter);
        assert(unlink(path) == 0);
    }
    snprintf(path, sizeof(path), "%s/Platinum/999.sav", root);
    output = fopen(path, "wb"); assert(output && fclose(output) == 0);
    assert(CpBackup_HasIdentity(&config, "Platinum.sav", identity) == 0);
    assert(unlink(path) == 0);
    /* Overflowing names and short files cannot displace the only good copy. */
    snprintf(path, sizeof(path), "%s/Platinum/99999999999999999999999999.sav", root);
    output = fopen(path, "wb"); assert(output && fclose(output) == 0);
    for (counter = 99; counter <= 102; ++counter) {
        snprintf(path, sizeof(path), "%s/Platinum/%u.sav", root, counter);
        output = fopen(path, "wb"); assert(output && fclose(output) == 0);
    }
    assert(CpBackup_HasIdentity(&config, "Platinum.sav", identity) == 0);
    /* Retail global counter wrap retains the three most recent copies. */
    assert(CpBackup_Create(&config, &file, "Platinum.sav", UINT32_MAX - 1) == CP_BACKUP_OK);
    assert(CpBackup_Create(&config, &file, "Platinum.sav", UINT32_MAX) == CP_BACKUP_OK);
    assert(CpBackup_Create(&config, &file, "Platinum.sav", 0) == CP_BACKUP_OK);
    assert(CpBackup_Create(&config, &file, "Platinum.sav", 1) == CP_BACKUP_OK);
    assert(!Exists(root, "4294967294.sav") && Exists(root, "4294967295.sav"));
    assert(Exists(root, "0.sav") && Exists(root, "1.sav"));
    Cleanup(root);
    puts("PASS backup naming, byte-identical copy, identity, newest-three rotation and counter wrap");
    puts("PASS free-space/query refusal, exclusive creation, write/read-back/rename failure cleanup and sole-backup preservation");
    return 0;
}
