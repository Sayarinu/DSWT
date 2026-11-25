#include "wonder_trade/engine.h"

#include <stddef.h>
#include <string.h>

static int Zero(const unsigned char *bytes, size_t size)
{
    unsigned char value = 0;
    size_t i;
    for (i = 0; i < size; ++i) value |= bytes[i];
    return value == 0;
}

static void Clear(void *arg, size_t size)
{
    volatile unsigned char *bytes = arg;
    while (size--) *bytes++ = 0;
}

static int Block(WtEngine *engine)
{
    engine->blocked = 1;
    Clear(engine->frame, sizeof(engine->frame)); engine->frameSize = 0;
    return -1;
}

static int Refresh(WtEngine *engine)
{
    if (!engine || engine->blocked) return -1;
    /* NV-seed updates share this serialized journal handle. */
    if (WtJournal_Read(&engine->journal, &engine->entry) != 0
        || engine->entry.generation == UINT64_MAX) return Block(engine);
    return 0;
}

static int StateKnown(const WtJournalEntry *entry, WtWireHash hash)
{
    unsigned operation = entry->requestLength ? entry->request[5] : 0;
    if (entry->requestLength && WtWire_CheckRequest(entry->request, entry->requestLength, hash) != 0) return 0;
    switch (entry->phase) {
    case WT_JOURNAL_EMPTY: case WT_JOURNAL_IDLE: case WT_JOURNAL_RECEIVED:
    case WT_JOURNAL_APPLYING: case WT_JOURNAL_RESTORING: return operation == 0;
    case WT_JOURNAL_ENROLLING: return operation == WT_OP_ENROLL;
#ifdef WT_ABANDON_UNSENT
    /* An escrow being abandoned holds the Cancel that fences its counter (Fence). */
    case WT_JOURNAL_ESCROW: return operation == WT_OP_SUBMIT || operation == WT_OP_CANCEL;
    case WT_JOURNAL_SUBMIT_READY: return operation == WT_OP_SUBMIT;
#else
    case WT_JOURNAL_ESCROW: case WT_JOURNAL_SUBMIT_READY: return operation == WT_OP_SUBMIT;
#endif
    case WT_JOURNAL_QUEUED: return operation == 0 || operation == WT_OP_CANCEL;
    /* A delivery finished at boot waits for its ACK until the next visit. */
    case WT_JOURNAL_APPLIED: return operation == 0 || operation == WT_OP_ACKNOWLEDGE;
    default: return 0;
    }
}

static int Commit(WtEngine *engine, WtJournalEntry *next)
{
    if (Refresh(engine) != 0) return -1;
    next->generation = engine->entry.generation + 1;
    memcpy(next->seed, engine->entry.seed, 32);
    if (WtJournal_Commit(&engine->journal, next) != 0) return Block(engine);
    engine->entry = *next;
    return 0;
}

static int Random(WtEngine *engine, unsigned char *bytes, size_t size)
{
    return engine->io.random(engine->io.arg, bytes, size) == 0 && !Zero(bytes, size) ? 0 : Block(engine);
}

#ifdef WT_ABANDON_UNSENT
/* LUNE-902: which escrows the DS Companion may abandon, proved by the save's
 * history rather than by the Pokémon it holds (wonder_trade/engine.h
 * WT_INVENTORY_HISTORY). Only a companion escrow qualifies, and its
 * removal must be one that moved the save counter far ahead, so it must be
 * one only the companion can write:
 * - its Submit's request ID is 4 random bytes, then 12 bytes of SHA-256 over
 *   them, the save, the counter, the save before the deposit and the offer
 *   (CompanionMark): an escrow without it (a game's, an older companion's) is
 *   never abandoned;
 * - its inventoryBefore is stored masked with SHA-256 of a label and the
 *   request ID (BeforeMask). The games' REMOVE (and an older companion's)
 *   finds the save matching neither recorded inventory and fails, changing
 *   nothing, unless the companion's own removal already landed; only this
 *   engine unmasks it, and it records the true hash once the removal is
 *   proven (SUBMIT_READY). The games accept any nonzero request ID and any
 *   nonzero inventory hash; nothing else is stored. */
static const char kCompanionDeposit[] = "DSWT LUNE-902 companion deposit v2";
static const char kBeforeMask[] = "DSWT LUNE-902 masked inventoryBefore";

static int CompanionMark(WtJournalHash hash, const WtJournalEntry *entry, const unsigned char before[32],
                         const unsigned char id[16], unsigned char mark[32])
{
    unsigned char bytes[sizeof(kCompanionDeposit) + 16 + 4 + 8 + 32 + 136], *at = bytes;
    uint64_t counter = entry->operationCounter;
    unsigned i;
    int result;
    memcpy(at, kCompanionDeposit, sizeof(kCompanionDeposit)); at += sizeof(kCompanionDeposit);
    memcpy(at, entry->saveID, 16); at += 16;
    memcpy(at, id, 4); at += 4;
    for (i = 0; i < 8; ++i) *at++ = (unsigned char)(counter >> (8 * i));
    memcpy(at, before, 32); at += 32;
    memcpy(at, entry->outgoing, 136);
    result = hash(bytes, sizeof(bytes), mark);
    Clear(bytes, sizeof(bytes));
    return result;
}

