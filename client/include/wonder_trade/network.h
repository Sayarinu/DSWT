#ifndef POKEPLATINUM_WONDER_TRADE_NETWORK_H
#define POKEPLATINUM_WONDER_TRADE_NETWORK_H

#include <nitro.h>

#include "constants/heap.h"

enum WonderTradeNetworkState {
    WT_NET_STARTING,
    WT_NET_CONNECTING,
    WT_NET_READY,
    WT_NET_PINGING,
    WT_NET_ACKNOWLEDGED,
    WT_NET_ERROR,
    WT_NET_CLOSING,
    WT_NET_CLOSED,
};

typedef struct WonderTradeNetwork {
    int state;
    int error;
    OSTick deadline;
    OSTick lastUpdate;
    u32 handshakeMs;
    u32 tlsPeakBytes;
    u32 stackBytes;
    u32 maxFrameMs;
    enum HeapID heapID;
    void *transport;
    void *heapBuffer;
    NNSFndHeapHandle heap;
    BOOL accessPointStarted;
    BOOL driverStopping;
} WonderTradeNetwork;

void WonderTradeNetwork_Start(WonderTradeNetwork *network, enum HeapID heapID);
void WonderTradeNetwork_Update(WonderTradeNetwork *network);
void WonderTradeNetwork_Ping(WonderTradeNetwork *network);
void WonderTradeNetwork_Close(WonderTradeNetwork *network);

#endif
