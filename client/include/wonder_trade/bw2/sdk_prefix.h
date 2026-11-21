#ifndef WONDER_TRADE_BW2_SDK_PREFIX_H
#define WONDER_TRADE_BW2_SDK_PREFIX_H

/* Included first in every Wonder Trade unit built for Black 2 and White 2:
 * the core is built for their family (wonder_trade/game_family.h). */
#define WT_GAME_FAMILY WT_FAMILY_B2W2

#include <nitro.h>
#include "nnsys/fnd.h"

/* NitroSystem's expanded heap with the default options. */
#define NNS_FndCreateExpHeap(address, size) NNS_FndCreateExpHeapEx((address), (size), 0)

/* TwlSDK keeps a thread's bottom guard word 8 bytes before its stack bottom. */
#define WT_STACK_GUARD_BYTES (sizeof(u32) + 8)

#endif