static int BeforeMask(WtJournalHash hash, const unsigned char id[16], unsigned char mask[32])
{
    unsigned char bytes[sizeof(kBeforeMask) + 16];
    memcpy(bytes, kBeforeMask, sizeof(kBeforeMask)); memcpy(bytes + sizeof(kBeforeMask), id, 16);
    return hash(bytes, sizeof(bytes), mask);
}

/* 1 when an ESCROW entry is the companion's, with `before` its true
 * inventoryBefore; 0 for any other escrow (`before` is then the stored one). */
static int CompanionEscrow(WtJournalHash hash, const WtJournalEntry *entry, unsigned char before[32])
{
    unsigned char mask[32], mark[32];
    unsigned i;
    int companion = 0;
    memcpy(before, entry->inventoryBefore, 32);
    if (entry->phase == WT_JOURNAL_ESCROW && entry->requestLength && entry->request[5] == WT_OP_SUBMIT
        && memcmp(entry->request + 8, entry->operation, 16) == 0 && BeforeMask(hash, entry->operation, mask) == 0) {
        for (i = 0; i < 32; ++i) mask[i] ^= entry->inventoryBefore[i];
        companion = CompanionMark(hash, entry, mask, entry->operation, mark) == 0
            && memcmp(mark, entry->operation + 4, 12) == 0;
        if (companion) memcpy(before, mask, 32);
    }
    Clear(mask, sizeof(mask)); Clear(mark, sizeof(mark));
    return companion;
}

int WtEngine_EscrowInventoryBefore(const WtJournalEntry *entry, WtJournalHash hash, unsigned char before[32])
{
    return entry && hash && before ? CompanionEscrow(hash, entry, before) : -1;
}
#endif

static void ClearRequest(WtJournalEntry *entry)
{
    Clear(entry->request, sizeof(entry->request)); entry->requestLength = 0;
}

static void Idle(WtJournalEntry *entry)
{
    Clear(entry->operation, offsetof(WtJournalEntry, seed) - offsetof(WtJournalEntry, operation));
    ClearRequest(entry); entry->phase = WT_JOURNAL_IDLE;
}

static int Encode(WtEngine *engine, WtWireRequest *request, WtJournalEntry *pending)
{
    size_t size = 0;
    int result;
    if (WtWire_Encode(request, engine->journal.io.hash, engine->frame, sizeof(engine->frame), &size) != 0) {
        Clear(request, sizeof(*request)); return Block(engine);
    }
    engine->frameSize = size;
    if (pending) {
        ClearRequest(pending); memcpy(pending->request, engine->frame, size);
        pending->requestLength = (uint16_t)size;
    }
    result = 0;
    Clear(request, sizeof(*request));
    return result;
}

static int RequestIdentity(WtEngine *engine, WtWireRequest *request, unsigned operation)
{
    memset(request, 0, sizeof(*request)); request->operation = (unsigned char)operation;
    memcpy(request->saveID, engine->entry.saveID, 16);
    memcpy(request->credential, engine->entry.credential, 32);
    return Random(engine, request->requestID, 16);
}

static int Inventory(WtEngine *engine, enum WtInventoryStep step, WtJournalEntry *entry, unsigned slot)
{
    if (engine->io.inventory(engine->io.arg, step, entry, slot) != 0) return Block(engine);
    return 0;
}

/* The offer recorded as a delivery of itself. */
static int Returning(WtEngine *engine, WtJournalEntry *next)
{
    next->deliveryKind = WT_DELIVERY_RETURN;
    memcpy(next->incoming, next->outgoing, sizeof(next->incoming));
    return engine->journal.io.hash(next->incoming, sizeof(next->incoming), next->incomingDigest) == 0 ? 0 : Block(engine);
}

/* Return a rejected or cancelled offer. While the save still matches the
 * escrow it goes back to its own slot. The journal first records it as a
 * return without a destination, so a restore cut short and then saved over by
 * another game is never put back twice: RECEIVE accepts that save as it is.
 * Once the game has saved since the escrow, the offer returns like a delivery
 * into the first free party or PC slot, recorded before the save is written.
 * 1 keeps it in the journal while storage is full. */
static int Restore(WtEngine *engine, WtJournalEntry *next)
{
    int inPlace = next->deliveryKind == WT_DELIVERY_RETURN && next->destination.kind == 0, prepared;
    if (next->deliveryKind != WT_DELIVERY_RETURN || inPlace) {
        prepared = engine->io.inventory(engine->io.arg, WT_INVENTORY_PREPARE_RESTORE, next, next->source.slot);
        if (prepared == 0) {
            if (!inPlace && (Returning(engine, next) != 0 || Commit(engine, next) != 0)) return -1;
            return Inventory(engine, WT_INVENTORY_RESTORE, next, next->source.slot);
        }
        if (prepared != 1) return Block(engine);
        if (!inPlace) {
            if (Returning(engine, next) != 0) return -1;
            prepared = engine->io.inventory(engine->io.arg, WT_INVENTORY_PREPARE_RECEIVE, next, 0);
            if (prepared == 1) return 1;
            if (prepared != 0) return Block(engine);
            if (Commit(engine, next) != 0) return -1;
        }
    }
    if (Inventory(engine, WT_INVENTORY_RECEIVE, next, next->destination.slot) != 0
        || Inventory(engine, WT_INVENTORY_VERIFY_RECEIVED, next, next->destination.slot) != 0) return -1;
    return 0;
}

