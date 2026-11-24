#ifndef COMPANION_PLATFORM_NDS_SD_FILE_H
#define COMPANION_PLATFORM_NDS_SD_FILE_H

#include <stdint.h>

#include "platform/file.h"

#define CP_SD_PATH_MAX 256
#define CP_SD_CHAIN_MAX 2048

enum CpSdError {
    CP_SD_ERROR_OPEN = -60,
    CP_SD_ERROR_SIZE = -61,       /* not exactly CP_SAVE_SIZE bytes */
    CP_SD_ERROR_PATH = -62,
    CP_SD_ERROR_RAW = -63          /* the card's FAT could not be read directly */
};

/* A save on the SD card (sd: in DSi mode, fat: through DLDI in DS mode),
 * opened in place through FatFs. Never created, truncated or resized.
 *
 * readDurable reads raw sectors through the card's block driver
 * (get_io_dsisd() for sd:, the DLDI driver for fat:), past FatFs and the
 * BlocksDS sector cache. Each call locates the file again from the FAT
 * (directory entries and cluster chain) and checks its size there. Where the
 * FAT cannot be parsed (exFAT), it falls back to closing and reopening the
 * file and reading it again; both were proven on hardware (LUNE-860). */
typedef struct CpSdFile {
    int fd, writable, raw;
    char path[CP_SD_PATH_MAX];
    uint32_t chain[CP_SD_CHAIN_MAX], chainLength, firstCluster;
} CpSdFile;

/* Zero on success, otherwise enum CpSdError. */
int CpSdFile_Open(CpSdFile *, const char *path, int writable);
CpPlatformFile CpSdFile_Platform(CpSdFile *);
/* Zero when the raw sector path is in use, else why not (enum CpSdError). */
int CpSdFile_RawStatus(const CpSdFile *);
void CpSdFile_Close(CpSdFile *);

#endif
