#ifndef COMPANION_PLATFORM_ENTROPY_POOL_H
#define COMPANION_PLATFORM_ENTROPY_POOL_H

#include <stddef.h>

#include "wonder_trade/entropy.h"
#include "wonder_trade/journal.h"

/* The companion's one entropy pool (wonder_trade/entropy.h), fed by the
 * platform: a boot sample once, then one input sample per frame. Nothing here
 * is a hardware random number generator; the DSi has none. The core's credit
 * rule decides when there is enough input for a new seed.
 *
 * Threads are cooperative (cothreads), so the pool needs no lock as long as
 * only the main thread calls CpEntropy_Frame and no interrupt handler calls
 * any of these. */

#define CP_ENTROPY_BOOT_MAX 1024

/* Boot sample bytes: timer readings, the clock, the MAC address, a console
 * ID, microphone samples. Past CP_ENTROPY_BOOT_MAX bytes, more input is
 * folded into what is already there rather than dropped. */
typedef struct CpEntropyBoot {
    unsigned char bytes[CP_ENTROPY_BOOT_MAX];
    size_t size, total;
} CpEntropyBoot;

void CpEntropyBoot_Start(CpEntropyBoot *);
void CpEntropyBoot_Add(CpEntropyBoot *, const void *, size_t);
/* Hash the sample into WtEntropy_Init's 64 bytes and start the pool. The
 * sample is cleared either way. Zero on success. */
int CpEntropyBoot_Finish(CpEntropyBoot *, WtJournalHash);

int CpEntropy_Frame(const WtEntropySample *);
/* WtEntropy_Seed and WtEntropy_Mix on the current pool. */
int CpEntropy_Seed(unsigned char output[32]);
int CpEntropy_Mix(unsigned char seed[32]);
/* Nonzero once the boot sample started the pool and no frame has failed. */
int CpEntropy_Ready(void);
/* Consume one computer-generated seed if fresh input is still needed. */
int CpEntropy_SeedFromFile(const char *path, WtJournalHash hash, unsigned char output[32]);

#endif