static int Resume(WtEngine *engine)
{
    WtJournalEntry next = engine->entry;
    int result = -1;
    if (engine->blocked) goto done;
    if (next.phase == WT_JOURNAL_ESCROW) {
#ifdef WT_ABANDON_UNSENT
        /* The companion only. An escrow whose fence is recorded (Fence) is
         * being abandoned whatever the save holds now. Otherwise the save's
         * history decides (WT_INVENTORY_HISTORY): REMOVE runs only on the
         * escrow's own save, the offer still in the party as offered, or a
         * save that provably descends from the removal. The companion's own
         * escrow whose offer left the save through play, before any removal,
         * waits for WtEngine_Request to fence its counter and abandon it: its
         * Submit must never go out. Any other escrow then blocks, as does
         * anything unproved: nothing written, nothing sent. */
        if (next.request[5] == WT_OP_CANCEL) { result = WT_INVENTORY_KEPT; goto done; }
        {
            int companion = CompanionEscrow(engine->journal.io.hash, &next, next.inventoryBefore);
            int history = engine->io.inventory(engine->io.arg, WT_INVENTORY_HISTORY, &next, next.source.slot);
            if (companion && (history == WT_HISTORY_LEFT || history == WT_HISTORY_AFTER)) {
                result = WT_INVENTORY_KEPT; goto done;
            }
            if (history != WT_HISTORY_UNCHANGED && history != WT_HISTORY_REMOVED
                && !(history == WT_HISTORY_AFTER && !companion)) { Block(engine); goto done; }
        }
        if (Inventory(engine, WT_INVENTORY_REMOVE, &next, next.source.slot) != 0
            || Inventory(engine, WT_INVENTORY_VERIFY_REMOVED, &next, next.source.slot) != 0) goto done;
#else
        if (Inventory(engine, WT_INVENTORY_REMOVE, &next, next.source.slot) != 0
            || Inventory(engine, WT_INVENTORY_VERIFY_REMOVED, &next, next.source.slot) != 0) goto done;
#endif
        next.phase = WT_JOURNAL_SUBMIT_READY;
        if (Commit(engine, &next) != 0) goto done;
    }
    if (next.phase == WT_JOURNAL_RESTORING) {
        int restored = Restore(engine, &next);
        if (restored != 0) { result = restored; goto done; }
        Idle(&next);
        if (Commit(engine, &next) != 0) goto done;
    }
    if (next.phase == WT_JOURNAL_RECEIVED) {
        int prepared = engine->io.inventory(engine->io.arg, WT_INVENTORY_PREPARE_RECEIVE, &next, 0);
        if (prepared == 1) { result = 1; goto done; }
        if (prepared != 0) { Block(engine); goto done; }
        next.phase = WT_JOURNAL_APPLYING;
        if (Commit(engine, &next) != 0) goto done;
    }
    if (next.phase == WT_JOURNAL_APPLYING) {
        if (Inventory(engine, WT_INVENTORY_RECEIVE, &next, next.destination.slot) != 0
            || Inventory(engine, WT_INVENTORY_VERIFY_RECEIVED, &next, next.destination.slot) != 0) goto done;
        /* Applying needs no randomness, so boot can finish it; the ACK,
         * which needs a request ID, is prepared on the next visit. */
        next.phase = WT_JOURNAL_APPLIED;
        memcpy(next.lastReceipt, next.receipt, 16);
        if (Commit(engine, &next) != 0) goto done;
    }
    result = 0;
done:
    Clear(&next, sizeof(next));
    return result;
}

static int Acknowledge(WtEngine *engine)
{
    WtJournalEntry next = engine->entry;
    WtWireRequest request;
    int result = -1;
    if (RequestIdentity(engine, &request, WT_OP_ACKNOWLEDGE) != 0) goto done;
    memcpy(next.operation, request.requestID, 16); next.operationCounter = next.nextCounter;
    request.counter = next.operationCounter;
    memcpy(request.body.acknowledge.trade, next.trade, 16);
    memcpy(request.body.acknowledge.receipt, next.receipt, 16);
    memcpy(request.body.acknowledge.deliveredDigest, next.incomingDigest, 32);
    memcpy(request.body.acknowledge.appliedDigest, next.appliedDigest, 32);
    request.body.acknowledge.generation = engine->entry.generation + 1;
    if (Encode(engine, &request, &next) != 0 || Commit(engine, &next) != 0) goto done;
    Clear(engine->frame, sizeof(engine->frame)); engine->frameSize = 0;
    engine->lastResult = 0;
    result = 0;
done:
    Clear(&next, sizeof(next)); Clear(&request, sizeof(request));
    return result;
}

