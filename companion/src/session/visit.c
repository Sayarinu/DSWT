#include "session/visit.h"

#include <string.h>

enum { MAX_EXCHANGES = 24 };

static int Cancelled(const CpTransport *transport)
{
    return transport->cancel && *transport->cancel;
}

int CpVisit_Run(WtEngine *engine, WtGame *game, uint32_t build, int slot, unsigned action,
    const CpTransport *transport, CpVisit *visit)
{
    unsigned char request[WT_WIRE_REQUEST_MAX], reply[WT_WIRE_REPLY_MAX];
    WtWireReply decoded;
    size_t requestSize, replySize;
    unsigned count, processing = 0, wireResult;
    int result = CP_VISIT_ERROR_JOURNAL, operation;
    if (!visit) return CP_VISIT_ERROR_JOURNAL;
    memset(visit, 0, sizeof(*visit));
    if (!engine || !game || !transport || !transport->post) return visit->result = CP_VISIT_ERROR_JOURNAL;
    for (count = 0; count < MAX_EXCHANGES && !Cancelled(transport); ++count) {
        if (WtJournal_Read(&engine->journal, &engine->entry) != 0) { result = CP_VISIT_ERROR_JOURNAL; break; }
        if (engine->entry.phase == WT_JOURNAL_EMPTY && WtEngine_Enroll(engine, build) != 0) {
            result = CP_VISIT_ERROR_JOURNAL; break;
        }
        /* A restored superseded copy stays fenced behind the other copy's history. */
        if (visit->superseded && engine->entry.phase == WT_JOURNAL_IDLE) { result = 0; break; }
        if (engine->entry.phase == WT_JOURNAL_IDLE && engine->synchronized) {
            if (slot < 0) { result = 0; break; }
            result = WtGame_Checkpoint(game);
            if (result != 0) { result = CP_VISIT_ERROR_CHECKPOINT + (result < -99 ? -99 : result); break; }
            if (WtEngine_Deposit(engine, (unsigned)slot) != 0) { result = CP_VISIT_ERROR_DEPOSIT; break; }
            slot = -1;
        }
        operation = WtSession_Next(engine, action);
        if (operation == WT_SESSION_BLOCKED) { result = CP_VISIT_ERROR_JOURNAL; break; }
        if (operation == WT_SESSION_DONE) { result = 0; break; }
        if (operation == WT_OP_ACKNOWLEDGE) visit->returned = engine->entry.deliveryKind == WT_DELIVERY_RETURN;
        requestSize = engine->frameSize;
        memcpy(request, engine->frame, requestSize);
        replySize = 0;
        if (transport->post(transport->arg, request, requestSize, reply, &replySize) != 0
            || replySize > sizeof(reply)) {
            result = CP_VISIT_ERROR_TRANSPORT; break;
        }
        /* The engine binds and checks the reply; the visit reads only its result. */
        if (WtWire_Decode(request, requestSize, reply, replySize, engine->journal.io.hash, &decoded) != 0) {
            (void)WtEngine_Reply(engine, reply, replySize);
            result = CP_VISIT_ERROR_JOURNAL; break;
        }
        if (WtEngine_Reply(engine, reply, replySize) < 0) { result = CP_VISIT_ERROR_JOURNAL; break; }
        /* Nothing was recorded; the trade stays QUEUED for a later Collect. */
        if (engine->refused) { visit->refused = 1; result = 0; break; }
        wireResult = decoded.result;
        if (wireResult == WT_RESULT_PROCESSING && processing < WT_ENGINE_PROCESSING_RETRIES) {
            /* Resending the saved request cannot add an operation. */
            ++processing; --count;
            if (transport->wait) transport->wait(transport->arg, WT_ENGINE_PROCESSING_WAIT_MS);
            continue;
        }
        if (wireResult == WT_RESULT_ILLEGAL_POKEMON) visit->rejected = 1;
        if (decoded.refusal) visit->refusal = decoded.refusal;
        if (operation == WT_OP_SUBMIT && wireResult == WT_RESULT_COUNTER_MISMATCH && !engine->blocked) {
            visit->superseded = 1; continue;
        }
        if (operation == WT_OP_ACKNOWLEDGE && wireResult == WT_RESULT_OK) {
            visit->completed = 1;
            if (slot < 0 && engine->entry.phase == WT_JOURNAL_IDLE) { result = 0; break; }
        }
        /* Collect reports CONFLICT only while the offer is still queued. */
        if (operation == WT_OP_COLLECT && wireResult == WT_RESULT_CONFLICT) { visit->waiting = 1; result = 0; break; }
        if (wireResult != WT_RESULT_OK && wireResult != WT_RESULT_ILLEGAL_POKEMON
            && !(operation == WT_OP_CANCEL && (wireResult == WT_RESULT_CONFLICT || wireResult == WT_RESULT_NOT_FOUND))) {
            result = -100 - (int)wireResult; break;
        }
        if (operation == WT_OP_STATUS) { result = 0; break; }
    }
    if (count == MAX_EXCHANGES) result = CP_VISIT_ERROR_LOOP;
    if (Cancelled(transport)) result = CP_VISIT_ERROR_CANCELLED;
    if (engine->rolledBack) { visit->rolledBack = 1; result = CP_VISIT_ERROR_ROLLED_BACK; }
    visit->phase = engine->entry.phase;
    visit->canLeave = (!engine->blocked || engine->rolledBack) && WtGame_CanLeave(game, &engine->entry);
    memset(request, 0, sizeof(request));
    memset(&decoded, 0, sizeof(decoded));
    return visit->result = result;
}
