#ifndef WONDER_TRADE_FLASH_H
#define WONDER_TRADE_FLASH_H

#include <stddef.h>

typedef struct WtFlashAccess {
    int error;
    int requireDurability;
} WtFlashAccess;

/* Read-only inspection can see ordinary saves on an unsupported emulator.
 * Every write independently requires the checked host-persistence capability. */
int WtFlash_ReadBank(void *, int, unsigned char[4096]);
int WtFlash_WriteBank(void *, int, const unsigned char[4096]);
int WtFlash_Hash(const unsigned char *, size_t, unsigned char[32]);
/* Checked raw reads for retail-copy verification; no fatal SDK load handler. */
int WtFlash_ReadRange(unsigned address, unsigned char *, size_t);

#endif