int WtEngine_Settle(WtEngine *engine)
{
    /* An interrupted deposit finishes its removal rather than cancel: its
     * Submit must stay unchanged until the server's outcome is proven. */
    return Refresh(engine) != 0 ? -1 : Resume(engine);
}

int WtEngine_Open(WtEngine *engine, WtJournalIO journal, WtEngineIO io)
{
    if (!engine) return -1;
    memset(engine, 0, sizeof(*engine)); engine->io = io;
    if (!io.random || !io.inventory || !io.canHold || WtJournal_Open(&engine->journal, journal) != 0
        || WtJournal_Read(&engine->journal, &engine->entry) != 0
        || engine->entry.generation == UINT64_MAX || !StateKnown(&engine->entry, journal.hash)
        || WtJournal_Repair(&engine->journal) != 0) return Block(engine);
    return 0;
}

void WtEngine_Close(WtEngine *engine)
{
    if (engine) Clear(engine, sizeof(*engine));
}

int WtEngine_Enroll(WtEngine *engine, uint32_t build)
{
    WtJournalEntry next;
    WtWireRequest request;
    int result = -1;
    if (Refresh(engine) != 0) return -1;
    if (engine->entry.phase == WT_JOURNAL_ENROLLING) return 0;
    if (engine->entry.phase != WT_JOURNAL_EMPTY) return -1;
    next = engine->entry;
    if (Random(engine, next.credential, 32) != 0 || Random(engine, next.enrollmentID, 16) != 0) goto done;
    memset(&request, 0, sizeof(request)); request.operation = WT_OP_ENROLL;
    memcpy(request.requestID, next.enrollmentID, 16); memcpy(request.credential, next.credential, 32);
    request.body.enroll.build = build;
    next.phase = WT_JOURNAL_ENROLLING; next.nextCounter = 1;
    if (Encode(engine, &request, &next) == 0) result = Commit(engine, &next);
    Clear(engine->frame, sizeof(engine->frame)); engine->frameSize = 0;
    engine->lastResult = 0;
done:
    Clear(&next, sizeof(next)); Clear(&request, sizeof(request));
    return result;
}

int WtEngine_Deposit(WtEngine *engine, unsigned slot)
{
    WtJournalEntry next;
    WtWireRequest request;
    int result = -1;
    if (Refresh(engine) != 0 || !engine->synchronized || engine->frameSize
        || engine->entry.phase != WT_JOURNAL_IDLE || engine->entry.nextCounter == UINT64_MAX) return -1;
    next = engine->entry;
    if (Inventory(engine, WT_INVENTORY_PREPARE_REMOVE, &next, slot) != 0
        || RequestIdentity(engine, &request, WT_OP_SUBMIT) != 0) goto done;
    next.phase = WT_JOURNAL_ESCROW;
    memcpy(next.operation, request.requestID, 16); next.operationCounter = next.nextCounter;
#ifdef WT_ABANDON_UNSENT
    {
        unsigned char mark[32], mask[32];
        unsigned i;
        int marked = CompanionMark(engine->journal.io.hash, &next, next.inventoryBefore, request.requestID, mark) == 0;
        if (marked) {
            memcpy(request.requestID + 4, mark, 12); memcpy(next.operation, request.requestID, 16);
            marked = BeforeMask(engine->journal.io.hash, next.operation, mask) == 0;
            for (i = 0; i < 32; ++i) next.inventoryBefore[i] ^= mask[i];
        }
        Clear(mark, sizeof(mark)); Clear(mask, sizeof(mask));
        if (!marked) { Block(engine); goto done; }
    }
#endif
    request.counter = next.operationCounter;
    request.body.submit.generation = next.generation + 1;
    memcpy(request.body.submit.pokemon, next.outgoing, 136);
    if (Encode(engine, &request, &next) != 0 || Commit(engine, &next) != 0) goto done;
    engine->frameSize = 0; Clear(engine->frame, sizeof(engine->frame));
    engine->lastResult = 0;
    result = Resume(engine);
done:
    Clear(&next, sizeof(next)); Clear(&request, sizeof(request));
    return result;
}

#ifdef WT_ABANDON_UNSENT
/* LUNE-902: an escrow REMOVE found kept is abandoned by fencing its counter.
 * An empty reconcile page is no proof: another copy of the save may have sent
 * this Submit, which the server checks against the counter, then validates
 * without holding it, then checks again before accepting it. So instead the
 * escrow sends a counted Cancel at the Submit's own counter, of a trade
 * nobody holds (a random ID). The server checks and records both under the
 * save's lock: if the Cancel is recorded first it is rejected NOT_FOUND and
 * uses the counter, and a Submit at that counter can never be accepted;
 * otherwise the Cancel finds the counter used and the escrow blocks.
 * The Cancel replaces the Submit in the journal before it is sent (journal.c
 * allows exactly that change), so a lost reply resends the same Cancel and a
 * cut never forgets it; from then on the escrow is being abandoned whatever
 * the save holds. The Submit was never sent from this journal: it leaves only
 * from SUBMIT_READY, committed once the removal is proven. */
