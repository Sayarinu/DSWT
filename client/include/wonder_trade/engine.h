#ifndef WONDER_TRADE_ENGINE_H
#define WONDER_TRADE_ENGINE_H

#include "wonder_trade/journal.h"
#include "wonder_trade/wire.h"

enum WtInventoryStep {
    WT_INVENTORY_PREPARE_REMOVE, WT_INVENTORY_REMOVE, WT_INVENTORY_VERIFY_REMOVED,
    WT_INVENTORY_PREPARE_RECEIVE, WT_INVENTORY_RECEIVE, WT_INVENTORY_VERIFY_RECEIVED,
    WT_INVENTORY_RESTORE
};

/* PREPARE only computes checked images/locators/hashes in the candidate entry.
 * REMOVE/RECEIVE/RESTORE accept only the recorded exact pre/post inventory;
 * they save BOTH retail copies and verify CRC, counters and affected bytes.
 * VERIFY checks both persistent copies, not merely the game's RAM. Nonzero
 * never authorizes a network mutation or gameplay. A full destination returns
 * 1 from PREPARE_RECEIVE and retains the received payload without an ACK.
 * Serialize the entire engine operation with transport and ordinary saves. */
typedef struct WtEngineIO {
    void *arg;
    int (*random)(void *, unsigned char *, size_t);
    int (*inventory)(void *, enum WtInventoryStep, WtJournalEntry *, unsigned slot);
} WtEngineIO;

typedef struct WtEngine {
    WtJournal journal;
    WtJournalEntry entry;
    WtEngineIO io;
    unsigned char frame[WT_WIRE_REQUEST_MAX];
    size_t frameSize;
    int synchronized, blocked;
    /* Recovery state for the pending operation; preparing a new ACK resets
     * this to zero. The decoded reply owns the current wire result. */
    int lastResult;
} WtEngine;

/* Open repairs supported mirrors. Unknown evidence and generation exhaustion
 * fail closed. An IDLE save must reconcile before a new deposit. */
int WtEngine_Open(WtEngine *, WtJournalIO, WtEngineIO);
void WtEngine_Close(WtEngine *);
int WtEngine_Enroll(WtEngine *, uint32_t build);
int WtEngine_Deposit(WtEngine *, unsigned slot);
/* Zero selects the recovery request. Pending mutation bytes always take
 * precedence. Returned frame belongs to the engine and contains a credential. */
int WtEngine_Request(WtEngine *, unsigned operation);
/* The exact outstanding frame binds the raw reply. HTTP/TLS success alone
 * cannot advance the journal. A positive return retains a pending operation. */
int WtEngine_Reply(WtEngine *, const unsigned char *, size_t);

#endif
