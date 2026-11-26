#include "wonder_trade/session.h"

int WtSession_Next(WtEngine *engine, unsigned requested)
{
    unsigned operation = requested;
    int result;
    if (!engine || engine->blocked) return WT_SESSION_BLOCKED;
    if (engine->entry.phase == WT_JOURNAL_IDLE && engine->synchronized) return WT_SESSION_DONE;
    if (engine->entry.deliveryKind == WT_DELIVERY_RETURN) operation = WT_OP_COLLECT;
    if (engine->lastResult == WT_RESULT_ILLEGAL_POKEMON
        || (engine->entry.phase == WT_JOURNAL_SUBMIT_READY && engine->lastResult == WT_RESULT_COUNTER_MISMATCH)
        || (engine->entry.requestLength && engine->entry.request[5] == WT_OP_CANCEL
            && (engine->lastResult == WT_RESULT_CONFLICT || engine->lastResult == WT_RESULT_NOT_FOUND))) operation = WT_OP_RECONCILE;
    if (operation == WT_OP_CANCEL && engine->entry.deliveryKind == WT_DELIVERY_MATCH) operation = WT_OP_COLLECT;
    result = WtEngine_Request(engine, operation);
    if (result != 0) return result < 0 ? WT_SESSION_BLOCKED : WT_SESSION_DONE;
    return engine->frame[5];
}
