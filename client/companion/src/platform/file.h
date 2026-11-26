#ifndef COMPANION_PLATFORM_FILE_H
#define COMPANION_PLATFORM_FILE_H

#include <stddef.h>
#include <stdint.h>

/* Every Generation IV save the companion opens is one 4 Mbit flash image. */
#define CP_SAVE_SIZE 524288u

/* An existing save file, opened in place: the platform never creates,
 * truncates or resizes it. Offsets are absolute; every call is bounded by
 * the save size. Each function returns 0 on success and a negative value on
 * failure. writeAt is NULL on a read-only file.
 *
 * sync makes everything written so far durable. readDurable reads what the
 * storage itself holds, past every cache, so a caller can prove a write
 * reached the card (on the DSi, raw SD sectors located through the FAT). */
typedef struct CpPlatformFile {
    void *arg;
    int (*readAt)(void *, uint32_t offset, void *, size_t);
    int (*writeAt)(void *, uint32_t offset, const void *, size_t);
    int (*sync)(void *);
    int (*size)(void *, uint32_t *);
    int (*readDurable)(void *, uint32_t offset, void *, size_t);
} CpPlatformFile;

#endif
