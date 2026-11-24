#ifndef COMPANION_APP_PLATFORM_H
#define COMPANION_APP_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

#include "game/reader.h"
#include "platform/file.h"
#include "save/backup_save.h"
#include "session/visit.h"
#include "wonder_trade/seed.h"

/* What the companion's session model (app/model.h) needs from the console:
 * the SD card, the ROMs, the worker, Wi-Fi, the network and entropy. The DS
 * supplies it in platform/nds/app_platform.c; host tests supply a fake with
 * files in a temporary folder, a synchronous worker and an in-memory server. */

/* One directory entry; size is meaningful for files only. */
typedef void (*CpAppListEntry)(void *context, const char *name, int directory, uint32_t size);

/* A job for the worker: it runs off the UI thread and returns its result. */
typedef int (*CpAppJob)(void *arg);

/* ROM slots: one for scanning headers, one for the loaded save's game. */
enum { CP_APP_ROM_SCAN, CP_APP_ROM_GAME, CP_APP_ROM_SLOTS };

typedef struct CpAppPlatform {
    void *arg;
    /* The card's root with its trailing slash: "sd:/", "fat:/", or a host folder. */
    const char *root;

    /* Call entry once per entry of a directory. Nonzero when it cannot be read. */
    int (*list)(void *arg, const char *path, CpAppListEntry entry, void *context);
    /* Open an existing save in place for reading and writing; one at a time. */
    int (*openSave)(void *arg, const char *path, CpPlatformFile *out);
    void (*closeSave)(void *arg);
    /* Open a ROM for reading into a slot. */
    int (*openRom)(void *arg, int slot, const char *path, CpReader *out);
    void (*closeRom)(void *arg, int slot);

    /* Run job on the worker. journal, when set, is the open engine journal the
     * TLS NV seed lives in for the whole job (worker.h CP_SEED_JOURNAL).
     * Zero when it started. */
    int (*start)(void *arg, CpAppJob job, void *jobArg, WtSeed *journal);
    /* 1 once the job finished, with its result; 0 while it runs. */
    int (*finished)(void *arg, int *result);
    /* Ask the running job to stop at its next wait. */
    void (*cancel)(void *arg);

    /* Join Wi-Fi with the console's saved connections; poll wifiStatus
     * (enum CpWifiStatus). Nonzero when it cannot start. */
    int (*wifiConnect)(void *arg);
    int (*wifiStatus)(void *arg);

    /* The visit's transport (CpNet_Post on the DS) and the engine's random
     * source. Both run on the worker. */
    CpTransport transport;
    int (*random)(void *arg, unsigned char *bytes, size_t size);
    /* 32 fresh bytes to seed a new journal: zero, WT_ENTROPY_WAIT while more
     * play is needed, negative on failure. */
    int (*seed)(void *arg, unsigned char out[32]);
    /* Keep the console awake while a journal bank is written. May be NULL. */
    void (*hold)(int);
    /* Where and how backups are made. */
    CpBackupConfig backup;

    /* Once per frame on the UI thread, after input was read. May be NULL. */
    void (*frame)(void *arg);
    /* One line for the companion's log, without secrets. May be NULL. */
    void (*log)(void *arg, const char *line);
} CpAppPlatform;

#endif
