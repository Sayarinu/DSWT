/* The host test adapter is excluded from the ROM's recursive source list. */
#ifndef __NDS__
#define _POSIX_C_SOURCE 200809L
#include "posix_file.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static int Size(void *context, uint64_t *size)
{
    struct stat status;
    if (fstat(((CpPosixFile *)context)->fd, &status) != 0
        || !S_ISREG(status.st_mode) || status.st_size < 0) return -1;
    *size = (uint64_t)status.st_size;
    return 0;
}

static int Transfer(void *context, uint32_t offset, void *data, size_t size, int write)
{
    CpPosixFile *file = context;
    uint64_t length;
    unsigned char *bytes = data;
    int flags = fcntl(file->fd, F_GETFL);
    if (flags < 0 || (flags & O_APPEND) || Size(context, &length) != 0
        || offset > length || size > length - offset || size > UINT32_MAX - offset) return -1;
    while (size != 0) {
        ssize_t count = write ? pwrite(file->fd, bytes, size, (off_t)offset)
                              : pread(file->fd, bytes, size, (off_t)offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return -1;
        size -= (size_t)count;
        bytes += count;
        offset += (uint32_t)count;
    }
    return 0;
}

static int ReadAt(void *context, uint32_t offset, void *data, size_t size)
{
    return Transfer(context, offset, data, size, 0);
}

static int WriteAt(void *context, uint32_t offset, const void *data, size_t size)
{
    return Transfer(context, offset, (void *)data, size, 1);
}

static int Sync(void *context)
{
    int result;
    do {
        result = fsync(((CpPosixFile *)context)->fd);
    } while (result < 0 && errno == EINTR);
    return result;
}

int CpPosixFile_Init(CpFile *file, CpPosixFile *context, int fd)
{
    uint64_t size;
    int flags;
    if (file == NULL || context == NULL) return -1;
    context->fd = fd;
    flags = fcntl(fd, F_GETFL);
    if (flags < 0 || (flags & O_APPEND) || Size(context, &size) != 0) return -1;
    file->context = context;
    file->readAt = ReadAt;
    file->writeAt = WriteAt;
    file->sync = Sync;
    file->size = Size;
    return 0;
}
#endif
