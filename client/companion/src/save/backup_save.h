#ifndef CP_BACKUP_SAVE_H
#define CP_BACKUP_SAVE_H

#include "file.h"

#define CP_BACKUP_DEFAULT_ROOT "sd:/_nds/dswt/backups"

typedef struct {
    /* NULL selects CP_BACKUP_DEFAULT_ROOT. freeSpace queries the created root. */
    const char *root;
    void *context;
    int (*freeSpace)(void *context, const char *path, uint64_t *available);
    /* Optional: read this backup path from storage, bypassing every cache.
     * Required for DSi durability proof; NULL uses host read() after fsync(). */
    int (*readDurable)(void *context, const char *path, uint32_t offset,
                       void *bytes, size_t size);
} CpBackupConfig;

typedef enum {
    CP_BACKUP_OK,
    CP_BACKUP_INVALID,
    CP_BACKUP_IO_ERROR,
    CP_BACKUP_NO_SPACE,
    CP_BACKUP_CONFLICT,
    /* New backup is complete; obsolete backups could not all be removed. */
    CP_BACKUP_ROTATION_FAILED
} CpBackupResult;

/* CALLER CONTRACT: take the first backup after enrollment and BEFORE the first
 * retail edit; refuse that edit until this succeeds. Hold exclusive access to
 * the live save and backup directory throughout; use a worker with at least
 * 32 KiB of stack. This module never writes to
 * the live save. saveName is a single filename (e.g. Platinum.sav), not a path.
 * Backups are <root>/<saveName without .sav>/<globalCounter>.sav. Existing
 * counters are never overwritten: identical content succeeds, differing content
 * returns CONFLICT. Keep the newest three using wrapping retail
 * counters, which must span less than 2^31 saves. Rotation starts only after a
 * new copy has been synced, read back, and renamed; it never deletes the last.
 * Stale <counter>.sav.tmp files are removed before creation and rotation.
 * FatFs f_rename is not atomic: a cut can leave two directory entries sharing
 * one cluster chain. Removing a temp/old entry may invalidate its duplicate;
 * rotation and identity lookup ignore copies failing retail/journal validation.
 * They cannot repair FAT damage. The caller must stop on any error, including
 * ROTATION_FAILED. */
CpBackupResult CpBackup_Create(const CpBackupConfig *config, const CpFile *save,
                              const char *saveName, uint32_t globalCounter);
/* 1: a complete backup with this enrolled journal saveID exists; 0: absent;
 * -1: invalid arguments or an I/O error. EMPTY/zero identities are invalid.
 * Malformed/truncated backups and temporary files do not satisfy the check.
 * Validation uses the retail loader of any supported layout (international/Korean)
 * and a committed enrolled journal, with 512 KiB of temporary heap storage. */
int CpBackup_HasIdentity(const CpBackupConfig *config, const char *saveName,
                         const unsigned char saveID[16]);

#endif
