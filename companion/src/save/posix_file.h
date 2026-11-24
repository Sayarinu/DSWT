#ifndef CP_POSIX_FILE_H
#define CP_POSIX_FILE_H

#include "file.h"

typedef struct {
    int fd;
} CpPosixFile;

/* Borrows an already-open regular file; never opens, closes or resizes it.
 * O_APPEND is refused because it can override pwrite's offset on some hosts. */
int CpPosixFile_Init(CpFile *file, CpPosixFile *context, int fd);

#endif
