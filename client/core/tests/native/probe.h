#ifndef WT_NATIVE_PROBE_H
#define WT_NATIVE_PROBE_H

#include "wonder_trade/engine.h"
#include <stdio.h>

#define INVENTORY_SLOTS 6
#define INVENTORY_BODY (8 + INVENTORY_SLOTS * 236)
#define INVENTORY_SIZE (INVENTORY_BODY + 32)

/* Synthetic inventory only: no SDK save, evolution or Pokedex implementation. */
typedef struct Probe {
    int directory, lock, journal, inventory[2], full;
    WtEngine engine;
    const char *fault, *checkpoint;
    unsigned occurrence, hits;
} Probe;

int Hash(const unsigned char *, size_t, unsigned char[32]);
int HostRandom(unsigned char *, size_t);
int Checkpoint(Probe *, const char *);
const char *Phase(unsigned);
void Hex(const unsigned char *, size_t);
int OpenFiles(Probe *, const char *);
void CloseFiles(Probe *);
int Initialize(const char *, const char *);
int ReadBank(void *, int, unsigned char[4096]);
int WriteBank(void *, int, const unsigned char[4096]);
int Inventory(void *, enum WtInventoryStep, WtJournalEntry *, unsigned);
int Report(Probe *, int);
int Exchange(Probe *, unsigned, const char *, const unsigned char[2][32]);

#endif
