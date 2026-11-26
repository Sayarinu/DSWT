#ifndef WONDER_TRADE_BW_SDK_NNSYS_FND_H
#define WONDER_TRADE_BW_SDK_NNSYS_FND_H

/* NitroSystem's expanded heap in ARM9 main. */

#include <nitro.h>

typedef void *NNSFndHeapHandle;

NNSFndHeapHandle NNS_FndCreateExpHeapEx(void *startAddress, u32 size, u16 optFlag);
void *NNS_FndAllocFromExpHeapEx(NNSFndHeapHandle heap, u32 size, int alignment);
void NNS_FndFreeToExpHeap(NNSFndHeapHandle heap, void *memBlock);
/* NNS_FndDestroyExpHeap as aletheia names it. */
void NNSi_FndDestroyExpHeap(NNSFndHeapHandle heap);
#define NNS_FndDestroyExpHeap(heap) NNSi_FndDestroyExpHeap(heap)

#endif
