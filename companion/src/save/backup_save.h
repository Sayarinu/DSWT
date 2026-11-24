#ifndef CP_BACKUP_SAVE_H
#define CP_BACKUP_SAVE_H

#include "file.h"

#define CP_BACKUP_DEFAULT_ROOT "sd:/_nds/dswt/backups"

typedef struct {
    /* NULL selects CP_BACKUP_DEFAULT_ROOT. freeSpace queries the created root. */
    const char *root;
    void *context;
    int (*freeSpace)(void *context, const char *path, uint64_t *available);
} CpBackupConfig;

typedef enum {
    CP_BACKUP_OK,
    CP_BACKUP_INVALID,
    CP_BACKUP_IO_ERROR,
    CP_BACKUP_NO_SPACE,
    CP_BACKUP_EXISTS,
    /* New backup is complete; obsolete backups could not all be removed. */
    CP_BACKUP_ROTATION_FAILED
} CpBackupResult;

/* CALLER CONTRACT: take the first backup after enrollment and BEFORE the first
 * retail edit; refuse that edit until this succeeds. Hold exclusive access to
 * the live save and backup directory throughout; use a worker with at least
 * 32 KiB of stack. This module never writes to
 * the live save. saveName is a single filename (e.g. Platinum.sav), not a path.
 * Backups are <root>/<saveName without .sav>/<globalCounter>.sav. Existing
 * counters are never overwritten. Keep the newest three using wrapping retail
 * counters, which must span less than 2^31 saves. Rotation starts only after a
 * new copy has been synced, read back, and renamed; it never deletes the last.
 * The caller must stop on any error, including ROTATION_FAILED. */
CpBackupResult CpBackup_Create(const CpBackupConfig *config, const CpFile *save,
                              const char *saveName, uint32_t globalCounter);
/* 1: a complete backup with this enrolled journal saveID exists; 0: absent;
 * -1: invalid arguments or an I/O error. EMPTY/zero identities are invalid.
 * Malformed/truncated backups and temporary files do not satisfy the check. */
int CpBackup_HasIdentity(const CpBackupConfig *config, const char *saveName,
                         const unsigned char saveID[16]);

#endif
