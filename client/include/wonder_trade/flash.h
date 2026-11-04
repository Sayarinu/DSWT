#ifndef WONDER_TRADE_FLASH_H
#define WONDER_TRADE_FLASH_H

#include <stddef.h>

typedef struct WtFlashAccess {
    int error;
} WtFlashAccess;

/* Journal banks use ordinary save flash on hardware and any emulator. A host
 * that delays its save file can roll back behind the server after a crash. */
int WtFlash_ReadBank(void *, int, unsigned char[4096]);
int WtFlash_WriteBank(void *, int, const unsigned char[4096]);
int WtFlash_Hash(const unsigned char *, size_t, unsigned char[32]);
/* Checked raw reads for retail-copy verification; no fatal SDK load handler. */
int WtFlash_ReadRange(unsigned address, unsigned char *, size_t);

#endif