static int Fence(WtEngine *engine)
{
    WtJournalEntry next = engine->entry;
    WtWireRequest request;
    int result = -1;
    if (next.phase != WT_JOURNAL_ESCROW || !next.requestLength || next.operationCounter != next.nextCounter
        || next.nextCounter == UINT64_MAX) { Block(engine); goto done; }
    if (next.request[5] == WT_OP_SUBMIT) {
        if (RequestIdentity(engine, &request, WT_OP_CANCEL) != 0 || Random(engine, request.body.trade, 16) != 0) goto done;
        request.counter = next.nextCounter;
        /* The journal keeps a Cancel's trade beside it. */
        memcpy(next.operation, request.requestID, 16); memcpy(next.trade, request.body.trade, 16);
        if (Encode(engine, &request, &next) != 0 || Commit(engine, &next) != 0) goto done;
    } else if (next.request[5] != WT_OP_CANCEL) { Block(engine); goto done; }
    /* Sent from the journal, as every counted request. */
    memcpy(engine->frame, engine->entry.request, engine->entry.requestLength);
    engine->frameSize = engine->entry.requestLength;
    engine->lastResult = 0;
    result = 0;
done:
    Clear(&next, sizeof(next)); Clear(&request, sizeof(request));
    return result;
}
#endif

int WtEngine_Request(WtEngine *engine, unsigned operation)
{
    WtJournalEntry next;
    WtWireRequest request;
    int result = -1, rejectionLookup;
    if (Refresh(engine) != 0 || engine->frameSize) return -1;
    result = Resume(engine);
#ifdef WT_ABANDON_UNSENT
    if (result == WT_INVENTORY_KEPT) return Fence(engine);
#endif
    if (result != 0) return result;
    if (engine->entry.phase == WT_JOURNAL_APPLIED && !engine->entry.requestLength && Acknowledge(engine) != 0) return -1;
    next = engine->entry;
    result = -1;
    rejectionLookup = operation == WT_OP_RECONCILE && next.requestLength
        && ((next.phase == WT_JOURNAL_SUBMIT_READY
                && (engine->lastResult == WT_RESULT_ILLEGAL_POKEMON || engine->lastResult == WT_RESULT_COUNTER_MISMATCH))
            || (next.phase == WT_JOURNAL_QUEUED && next.request[5] == WT_OP_CANCEL
                && (engine->lastResult == WT_RESULT_CONFLICT || engine->lastResult == WT_RESULT_NOT_FOUND)));
    if (next.requestLength && !rejectionLookup) {
        if ((next.phase == WT_JOURNAL_SUBMIT_READY
                && Inventory(engine, WT_INVENTORY_VERIFY_REMOVED, &next, next.source.slot) != 0)
            || (next.phase == WT_JOURNAL_APPLIED
                && Inventory(engine, WT_INVENTORY_VERIFY_RECEIVED, &next, next.destination.slot) != 0)) goto done;
        memcpy(engine->frame, next.request, next.requestLength); engine->frameSize = next.requestLength;
        result = 0; goto done;
    }
    if (next.phase != WT_JOURNAL_IDLE && next.phase != WT_JOURNAL_QUEUED
        && !rejectionLookup) goto done;
    if (!engine->synchronized) operation = WT_OP_RECONCILE;
    else if (operation == 0) operation = next.phase == WT_JOURNAL_IDLE ? WT_OP_RECONCILE : WT_OP_STATUS;
    if (operation != WT_OP_RECONCILE && operation != WT_OP_STATUS && operation != WT_OP_COLLECT && operation != WT_OP_CANCEL) goto done;
    if ((operation == WT_OP_CANCEL || operation == WT_OP_COLLECT) && next.phase != WT_JOURNAL_QUEUED) goto done;
    if (operation == WT_OP_CANCEL && (next.deliveryKind || !Zero(next.receipt, 16))) goto done;
    if (next.phase != WT_JOURNAL_IDLE && Inventory(engine, WT_INVENTORY_VERIFY_REMOVED, &next, next.source.slot) != 0) goto done;
    if (RequestIdentity(engine, &request, operation) != 0) goto done;
    if (operation == WT_OP_RECONCILE) { request.body.reconcile.after = next.eventCursor; request.body.reconcile.limit = 8; }
    else memcpy(request.body.trade, next.trade, 16);
    if (operation == WT_OP_CANCEL) {
        if (next.nextCounter == UINT64_MAX) goto done;
        request.counter = next.nextCounter; memcpy(next.operation, request.requestID, 16); next.operationCounter = next.nextCounter;
    }
    if (Encode(engine, &request, operation == WT_OP_CANCEL ? &next : NULL) != 0) goto done;
    result = operation == WT_OP_CANCEL ? Commit(engine, &next) : 0;
    if (operation == WT_OP_CANCEL) engine->lastResult = 0;
done:
    Clear(&next, sizeof(next)); Clear(&request, sizeof(request));
    return result;
}

static int ContextMatches(WtEngine *engine, const WtWireReply *reply, int finalized)
{
    uint64_t counter = engine->entry.nextCounter;
    if (finalized) {
        if (engine->entry.operationCounter != counter || counter == UINT64_MAX) return 0;
        ++counter;
    }
    return reply->nextCounter == counter && reply->highwater >= engine->entry.eventCursor;
}

