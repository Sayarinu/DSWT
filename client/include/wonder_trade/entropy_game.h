#ifndef POKEPLATINUM_WONDER_TRADE_ENTROPY_GAME_H
#define POKEPLATINUM_WONDER_TRADE_ENTROPY_GAME_H

#include <stddef.h>

void WonderTradeEntropy_Init(void);
void WonderTradeEntropy_Frame(void);
int WonderTradeEntropy_Seed(unsigned char[32]);
int WonderTradeEntropy_Mix(unsigned char *, size_t);

#endif
