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
    case WT_JOURNAL_ESCROW: case WT_JOURNAL_SUBMIT_READY: return operation == WT_OP_SUBMIT;
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

/* Return a rejected or cancelled offer. While the save still matches the
 * escrow it goes back to its own slot; once the game has saved since, it
 * returns like a delivery into the first free party or PC slot, recorded
 * before the save is written. 1 keeps it in the journal while storage is full. */
static int Restore(WtEngine *engine, WtJournalEntry *next)
{
    int prepared;
    if (next->deliveryKind != WT_DELIVERY_RETURN) {
        prepared = engine->io.inventory(engine->io.arg, WT_INVENTORY_PREPARE_RESTORE, next, next->source.slot);
        if (prepared == 0) return Inventory(engine, WT_INVENTORY_RESTORE, next, next->source.slot);
        if (prepared != 1) return Block(engine);
        next->deliveryKind = WT_DELIVERY_RETURN;
        memcpy(next->incoming, next->outgoing, sizeof(next->incoming));
        if (engine->journal.io.hash(next->incoming, sizeof(next->incoming), next->incomingDigest) != 0) return Block(engine);
        prepared = engine->io.inventory(engine->io.arg, WT_INVENTORY_PREPARE_RECEIVE, next, 0);
        if (prepared == 1) return 1;
        if (prepared != 0) return Block(engine);
        if (Commit(engine, next) != 0) return -1;
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
        if (Inventory(engine, WT_INVENTORY_REMOVE, &next, next.source.slot) != 0
            || Inventory(engine, WT_INVENTORY_VERIFY_REMOVED, &next, next.source.slot) != 0) goto done;
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

int WtEngine_Request(WtEngine *engine, unsigned operation)
{
    WtJournalEntry next;
    WtWireRequest request;
    int result = -1, rejectionLookup;
    if (Refresh(engine) != 0 || engine->frameSize) return -1;
    result = Resume(engine);
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
        if (next->phase == WT_JOURNAL_SUBMIT_READY && event->kind == WT_EVENT_REJECTED
            && memcmp(event->operation, next->operation, 16) == 0
            && memcmp(event->digest, outgoingDigest, 32) == 0 && Zero(event->trade, 16)
            && event->state == WT_TRADE_NONE && !rejected) { rejected = REJECTED_SUBMIT; continue; }
        if (next->phase == WT_JOURNAL_QUEUED && next->requestLength && next->request[5] == WT_OP_CANCEL
            && event->kind == WT_EVENT_REJECTED && !rejected && Zero(event->digest, 32)
            && memcmp(event->operation, next->operation, 16) == 0 && memcmp(event->trade, next->trade, 16) == 0
            && (event->state == WT_TRADE_MATCHED || event->state == WT_TRADE_RETURN_READY)) { rejected = REJECTED_CANCEL; continue; }
        if (next->phase != WT_JOURNAL_QUEUED || memcmp(event->trade, next->trade, 16) != 0) return -1;
        if (event->kind == WT_EVENT_SUBMITTED) {
            if (memcmp(event->operation, next->lastOperation, 16) != 0 || memcmp(event->digest, outgoingDigest, 32) != 0) return -1;
        } else if (event->kind == WT_EVENT_MATCHED || event->kind == WT_EVENT_CANCELLED) {
            unsigned kind = event->kind == WT_EVENT_MATCHED ? WT_DELIVERY_MATCH : WT_DELIVERY_RETURN;
            if (next->deliveryKind && next->deliveryKind != kind) return -1;
            if (!Zero(next->receipt, 16) && memcmp(next->receipt, event->receipt, 16) != 0) return -1;
            if (event->kind == WT_EVENT_CANCELLED && (memcmp(event->operation, next->lastOperation, 16) != 0
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
    int result = -1;
    if (Refresh(engine) != 0 || !engine->frameSize) return -1;
    operation = engine->frame[5];
    if (WtWire_Decode(engine->frame, engine->frameSize, packet, size, engine->journal.io.hash, &reply) != 0) return Block(engine);
    Clear(engine->frame, sizeof(engine->frame)); engine->frameSize = 0;
    engine->lastResult = reply.result; engine->refused = 0;
    next = engine->entry;
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
        if (Reconcile(engine, &reply, &next) != 0) goto blocked;
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
    result = Block(engine);
done:
    Clear(&reply, sizeof(reply)); Clear(&next, sizeof(next)); Clear(outgoingDigest, sizeof(outgoingDigest));
    return result;
}
