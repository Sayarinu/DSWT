#ifndef CP_FILE_H
#define CP_FILE_H

#include <stddef.h>
#include <stdint.h>

/* Operations transfer the entire range and return zero on success. sync must
 * make preceding writes durable. The caller holds exclusive access throughout
 * load/edit/commit and keeps the file open and its size unchanged. */
typedef struct {
    void *context;
    int (*readAt)(void *context, uint32_t offset, void *data, size_t size);
    int (*writeAt)(void *context, uint32_t offset, const void *data, size_t size);
    int (*sync)(void *context);
    int (*size)(void *context, uint64_t *size);
} CpFile;

#endif
