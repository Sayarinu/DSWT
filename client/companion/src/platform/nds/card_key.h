#ifndef COMPANION_NDS_CARD_KEY_H
#define COMPANION_NDS_CARD_KEY_H

#include "platform/nds/card_rom.h"

/* Expand a caller-supplied BIOS table. The console and cartridge use the
 * game code as a little-endian word. */
void CpCardKey_Init(uint32_t table[CP_CARD_KEY_WORDS], uint32_t gameCode, unsigned level);
uint64_t CpCardKey_Crypt(const uint32_t table[CP_CARD_KEY_WORDS], uint64_t value, int decrypt);

#endif
