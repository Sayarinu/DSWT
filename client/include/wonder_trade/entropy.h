#ifndef WONDER_TRADE_ENTROPY_H
#define WONDER_TRADE_ENTROPY_H

#include <stdint.h>

#define WT_ENTROPY_REQUIRED_CHANGES 128
#define WT_ENTROPY_WAIT 1

typedef struct WtEntropySample {
    unsigned char low[32];
    uint32_t frame, keys;
    uint16_t x, y, touching;
} WtEntropySample;

typedef struct WtEntropyPool {
    unsigned char digest[32];
    uint64_t frames, lastCreditFrame;
    uint32_t keys;
    uint16_t x, y, credit;
    unsigned char touching, valid;
} WtEntropyPool;

/* Credit is a conservative input-change heuristic, not an entropy guarantee.
 * An exactly replayed boot/input/device sample stream can reproduce a seed. */
int WtEntropy_Init(WtEntropyPool *, const unsigned char boot[64]);
int WtEntropy_Frame(WtEntropyPool *, const WtEntropySample *);
/* Both outputs are cleared on failure. Seed returns WAIT until enough input. */
int WtEntropy_Seed(const WtEntropyPool *, unsigned char output[32]);
int WtEntropy_Mix(const WtEntropyPool *, const unsigned char stored[32], unsigned char output[32]);

#endif
