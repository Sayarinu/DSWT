#ifndef COMPANION_TESTS_POSIX_READER_H
#define COMPANION_TESTS_POSIX_READER_H

#include "game/reader.h"

#include <stdio.h>

/* A CpReader over a host file, standing in for the platform's SD card reader. */
typedef struct PosixReader {
    CpReader reader;
    FILE *file;
    uint32_t size;
} PosixReader;

int PosixReader_Open(PosixReader *, const char *path);
void PosixReader_Close(PosixReader *);

#endif
