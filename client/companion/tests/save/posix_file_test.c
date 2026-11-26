#define _POSIX_C_SOURCE 200809L
#include "save/posix_file.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    char path[] = "/tmp/dswt-cp-file-XXXXXX";
    const unsigned char original[] = { 1, 2, 3, 4, 5 };
    unsigned char actual[5], replacement = 9;
    uint64_t size;
    CpPosixFile context;
    CpFile file;
    int fd = mkstemp(path), readonly, flags;
    assert(fd >= 0);
    assert(write(fd, original, sizeof(original)) == sizeof(original));
    assert(lseek(fd, 3, SEEK_SET) == 3);
    assert(CpPosixFile_Init(&file, &context, fd) == 0);
    assert(file.size(file.context, &size) == 0 && size == 5);
    assert(file.readAt(file.context, 0, actual, sizeof(actual)) == 0);
    assert(memcmp(original, actual, 5) == 0);
    assert(file.writeAt(file.context, 2, &replacement, 1) == 0);
    assert(file.sync(file.context) == 0);
    assert(lseek(fd, 0, SEEK_CUR) == 3);
    assert(file.readAt(file.context, 2, actual, 1) == 0 && actual[0] == 9);
    assert(file.writeAt(file.context, 5, &replacement, 1) == -1);
    assert(file.writeAt(file.context, 4, original, 2) == -1);
    assert(file.writeAt(file.context, UINT32_MAX, original, 1) == -1);
    assert(file.readAt(file.context, 4, actual, 2) == -1);
    assert(file.readAt(file.context, 6, actual, 0) == -1);
    assert(file.readAt(file.context, 5, actual, 0) == 0);
    assert(file.writeAt(file.context, 5, original, 0) == 0);
    assert(file.size(file.context, &size) == 0 && size == 5);
    flags = fcntl(fd, F_GETFL);
    assert(flags >= 0 && fcntl(fd, F_SETFL, flags | O_APPEND) == 0);
    assert(file.writeAt(file.context, 0, original, 1) == -1);
    assert(CpPosixFile_Init(&file, &context, fd) == -1);
    assert(fcntl(fd, F_SETFL, flags) == 0);
    readonly = open(path, O_RDONLY);
    assert(readonly >= 0 && CpPosixFile_Init(&file, &context, readonly) == 0);
    assert(file.readAt(file.context, 0, actual, 5) == 0);
    assert(file.writeAt(file.context, 0, original, 1) == -1);
    assert(close(readonly) == 0);
    assert(file.readAt(file.context, 0, actual, 1) == -1);
    assert(file.sync(file.context) == -1);
    assert(CpPosixFile_Init(&file, &context, -1) == -1);
    assert(CpPosixFile_Init(NULL, &context, fd) == -1);
    assert(CpPosixFile_Init(&file, NULL, fd) == -1);
    assert(close(fd) == 0 && unlink(path) == 0);
    puts("PASS POSIX: positioned I/O, fsync, fixed extent, append/read-only/closed descriptor failures");
    return 0;
}
