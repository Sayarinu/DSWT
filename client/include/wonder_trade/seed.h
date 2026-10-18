#ifndef WONDER_TRADE_SEED_H
#define WONDER_TRADE_SEED_H

#include <stddef.h>
#include <stdint.h>
#include "wonder_trade/journal.h"

#define WT_JOURNAL_SIZE WT_JOURNAL_RECORD_SIZE
#define WT_SEED_SIZE 32

enum WtSeedError {
    WT_SEED_ERROR_HEADER = -30,
    WT_SEED_ERROR_FORMAT = -31,
    WT_SEED_ERROR_COMMIT = -32,
    WT_SEED_ERROR_HASH = -33,
    WT_SEED_ERROR_DIGEST = -34,
    WT_SEED_ERROR_RESERVED = -35,
    WT_SEED_ERROR_EMPTY = -36,
    WT_SEED_ERROR_UNKNOWN = -37,
    WT_SEED_ERROR_CONFLICT = -38,
    WT_SEED_ERROR_READ = -39,
    WT_SEED_ERROR_SELF_TEST = -40,
};

/* Backend writes invalidate CMIT, verify the body, write CMIT last, then read back. */
typedef int (*WtSeedReadBank)(void *arg, int bank, unsigned char record[4096]);
typedef int (*WtSeedWriteBank)(void *arg, int bank, const unsigned char record[4096]);
typedef WtJournal WtSeed;

/* This connectivity port accepts only an EMPTY journal with a provisioned seed. */
int WtSeed_Open(WtSeed *store, void *arg, WtSeedReadBank read, WtSeedWriteBank write);
int WtSeed_Read(WtSeed *store, unsigned char *seed, size_t size);
int WtSeed_Write(WtSeed *store, const unsigned char *seed, size_t size);

#endif
