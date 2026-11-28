#ifndef COMPANION_TESTS_SIM_CARD_H
#define COMPANION_TESTS_SIM_CARD_H

#include "platform/nds/card_key.h"

/* A power-cycled retail card with an image behind its read commands. */
typedef struct SimCard {
    CpCardBus bus;
    const CpReader *image;
    uint32_t id, gameCode, twlStart, corruptPage, key[CP_CARD_KEY_WORDS];
    uint8_t secure[CP_CARD_SECURE];
    unsigned mode, counter, repeats, resets;
    int present, twl, seeded, delayed, changeHeader, failPage;
} SimCard;

void SimCard_Init(SimCard *, const CpReader *image, uint32_t id);

#endif
