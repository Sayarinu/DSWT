#ifndef WONDER_TRADE_SESSION_H
#define WONDER_TRADE_SESSION_H

#include "wonder_trade/engine.h"

enum WtSessionResult { WT_SESSION_BLOCKED = -1, WT_SESSION_DONE = 0 };

/* Prepare the next request and return its WT_OP_* value, or DONE/BLOCKED.
 * DONE ends this visit, including when storage is full; it need not mean the
 * trade is complete. requested is zero (ordinary visit), COLLECT or CANCEL.
 * The caller opens/refreshes the engine, enrolls an EMPTY journal and deposits
 * any selected offer before calling. The caller must stop a superseded copy
 * at IDLE before calling, even when the engine is not synchronized.
 * Exchange engine->frame, then apply the reply with WtEngine_Reply before
 * calling again. Serialize with saves and IO. */
int WtSession_Next(WtEngine *engine, unsigned requested);

#endif