/* Reconcile's answer when the page holds an event for a trade this journal
 * never made: only another copy of this save could have made it. */
#define WT_RECONCILE_FOREIGN (-2)

#ifdef WT_ABANDON_UNSENT
/* LUNE-902: the fence's answer. NOT_FOUND at the next counter proves the
 * server recorded this Cancel at the escrow's counter: the Submit can never be
 * accepted, and the escrow returns to IDLE with the Cancel as its last
 * operation, the save untouched. The cursor stays behind the Cancel's
 * REJECTED event, which the next Reconcile reads as this journal's own.
 * Anything else that is not worth retrying blocks: COUNTER_MISMATCH means
 * another copy used the counter, perhaps for this very Submit. */
static int Abandon(WtEngine *engine, const WtWireReply *reply, WtJournalEntry *next)
{
    if (reply->result != WT_RESULT_NOT_FOUND || next->request[5] != WT_OP_CANCEL
        || next->operationCounter != next->nextCounter || !ContextMatches(engine, reply, 1)) return -1;
    memcpy(next->lastOperation, next->operation, 16); next->lastOperationCounter = next->operationCounter;
    next->nextCounter = reply->nextCounter;
    Idle(next);
    if (Commit(engine, next) != 0) return -1;
    engine->synchronized = 0;
    return 0;
}

/* The fence's own REJECTED event, read by an IDLE journal after Abandon. */
static int OwnFence(const WtJournalEntry *entry, const WtWireEvent *event)
{
    return entry->phase == WT_JOURNAL_IDLE && event->kind == WT_EVENT_REJECTED && entry->lastOperationCounter
        && memcmp(event->operation, entry->lastOperation, 16) == 0 && Zero(event->trade, 16)
        && Zero(event->digest, 32) && event->state == WT_TRADE_NONE;
}
#endif

static int Reconcile(WtEngine *engine, const WtWireReply *reply, WtJournalEntry *next)
{
    unsigned i;
    enum { NO_REJECTION, REJECTED_SUBMIT, REJECTED_CANCEL } rejected = NO_REJECTION;
    unsigned char outgoingDigest[32];
    if (next->phase == WT_JOURNAL_SUBMIT_READY && reply->body.page.count
        && memcmp(reply->body.page.events[0].operation, next->operation, 16) != 0) {
        /* Counters only advance and every counted operation records one event
         * with its ID. The first event after this cursor is the operation that
         * used this Submit's counter; if it is another copy's, this request
         * can never be accepted and its escrow is safe to restore. */
        const WtWireEvent *event = &reply->body.page.events[0];
        if (event->sequence != next->eventCursor + 1 || Zero(event->operation, 16)
            || next->operationCounter != next->nextCounter || reply->nextCounter <= next->operationCounter) return -1;
        /* Finalize the unaccepted operation so its retained request may be dropped. */
        memcpy(next->lastOperation, next->operation, 16); next->lastOperationCounter = next->operationCounter;
        next->nextCounter = next->operationCounter + 1;
        next->phase = WT_JOURNAL_RESTORING; ClearRequest(next);
        if (Commit(engine, next) != 0) return -1;
        engine->synchronized = 0;  /* The cursor stays behind the other copy's history. */
        return 0;
    }
    if (engine->journal.io.hash(next->outgoing, 136, outgoingDigest) != 0) return -1;
    for (i = 0; i < reply->body.page.count; ++i) {
        const WtWireEvent *event = &reply->body.page.events[i];
        if (event->kind == WT_EVENT_ENROLLED && next->phase == WT_JOURNAL_IDLE && next->nextCounter == 1) continue;
#ifdef WT_ABANDON_UNSENT
        if (OwnFence(next, event)) continue;
#endif
        if (next->phase == WT_JOURNAL_SUBMIT_READY && event->kind == WT_EVENT_REJECTED
            && memcmp(event->operation, next->operation, 16) == 0
            && memcmp(event->digest, outgoingDigest, 32) == 0 && Zero(event->trade, 16)
            && event->state == WT_TRADE_NONE && !rejected) { rejected = REJECTED_SUBMIT; continue; }
        if (next->phase == WT_JOURNAL_QUEUED && next->requestLength && next->request[5] == WT_OP_CANCEL
            && event->kind == WT_EVENT_REJECTED && !rejected && Zero(event->digest, 32)
            && memcmp(event->operation, next->operation, 16) == 0 && memcmp(event->trade, next->trade, 16) == 0
            && (event->state == WT_TRADE_MATCHED || event->state == WT_TRADE_RETURN_READY)) { rejected = REJECTED_CANCEL; continue; }
        if (next->phase == WT_JOURNAL_IDLE
            || (next->phase == WT_JOURNAL_QUEUED && memcmp(event->trade, next->trade, 16) != 0)) return WT_RECONCILE_FOREIGN;
        if (next->phase != WT_JOURNAL_QUEUED) return -1;
        if (event->kind == WT_EVENT_SUBMITTED) {
            if (memcmp(event->operation, next->lastOperation, 16) != 0 || memcmp(event->digest, outgoingDigest, 32) != 0) return -1;
        } else if (event->kind == WT_EVENT_MATCHED || event->kind == WT_EVENT_CANCELLED) {
            unsigned kind = event->kind == WT_EVENT_MATCHED ? WT_DELIVERY_MATCH : WT_DELIVERY_RETURN;
            if (next->deliveryKind && next->deliveryKind != kind) return -1;
            if (!Zero(next->receipt, 16) && memcmp(next->receipt, event->receipt, 16) != 0) return -1;
            /* The owner's own Cancel, or the server handing back an offer it refused
             * (no operation: it used none of this save's counters). Either returns this offer. */
            if (event->kind == WT_EVENT_CANCELLED && ((!Zero(event->operation, 16)
                && memcmp(event->operation, next->lastOperation, 16) != 0)
                || memcmp(event->digest, outgoingDigest, 32) != 0)) return -1;
            if (!Zero(next->incomingDigest, 32) && memcmp(next->incomingDigest, event->digest, 32) != 0) return -1;
            memcpy(next->receipt, event->receipt, 16);
            memcpy(next->incomingDigest, event->digest, 32);
            next->deliveryKind = (unsigned char)kind;
        } else return -1;
    }
    if (!ContextMatches(engine, reply, rejected != 0)
        && !(reply->body.page.hasMore && next->requestLength && ContextMatches(engine, reply, 1))) return -1;
    if (rejected) {
        memcpy(next->lastOperation, next->operation, 16); next->lastOperationCounter = next->operationCounter;
        next->nextCounter = reply->nextCounter;
        if (rejected == REJECTED_SUBMIT) next->phase = WT_JOURNAL_RESTORING;
        ClearRequest(next);
    }
    next->eventCursor = reply->body.page.end;
    if (Commit(engine, next) != 0) return -1;
    engine->synchronized = !reply->body.page.hasMore;
    return 0;
}

