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

static int Resume(WtEngine *engine)
{
    WtJournalEntry next = engine->entry;
    WtWireRequest request;
    int result = -1;
    if (engine->blocked) goto done;
    if (next.phase == WT_JOURNAL_ESCROW) {
        if (Inventory(engine, WT_INVENTORY_REMOVE, &next, next.source.slot) != 0
            || Inventory(engine, WT_INVENTORY_VERIFY_REMOVED, &next, next.source.slot) != 0) goto done;
        next.phase = WT_JOURNAL_SUBMIT_READY;
        if (Commit(engine, &next) != 0) goto done;
    }
    if (next.phase == WT_JOURNAL_RESTORING) {
        if (Inventory(engine, WT_INVENTORY_RESTORE, &next, next.source.slot) != 0) goto done;
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
        next.phase = WT_JOURNAL_APPLIED;
        memcpy(next.lastReceipt, next.receipt, 16);
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
    }
    result = 0;
done:
    Clear(&next, sizeof(next)); Clear(&request, sizeof(request));
    return result;
}

int WtEngine_Open(WtEngine *engine, WtJournalIO journal, WtEngineIO io)
{
    if (!engine) return -1;
    memset(engine, 0, sizeof(*engine)); engine->io = io;
    if (!io.random || !io.inventory || WtJournal_Open(&engine->journal, journal) != 0
        || WtJournal_Read(&engine->journal, &engine->entry) != 0
        || engine->entry.generation == UINT64_MAX || engine->entry.phase == WT_JOURNAL_RECOVERY_REQUIRED
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
    result = Resume(engine);
done:
    Clear(&next, sizeof(next)); Clear(&request, sizeof(request));
    return result;
}

int WtEngine_Request(WtEngine *engine, unsigned operation)
{
    WtJournalEntry next;
    WtWireRequest request;
    int result = -1;
    if (Refresh(engine) != 0 || engine->frameSize) return -1;
    result = Resume(engine);
    if (result != 0) return result;
    next = engine->entry;
    result = -1;
    if (next.requestLength && !(operation == WT_OP_RECONCILE
            && next.phase == WT_JOURNAL_SUBMIT_READY && engine->lastResult != 0)) {
        if ((next.phase == WT_JOURNAL_SUBMIT_READY
                && Inventory(engine, WT_INVENTORY_VERIFY_REMOVED, &next, next.source.slot) != 0)
            || (next.phase == WT_JOURNAL_APPLIED
                && Inventory(engine, WT_INVENTORY_VERIFY_RECEIVED, &next, next.destination.slot) != 0)) goto done;
        memcpy(engine->frame, next.request, next.requestLength); engine->frameSize = next.requestLength;
        result = 0; goto done;
    }
    if (next.phase != WT_JOURNAL_IDLE && next.phase != WT_JOURNAL_QUEUED
        && !(next.phase == WT_JOURNAL_SUBMIT_READY && operation == WT_OP_RECONCILE && engine->lastResult)) goto done;
    if (!engine->synchronized) operation = WT_OP_RECONCILE;
    else if (operation == 0) operation = next.phase == WT_JOURNAL_IDLE ? WT_OP_RECONCILE : WT_OP_STATUS;
    if (operation != WT_OP_RECONCILE && operation != WT_OP_STATUS && operation != WT_OP_COLLECT && operation != WT_OP_CANCEL) goto done;
    if ((operation == WT_OP_CANCEL || operation == WT_OP_COLLECT) && next.phase != WT_JOURNAL_QUEUED) goto done;
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
    int rejected = 0;
    unsigned char outgoingDigest[32];
    if (engine->journal.io.hash(next->outgoing, 136, outgoingDigest) != 0) return -1;
    for (i = 0; i < reply->body.page.count; ++i) {
        const WtWireEvent *event = &reply->body.page.events[i];
        if (event->kind == WT_EVENT_ENROLLED && next->phase == WT_JOURNAL_IDLE && next->nextCounter == 1) continue;
        if (next->phase == WT_JOURNAL_SUBMIT_READY && event->kind == WT_EVENT_REJECTED
            && memcmp(event->operation, next->operation, 16) == 0
            && memcmp(event->digest, outgoingDigest, 32) == 0 && Zero(event->trade, 16)
            && event->state == WT_TRADE_NONE && !rejected) { rejected = 1; continue; }
        if (next->phase != WT_JOURNAL_QUEUED || memcmp(event->trade, next->trade, 16) != 0) return -1;
        if (event->kind == WT_EVENT_SUBMITTED) {
            if (memcmp(event->operation, next->lastOperation, 16) != 0 || memcmp(event->digest, outgoingDigest, 32) != 0) return -1;
        } else if (event->kind == WT_EVENT_MATCHED || event->kind == WT_EVENT_CANCELLED) {
            if (!Zero(next->receipt, 16) && memcmp(next->receipt, event->receipt, 16) != 0) return -1;
            memcpy(next->receipt, event->receipt, 16);
        } else return -1;
    }
    if (!ContextMatches(engine, reply, rejected)) return -1;
    if (rejected) {
        memcpy(next->lastOperation, next->operation, 16); next->lastOperationCounter = next->operationCounter;
        next->nextCounter = reply->nextCounter; next->phase = WT_JOURNAL_RESTORING; ClearRequest(next);
    }
    next->eventCursor = reply->body.page.end;
    if (Commit(engine, next) != 0) return -1;
    engine->synchronized = !reply->body.page.hasMore;
    return 0;
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
    engine->lastResult = reply.result;
    next = engine->entry;
    if (reply.result != WT_RESULT_OK) {
        /* An error, including a validator verdict, is not restoration proof.
         * Reconcile the retained Submit's exact REJECTED event instead. */
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
        memcpy(next.lastOperation, next.operation, 16); next.lastOperationCounter = next.operationCounter;
        next.nextCounter = reply.nextCounter; next.eventCursor = reply.body.mutation.event;
        ClearRequest(&next);
        if (operation == WT_OP_ACKNOWLEDGE) Idle(&next);
        else { next.phase = WT_JOURNAL_QUEUED; memcpy(next.trade, snapshot->trade, 16); }
        result = Commit(engine, &next); goto done;
    }
    if (!ContextMatches(engine, &reply, 0) || next.phase != WT_JOURNAL_QUEUED) goto blocked;
    {
        const WtWireSnapshot *snapshot = operation == WT_OP_STATUS ? &reply.body.status : &reply.body.collection.snapshot;
        if (memcmp(snapshot->trade, next.trade, 16) != 0
            || engine->journal.io.hash(next.outgoing, 136, outgoingDigest) != 0
            || memcmp(snapshot->offeredDigest, outgoingDigest, 32) != 0
            || snapshot->state == WT_TRADE_NONE || snapshot->state == WT_TRADE_ACKNOWLEDGED) goto blocked;
        if (operation == WT_OP_STATUS) { result = 0; goto done; }
        if (operation != WT_OP_COLLECT || (snapshot->state != WT_TRADE_MATCHED && snapshot->state != WT_TRADE_RETURN_READY)) goto blocked;
        if (!Zero(next.receipt, 16) && memcmp(next.receipt, snapshot->receipt, 16) != 0) goto blocked;
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
