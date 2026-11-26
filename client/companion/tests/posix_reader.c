#include "posix_reader.h"

static int ReadAt(void *context, uint32_t offset, void *destination, uint32_t size)
{
    PosixReader *self = context;

    if (offset > self->size || size > self->size - offset) return -1;
    if (fseek(self->file, (long)offset, SEEK_SET) != 0) return -1;
    return fread(destination, 1, size, self->file) == size ? 0 : -1;
}

static uint32_t Size(void *context)
{
    return ((PosixReader *)context)->size;
}

int PosixReader_Open(PosixReader *self, const char *path)
{
    long size;

    if (!(self->file = fopen(path, "rb"))) return -1;
    if (fseek(self->file, 0, SEEK_END) != 0 || (size = ftell(self->file)) < 0 || size > 0xFFFFFFFFL) {
        fclose(self->file);
        return -1;
    }
    self->size = (uint32_t)size;
    self->reader.context = self;
    self->reader.readAt = ReadAt;
    self->reader.size = Size;
    return 0;
}

void PosixReader_Close(PosixReader *self)
{
    fclose(self->file);
}
