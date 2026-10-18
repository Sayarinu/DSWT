#ifndef WONDER_TRADE_SEED_H
#define WONDER_TRADE_SEED_H

#include <stddef.h>
#include <stdint.h>

#define WT_JOURNAL_SIZE 4096
#define WT_SEED_SIZE 32

/* Backend writes must verify the body, program CMIT last, then read back. */
typedef int (*WtSeedReadBank)(void *arg, int bank, unsigned char record[4096]);
typedef int (*WtSeedWriteBank)(void *arg, int bank, const unsigned char record[4096]);
typedef struct WtSeed {
    unsigned char record[WT_JOURNAL_SIZE];
    void *arg;
    WtSeedWriteBank write;
    int selected;
    int usable;
} WtSeed;

/* This connectivity port accepts only an EMPTY journal with a provisioned seed. */
int WtSeed_Open(WtSeed *store, void *arg, WtSeedReadBank read, WtSeedWriteBank write);
int WtSeed_Read(WtSeed *store, unsigned char *seed, size_t size);
int WtSeed_Write(WtSeed *store, const unsigned char *seed, size_t size);

#endif
