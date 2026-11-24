#ifndef COMPANION_SESSION_VISIT_H
#define COMPANION_SESSION_VISIT_H

#include <stddef.h>
#include <stdint.h>

#include "wonder_trade/game_adapter.h"
#include "wonder_trade/session.h"

/* One visit to the Wonder Trade server, as the patched game's network worker
 * runs it (client/src/wonder_trade/network.c RunTrade), on WtSession_Next.
 * The transport is injected: CpNet_Post on the DS, a fake server in tests. */

/* POST one request frame and return the reply body: zero on success with
 * replySize set (at most WT_WIRE_REPLY_MAX), nonzero on a transport failure. */
typedef struct CpTransport {
    void *arg;
    int (*post)(void *arg, const unsigned char *request, size_t size,
        unsigned char reply[WT_WIRE_REPLY_MAX], size_t *replySize);
    /* Wait while the server finishes a request (PROCESSING). May be NULL. */
    void (*wait)(void *arg, unsigned milliseconds);
    /* Polled between requests; nonzero stops the visit. May be NULL. */
    const volatile int *cancel;
    /* Called before every exchange, ahead of its journal read and request:
     * nonzero stops the visit with CP_VISIT_ERROR_CRYPTO. On the DS this
     * starts PSA crypto again (psa_crypto_init), as the game's RunTrade does:
     * closing TLS frees it, and starting it reads and advances the NV seed
     * through the open engine journal, which must happen before the entry is
     * read for the next request. May be NULL. */
    int (*prepare)(void *arg);
} CpTransport;

enum CpVisitError {
    CP_VISIT_ERROR_JOURNAL = -30,     /* the engine blocked or the journal failed */
    CP_VISIT_ERROR_CRYPTO = -31,      /* prepare failed: no random numbers */
    CP_VISIT_ERROR_DEPOSIT = -32,     /* the offer could not be escrowed */
    CP_VISIT_ERROR_LOOP = -33,        /* too many exchanges in one visit */
    CP_VISIT_ERROR_TRANSPORT = -34,
    CP_VISIT_ERROR_CANCELLED = -15,
    CP_VISIT_ERROR_ROLLED_BACK = -35, /* an older copy of this save: it may never trade again */
    CP_VISIT_ERROR_CHECKPOINT = -3200 /* minus the checkpoint's own result */
};

typedef struct CpVisit {
    int result;
    /* What happened, for the screens: the network worker's flags. */
    int completed, returned, rejected, waiting, superseded, refused, rolledBack;
    unsigned char refusal; /* enum WtWireRefusal, when the server said */
    /* Whether gameplay may resume on this save: the patched game's CanLeave. */
    int canLeave;
    uint16_t phase;
} CpVisit;

/* Run one visit on an open engine (WtEngine_Open with the save's journal and
 * an engine IO whose argument is game). An EMPTY journal is enrolled with
 * build; slot >= 0 offers that party slot once the journal is IDLE and in
 * step with the server. action is zero (an ordinary visit: reconcile or
 * status), WT_OP_COLLECT or WT_OP_CANCEL. Returns visit->result. After a
 * failure the engine may still hold its request: close it, and open it again
 * for the next visit, which resends what the journal kept. */
int CpVisit_Run(WtEngine *engine, WtGame *game, uint32_t build, int slot, unsigned action,
    const CpTransport *transport, CpVisit *visit);

#endif
