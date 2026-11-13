#ifndef WONDER_TRADE_ENGINE_H
#define WONDER_TRADE_ENGINE_H

#include "wonder_trade/journal.h"
#include "wonder_trade/wire.h"

/* How long a client resends a request while the server waits for its backup to
 * cover the outcome, before giving up to the normal retry screen. */
#define WT_ENGINE_PROCESSING_RETRIES 30
#define WT_ENGINE_PROCESSING_WAIT_MS 1000

enum WtInventoryStep {
    WT_INVENTORY_PREPARE_REMOVE, WT_INVENTORY_REMOVE, WT_INVENTORY_VERIFY_REMOVED,
    WT_INVENTORY_PREPARE_RECEIVE, WT_INVENTORY_RECEIVE, WT_INVENTORY_VERIFY_RECEIVED,
    WT_INVENTORY_RESTORE,
    /* 0 when RESTORE can put the escrowed Pokémon back exactly (the save is
     * unchanged since the escrow), 1 when the save has changed since. */
    WT_INVENTORY_PREPARE_RESTORE
};

/* PREPARE only computes checked images/locators/hashes in the candidate entry.
 * REMOVE/RECEIVE/RESTORE accept only the recorded exact pre/post inventory;
 * they save BOTH retail copies and verify CRC, counters and affected bytes.
 * VERIFY checks both persistent copies, not merely the game's RAM. Nonzero
 * never authorizes a network mutation or gameplay. A full destination returns
 * 1 from PREPARE_RECEIVE and retains the received payload without an ACK.
 * canHold returns nonzero when the game can hold a matched delivery's
 * canonical PK4; zero refuses it before the journal records it.
 * Serialize the entire engine operation with transport and ordinary saves. */
typedef struct WtEngineIO {
    void *arg;
    int (*random)(void *, unsigned char *, size_t);
    int (*inventory)(void *, enum WtInventoryStep, WtJournalEntry *, unsigned slot);
    int (*canHold)(void *, const unsigned char[136]);
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
    /* Set when the last reply was a Collect whose Pokémon this game cannot
     * hold: nothing was recorded and the trade stays QUEUED for a later Collect. */
    int refused;
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
/* Finish what needs no network or randomness, before gameplay: complete an
 * interrupted deposit's removal (its Submit is sent on the next visit),
 * restore a rejected offer, and apply a collected delivery. 1 keeps a delivery or return in the journal
 * while storage is full. Everything else waits for the next visit. */
int WtEngine_Settle(WtEngine *);
/* The exact outstanding frame binds the raw reply. HTTP/TLS success alone
 * cannot advance the journal. A positive return retains a pending operation,
 * including a refused delivery (see refused). */
int WtEngine_Reply(WtEngine *, const unsigned char *, size_t);

#endif