/* The server counted an operation this journal never sent: a newer copy of
 * this save has traded since this one was saved. */
static int Ahead(const WtEngine *engine, const WtWireReply *reply)
{
    const WtJournalEntry *entry = &engine->entry;
    uint64_t own = entry->nextCounter;
    if (entry->requestLength && entry->operationCounter == own && own != UINT64_MAX) ++own;
    return reply->nextCounter > own;
}

static int DeliveryMatches(const WtJournalEntry *entry, const WtWireSnapshot *snapshot)
{
    return (Zero(entry->receipt, 16) || memcmp(entry->receipt, snapshot->receipt, 16) == 0)
        && (Zero(entry->incomingDigest, 32) || memcmp(entry->incomingDigest, snapshot->receivedDigest, 32) == 0)
        && (!entry->deliveryKind || entry->deliveryKind == snapshot->deliveryKind);
}

int WtEngine_Reply(WtEngine *engine, const unsigned char *packet, size_t size)
{
    WtWireReply reply;
    WtJournalEntry next;
    unsigned char outgoingDigest[32];
    unsigned operation;
    int result = -1, foreign = 0;
    if (Refresh(engine) != 0 || !engine->frameSize) return -1;
    operation = engine->frame[5];
    if (WtWire_Decode(engine->frame, engine->frameSize, packet, size, engine->journal.io.hash, &reply) != 0) return Block(engine);
    Clear(engine->frame, sizeof(engine->frame)); engine->frameSize = 0;
    engine->lastResult = reply.result; engine->refused = 0;
    next = engine->entry;
#ifdef WT_ABANDON_UNSENT
    /* Only Fence sends a Cancel while the journal is in ESCROW. */
    if (operation == WT_OP_CANCEL && next.phase == WT_JOURNAL_ESCROW && reply.result != WT_RESULT_PROCESSING
        && reply.result != WT_RESULT_UNAVAILABLE) {
        engine->synchronized = 0;
        if (Abandon(engine, &reply, &next) != 0) goto blocked;
        result = 0; goto done;
    }
#endif
    if (reply.result != WT_RESULT_OK) {
        /* An error, including a validator verdict, is not restoration proof.
         * Reconcile the retained Submit's exact REJECTED event instead. */
        int definitiveRejection = next.requestLength && next.operationCounter == next.nextCounter
            && ((operation == WT_OP_SUBMIT && reply.result == WT_RESULT_ILLEGAL_POKEMON)
                || (operation == WT_OP_CANCEL && (reply.result == WT_RESULT_CONFLICT || reply.result == WT_RESULT_NOT_FOUND)));
        /* A mismatch is not restoration proof either; reconcile who used the counter. */
        int fenced = operation == WT_OP_SUBMIT && reply.result == WT_RESULT_COUNTER_MISMATCH
            && next.phase == WT_JOURNAL_SUBMIT_READY && next.requestLength && next.operationCounter == next.nextCounter
            && reply.nextCounter > next.operationCounter && reply.highwater > next.eventCursor;
        engine->synchronized = 0;
        if ((reply.result == WT_RESULT_COUNTER_MISMATCH && !fenced) || reply.result == WT_RESULT_CURSOR_AHEAD
            || reply.result == WT_RESULT_UNAUTHENTICATED || reply.result == WT_RESULT_INVALID_REQUEST
            || (reply.nextCounter && !fenced && ((!ContextMatches(engine, &reply, 0)
                    && !(definitiveRejection && ContextMatches(engine, &reply, 1)))))) goto blocked;
        result = 1;
        goto done;
    }
    if (operation == WT_OP_ENROLL) {
        if (next.phase != WT_JOURNAL_ENROLLING || reply.nextCounter != 1 || reply.highwater != 1) goto blocked;
        memcpy(next.saveID, reply.body.saveID, 16); next.phase = WT_JOURNAL_IDLE; ClearRequest(&next);
        result = Commit(engine, &next); goto done;
    }
    if (operation == WT_OP_RECONCILE) {
        result = Reconcile(engine, &reply, &next);
        foreign = result == WT_RECONCILE_FOREIGN;
        if (result != 0) goto blocked;
        result = Resume(engine); goto done;
    }
    if (operation == WT_OP_SUBMIT || operation == WT_OP_CANCEL || operation == WT_OP_ACKNOWLEDGE) {
        const WtWireSnapshot *snapshot = &reply.body.mutation.snapshot;
        if (!ContextMatches(engine, &reply, 1) || reply.body.mutation.event <= next.eventCursor) goto blocked;
        if (engine->journal.io.hash(next.outgoing, 136, outgoingDigest) != 0
            || memcmp(snapshot->offeredDigest, outgoingDigest, 32) != 0) goto blocked;
        if ((operation == WT_OP_SUBMIT && next.phase != WT_JOURNAL_SUBMIT_READY)
            || (operation == WT_OP_CANCEL && next.phase != WT_JOURNAL_QUEUED)
            || (operation == WT_OP_ACKNOWLEDGE && next.phase != WT_JOURNAL_APPLIED)) goto blocked;
        if ((operation == WT_OP_ACKNOWLEDGE || operation == WT_OP_CANCEL) && !DeliveryMatches(&next, snapshot)) goto blocked;
        memcpy(next.lastOperation, next.operation, 16); next.lastOperationCounter = next.operationCounter;
        next.nextCounter = reply.nextCounter; next.eventCursor = reply.body.mutation.event;
        ClearRequest(&next);
        if (operation == WT_OP_ACKNOWLEDGE) Idle(&next);
        else {
            next.phase = WT_JOURNAL_QUEUED; memcpy(next.trade, snapshot->trade, 16);
            if (operation == WT_OP_CANCEL) {
                memcpy(next.receipt, snapshot->receipt, 16); memcpy(next.incomingDigest, snapshot->receivedDigest, 32);
                next.deliveryKind = snapshot->deliveryKind;
            }
        }
        result = Commit(engine, &next); goto done;
    }
    if (!ContextMatches(engine, &reply, 0) || next.phase != WT_JOURNAL_QUEUED) goto blocked;
    {
        const WtWireSnapshot *snapshot = operation == WT_OP_STATUS ? &reply.body.status : &reply.body.collection.snapshot;
        if (memcmp(snapshot->trade, next.trade, 16) != 0
            || engine->journal.io.hash(next.outgoing, 136, outgoingDigest) != 0
            || memcmp(snapshot->offeredDigest, outgoingDigest, 32) != 0
            || snapshot->state == WT_TRADE_NONE || snapshot->state == WT_TRADE_ACKNOWLEDGED) goto blocked;
        if (!DeliveryMatches(&next, snapshot)) goto blocked;
        if (operation == WT_OP_STATUS) { result = 0; goto done; }
        if (operation != WT_OP_COLLECT || (snapshot->state != WT_TRADE_MATCHED && snapshot->state != WT_TRADE_RETURN_READY)) goto blocked;
        /* The digest and receipt are verified; refuse what this game cannot hold
         * before recording it. Collect only replays, so the journal stays QUEUED
         * and nothing is acknowledged. A return is this save's own offer. */
        if (snapshot->deliveryKind == WT_DELIVERY_MATCH
            && !engine->io.canHold(engine->io.arg, reply.body.collection.pokemon)) {
            engine->refused = 1; result = 1; goto done;
        }
        memcpy(next.receipt, snapshot->receipt, 16); next.deliveryKind = snapshot->deliveryKind;
        memcpy(next.incoming, reply.body.collection.pokemon, 136); memcpy(next.incomingDigest, snapshot->receivedDigest, 32);
        next.phase = WT_JOURNAL_RECEIVED;
        if (Commit(engine, &next) != 0) goto done;
        result = Resume(engine); goto done;
    }
blocked:
    /* Only an answer this journal cannot follow, with nothing failed or written
     * here, proves an older copy; a local failure stays an ordinary block. */
    if (!engine->blocked && (foreign || Ahead(engine, &reply))) engine->rolledBack = 1;
    result = Block(engine);
done:
    Clear(&reply, sizeof(reply)); Clear(&next, sizeof(next)); Clear(outgoingDigest, sizeof(outgoingDigest));
    return result;
}
