#ifndef WONDER_TRADE_NETWORK_H
#define WONDER_TRADE_NETWORK_H

#include <nitro.h>

#include "wonder_trade/game_adapter.h"
#include "wonder_trade/wire.h"

enum WonderTradeNetworkState {
    WT_NET_STARTING,
    WT_NET_CONNECTING,
    WT_NET_READY,
    WT_NET_PINGING,
    WT_NET_ACKNOWLEDGED,
    WT_NET_EXCHANGING,
    WT_NET_RESPONSE,
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
    WtGame *game;
    void *transport;
    void *heapBuffer;
    NNSFndHeapHandle heap;
    BOOL accessPointStarted;
    BOOL driverStopping;
    BOOL requestStarted;
    BOOL timedOut;
    WtWireReply reply;
    unsigned tradePhase;
    BOOL canLeave;
    BOOL completed, returned, rejected, waiting, superseded, needsEntropy, hasReceived;
    WtReceivedPokemon received;
} WonderTradeNetwork;

/* The game context must outlive the network, through WT_NET_CLOSED. */
void WonderTradeNetwork_Start(WonderTradeNetwork *network, WtGame *game);
void WonderTradeNetwork_Update(WonderTradeNetwork *network);
void WonderTradeNetwork_Ping(WonderTradeNetwork *network);
/* Copy an exact frame into owned worker storage. TRUE means a job started;
 * WT_NET_RESPONSE exposes a validated envelope, whose result may be non-OK.
 * The caller must first prove/persist the request's journal transition. This
 * transport does not authorize transfers or retire/rebuild pending packets. */
BOOL WonderTradeNetwork_Exchange(WonderTradeNetwork *network, const unsigned char *frame, size_t size);
/* A negative slot resumes/reconciles; a nonnegative slot confirms a deposit. */
BOOL WonderTradeNetwork_Trade(WonderTradeNetwork *, int slot);
BOOL WonderTradeNetwork_Collect(WonderTradeNetwork *, BOOL cancel);
void WonderTradeNetwork_Close(WonderTradeNetwork *network);

#endif
