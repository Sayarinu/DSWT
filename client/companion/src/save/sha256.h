#ifndef CP_SAVE_SHA256_H
#define CP_SAVE_SHA256_H

#include <stddef.h>
#include <stdint.h>

/* Internal streaming SHA-256 for the portable save layer. */
typedef struct {
    uint32_t state[8];
    uint64_t length;
    size_t used;
    unsigned char buffer[64];
} CpSaveSha256;

void CpSaveSha256_Init(CpSaveSha256 *hash);
void CpSaveSha256_Update(CpSaveSha256 *hash, const unsigned char *data, size_t size);
void CpSaveSha256_Finish(CpSaveSha256 *hash, unsigned char digest[32]);

#endif
