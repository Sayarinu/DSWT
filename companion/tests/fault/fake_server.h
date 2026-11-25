#ifndef COMPANION_FAULT_FAKE_SERVER_H
#define COMPANION_FAULT_FAKE_SERVER_H

/* The single-save wire fake from tests/session/visit_test.c, with durable
 * request-ID replay for restarts after a counted operation's reply. */
#include "wonder_trade/wire_layout.h"

/* --- The fake server: one save, one trade, the wire contract's replies. --- */
typedef struct Event {
    unsigned char bytes[WT_WIRE_EVENT_SIZE];
} Event;

typedef struct Server {
    unsigned char saveID[16], trade[16], asset[16], receipt[16], digest[32], pokemon[136];
    uint64_t nextCounter;
    Event events[16];
    unsigned eventCount, state, rejectSubmit, requests[8];
} Server;

static Server server;

static uint64_t Get(const unsigned char *p, unsigned size)
{
    uint64_t value = 0;
    while (size--) value = (value << 8) | p[size];
    return value;
}

static void Put(unsigned char *p, uint64_t value, unsigned size)
{
    while (size--) { *p++ = (unsigned char)value; value >>= 8; }
}

static void AddEvent(unsigned kind, const unsigned char *trade, const unsigned char *operation,
    const unsigned char *asset, const unsigned char *receipt, unsigned state)
{
    unsigned char *e = server.events[server.eventCount].bytes;
    memset(e, 0, WT_WIRE_EVENT_SIZE);
    Put(e + WT_WIRE_EVENT_SEQUENCE, ++server.eventCount, 8); e[WT_WIRE_EVENT_KIND] = (unsigned char)kind;
    if (trade) memcpy(e + WT_WIRE_EVENT_TRADE, trade, 16);
    if (operation) memcpy(e + WT_WIRE_EVENT_OPERATION, operation, 16);
    if (asset) memcpy(e + WT_WIRE_EVENT_ASSET, asset, 16);
    if (kind != WT_EVENT_ENROLLED) memcpy(e + WT_WIRE_EVENT_DIGEST, server.digest, 32);
    if (receipt) memcpy(e + WT_WIRE_EVENT_RECEIPT, receipt, 16);
    e[WT_WIRE_EVENT_STATE] = (unsigned char)state;
}

static void Snapshot(unsigned char *s)
{
    memset(s, 0, WT_WIRE_SNAPSHOT_SIZE);
    memcpy(s + WT_WIRE_SNAPSHOT_TRADE, server.trade, 16); s[WT_WIRE_SNAPSHOT_STATE] = (unsigned char)server.state;
    memcpy(s + WT_WIRE_SNAPSHOT_OFFERED_ASSET, server.asset, 16);
    memcpy(s + WT_WIRE_SNAPSHOT_OFFERED_DIGEST, server.digest, 32);
    if (server.state >= WT_TRADE_RETURN_READY) {
        s[WT_WIRE_SNAPSHOT_DELIVERY_KIND] = WT_DELIVERY_RETURN;
        memcpy(s + WT_WIRE_SNAPSHOT_RECEIVED_ASSET, server.asset, 16);
        memcpy(s + WT_WIRE_SNAPSHOT_RECEIVED_DIGEST, server.digest, 32);
        memcpy(s + WT_WIRE_SNAPSHOT_RECEIPT, server.receipt, 16);
    }
    Put(s + WT_WIRE_SNAPSHOT_LAST_EVENT, server.eventCount, 8);
}

static int PostOnce(void *arg, const unsigned char *request, size_t size,
    unsigned char reply[WT_WIRE_REPLY_MAX], size_t *replySize)
{
    const unsigned char *body = request + WT_WIRE_REQUEST_HEADER_SIZE;
    unsigned operation = request[WT_WIRE_REQUEST_OPERATION], result = WT_RESULT_OK;
    size_t length = 0;
    uint64_t counter = Get(request + WT_WIRE_REQUEST_COUNTER, 8);
    unsigned char *out = reply + WT_WIRE_REPLY_HEADER_SIZE;
    (void)arg;
    assert(WtWire_CheckRequest(request, size, Hash) == 0);
    ++server.requests[operation];
    switch (operation) {
    case WT_OP_ENROLL:
        memset(server.saveID, 0x51, 16); server.nextCounter = 1;
        if (!server.eventCount) AddEvent(WT_EVENT_ENROLLED, NULL, NULL, NULL, NULL, WT_TRADE_NONE);
        memcpy(out, server.saveID, 16); length = 16;
        break;
    case WT_OP_RECONCILE: {
        uint64_t after = Get(body + WT_WIRE_RECONCILE_AFTER, 8), i;
        unsigned limit = body[WT_WIRE_RECONCILE_LIMIT], count = 0;
        for (i = after; i < server.eventCount && count < limit; ++i, ++count)
            memcpy(out + WT_WIRE_PAGE_EVENTS + count * WT_WIRE_EVENT_SIZE, server.events[i].bytes, WT_WIRE_EVENT_SIZE);
        out[WT_WIRE_PAGE_COUNT] = (unsigned char)count; out[WT_WIRE_PAGE_HAS_MORE] = after + count < server.eventCount;
        Put(out + WT_WIRE_PAGE_END, after + count, 8);
        length = WT_WIRE_PAGE_EVENTS + count * WT_WIRE_EVENT_SIZE;
        break;
    }
    case WT_OP_SUBMIT:
        if (counter != server.nextCounter) { result = WT_RESULT_COUNTER_MISMATCH; break; }
        memcpy(server.pokemon, body + WT_WIRE_SUBMIT_POKEMON, 136); FxSha(server.pokemon, 136, server.digest);
        ++server.nextCounter;
        if (server.rejectSubmit) {
            AddEvent(WT_EVENT_REJECTED, NULL, request + WT_WIRE_REQUEST_ID, NULL, NULL, WT_TRADE_NONE);
            result = WT_RESULT_ILLEGAL_POKEMON; break;
        }
        memset(server.trade, 0x71, 16); memset(server.asset, 0x72, 16); server.state = WT_TRADE_QUEUED;
        AddEvent(WT_EVENT_SUBMITTED, server.trade, request + WT_WIRE_REQUEST_ID, server.asset, NULL, WT_TRADE_QUEUED);
        Put(out, server.eventCount, 8); Snapshot(out + WT_WIRE_OUTCOME_SNAPSHOT); length = WT_WIRE_OUTCOME_SIZE;
        break;
    case WT_OP_STATUS:
        Snapshot(out); length = WT_WIRE_SNAPSHOT_SIZE;
        break;
    case WT_OP_CANCEL:
        if (counter != server.nextCounter) { result = WT_RESULT_COUNTER_MISMATCH; break; }
        if (server.state != WT_TRADE_QUEUED) { result = WT_RESULT_CONFLICT; break; }
        ++server.nextCounter; memset(server.receipt, 0x73, 16); server.state = WT_TRADE_RETURN_READY;
        AddEvent(WT_EVENT_CANCELLED, server.trade, request + WT_WIRE_REQUEST_ID, server.asset, server.receipt, WT_TRADE_RETURN_READY);
        Put(out, server.eventCount, 8); Snapshot(out + WT_WIRE_OUTCOME_SNAPSHOT); length = WT_WIRE_OUTCOME_SIZE;
        break;
    case WT_OP_COLLECT:
        if (server.state != WT_TRADE_RETURN_READY) { result = WT_RESULT_CONFLICT; break; }
        Snapshot(out); memcpy(out + WT_WIRE_COLLECTION_POKEMON, server.pokemon, 136); length = WT_WIRE_COLLECTION_SIZE;
        break;
    case WT_OP_ACKNOWLEDGE:
        if (counter != server.nextCounter) { result = WT_RESULT_COUNTER_MISMATCH; break; }
        ++server.nextCounter; server.state = WT_TRADE_ACKNOWLEDGED;
        AddEvent(WT_EVENT_ACKNOWLEDGED, server.trade, request + WT_WIRE_REQUEST_ID, server.asset, server.receipt, WT_TRADE_ACKNOWLEDGED);
        Put(out, server.eventCount, 8); Snapshot(out + WT_WIRE_OUTCOME_SNAPSHOT); length = WT_WIRE_OUTCOME_SIZE;
        break;
    default:
        assert(0);
    }
    if (result != WT_RESULT_OK) length = 0;
    memset(reply, 0, WT_WIRE_REPLY_HEADER_SIZE);
    memcpy(reply, request, WT_WIRE_MAGIC_SIZE); reply[WT_WIRE_REPLY_OPERATION] = (unsigned char)operation;
    Put(reply + WT_WIRE_REPLY_BODY_LENGTH, length, 2);
    memcpy(reply + WT_WIRE_REPLY_REQUEST_ID, request + WT_WIRE_REQUEST_ID, 16);
    Put(reply + WT_WIRE_REPLY_RESULT, result, 2);
    Put(reply + WT_WIRE_REPLY_NEXT_COUNTER, server.nextCounter, 8);
    Put(reply + WT_WIRE_REPLY_HIGHWATER, server.eventCount, 8);
    *replySize = WT_WIRE_REPLY_HEADER_SIZE + length;
    return 0;
}



/* A counted request replays the same reply even if the client lost power
 * before recording it. Keep this state across the simulated client restart. */
typedef struct Replay {
    unsigned char request[WT_WIRE_REQUEST_MAX], reply[WT_WIRE_REPLY_MAX];
    size_t requestSize, replySize;
} Replay;
static Replay replay[16];
static unsigned replayCount;

static int Post(void *arg, const unsigned char *request, size_t size,
    unsigned char reply[WT_WIRE_REPLY_MAX], size_t *replySize)
{
    unsigned op = request[WT_WIRE_REQUEST_OPERATION], i;
    int counted = op == WT_OP_ENROLL || op == WT_OP_SUBMIT || op == WT_OP_CANCEL || op == WT_OP_ACKNOWLEDGE;
    if (counted) for (i = 0; i < replayCount; ++i) {
        if (memcmp(request + WT_WIRE_REQUEST_ID, replay[i].request + WT_WIRE_REQUEST_ID, 16) == 0) {
            assert(size == replay[i].requestSize && memcmp(request, replay[i].request, size) == 0);
            memcpy(reply, replay[i].reply, replay[i].replySize); *replySize = replay[i].replySize;
            return 0;
        }
    }
    assert(PostOnce(arg, request, size, reply, replySize) == 0);
    if (counted) {
        Replay *saved;
        assert(replayCount < 16); saved = &replay[replayCount++];
        memcpy(saved->request, request, size); saved->requestSize = size;
        memcpy(saved->reply, reply, *replySize); saved->replySize = *replySize;
    }
    return 0;
}
static const CpTransport transport = { NULL, Post, NULL, NULL, NULL };

#endif
