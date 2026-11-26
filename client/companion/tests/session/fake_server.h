#ifndef COMPANION_TEST_FAKE_SERVER_H
#define COMPANION_TEST_FAKE_SERVER_H

/* An in-memory Wonder Trade server for the visit and session model tests:
 * one save, one trade, the wire contract's replies (CpTransport.post). */
#include "../adapter/save_fixture.h"

#include "wonder_trade/wire.h"
#include "wonder_trade/wire_layout.h"

static inline int FsHash(const unsigned char *bytes, size_t size, unsigned char digest[32]) { return FxSha(bytes, size, digest); }

/* --- The fake server: one save, one trade, the wire contract's replies. --- */
typedef struct FsEvent {
    unsigned char bytes[WT_WIRE_EVENT_SIZE];
} FsEvent;

typedef struct FsServer {
    unsigned char saveID[16], trade[16], asset[16], receipt[16], digest[32], pokemon[136];
    /* A matched partner's Pokémon, once FsMatch was called. */
    unsigned char matchAsset[16], matchDigest[32], matchPokemon[136];
    int matched;
    uint64_t nextCounter;
    FsEvent events[16];
    unsigned eventCount, state, rejectSubmit, requests[8];
} FsServer;

static FsServer server;
static unsigned fsEnrollments;

static inline uint64_t FsGet(const unsigned char *p, unsigned size)
{
    uint64_t value = 0;
    while (size--) value = (value << 8) | p[size];
    return value;
}

static inline void FsPut(unsigned char *p, uint64_t value, unsigned size)
{
    while (size--) { *p++ = (unsigned char)value; value >>= 8; }
}

static inline void FsAddEvent(unsigned kind, const unsigned char *trade, const unsigned char *operation,
    const unsigned char *asset, const unsigned char *receipt, unsigned state)
{
    unsigned char *e = server.events[server.eventCount].bytes;
    memset(e, 0, WT_WIRE_EVENT_SIZE);
    FsPut(e + WT_WIRE_EVENT_SEQUENCE, ++server.eventCount, 8); e[WT_WIRE_EVENT_KIND] = (unsigned char)kind;
    if (trade) memcpy(e + WT_WIRE_EVENT_TRADE, trade, 16);
    if (operation) memcpy(e + WT_WIRE_EVENT_OPERATION, operation, 16);
    if (asset) memcpy(e + WT_WIRE_EVENT_ASSET, asset, 16);
    if (kind != WT_EVENT_ENROLLED) memcpy(e + WT_WIRE_EVENT_DIGEST, server.digest, 32);
    if (receipt) memcpy(e + WT_WIRE_EVENT_RECEIPT, receipt, 16);
    e[WT_WIRE_EVENT_STATE] = (unsigned char)state;
}

static inline void FsSnapshot(unsigned char *s)
{
    memset(s, 0, WT_WIRE_SNAPSHOT_SIZE);
    memcpy(s + WT_WIRE_SNAPSHOT_TRADE, server.trade, 16); s[WT_WIRE_SNAPSHOT_STATE] = (unsigned char)server.state;
    memcpy(s + WT_WIRE_SNAPSHOT_OFFERED_ASSET, server.asset, 16);
    memcpy(s + WT_WIRE_SNAPSHOT_OFFERED_DIGEST, server.digest, 32);
    if (server.matched) {
        s[WT_WIRE_SNAPSHOT_DELIVERY_KIND] = WT_DELIVERY_MATCH;
        memcpy(s + WT_WIRE_SNAPSHOT_RECEIVED_ASSET, server.matchAsset, 16);
        memcpy(s + WT_WIRE_SNAPSHOT_RECEIVED_DIGEST, server.matchDigest, 32);
        memcpy(s + WT_WIRE_SNAPSHOT_RECEIPT, server.receipt, 16);
    } else if (server.state >= WT_TRADE_RETURN_READY) {
        s[WT_WIRE_SNAPSHOT_DELIVERY_KIND] = WT_DELIVERY_RETURN;
        memcpy(s + WT_WIRE_SNAPSHOT_RECEIVED_ASSET, server.asset, 16);
        memcpy(s + WT_WIRE_SNAPSHOT_RECEIVED_DIGEST, server.digest, 32);
        memcpy(s + WT_WIRE_SNAPSHOT_RECEIPT, server.receipt, 16);
    }
    FsPut(s + WT_WIRE_SNAPSHOT_LAST_EVENT, server.eventCount, 8);
}

static inline int FsPost(void *arg, const unsigned char *request, size_t size,
    unsigned char reply[WT_WIRE_REPLY_MAX], size_t *replySize)
{
    const unsigned char *body = request + WT_WIRE_REQUEST_HEADER_SIZE;
    unsigned operation = request[WT_WIRE_REQUEST_OPERATION], result = WT_RESULT_OK;
    size_t length = 0;
    uint64_t counter = FsGet(request + WT_WIRE_REQUEST_COUNTER, 8);
    unsigned char *out = reply + WT_WIRE_REPLY_HEADER_SIZE;
    (void)arg;
    assert(WtWire_CheckRequest(request, size, FsHash) == 0);
    ++server.requests[operation];
    switch (operation) {
    case WT_OP_ENROLL:
        memset(server.saveID, 0x51, 16); server.nextCounter = 1;
        server.saveID[0] = (unsigned char)++fsEnrollments; /* each enrollment its own save */
        if (!server.eventCount) FsAddEvent(WT_EVENT_ENROLLED, NULL, NULL, NULL, NULL, WT_TRADE_NONE);
        memcpy(out, server.saveID, 16); length = 16;
        break;
    case WT_OP_RECONCILE: {
        uint64_t after = FsGet(body + WT_WIRE_RECONCILE_AFTER, 8), i;
        unsigned limit = body[WT_WIRE_RECONCILE_LIMIT], count = 0;
        for (i = after; i < server.eventCount && count < limit; ++i, ++count)
            memcpy(out + WT_WIRE_PAGE_EVENTS + count * WT_WIRE_EVENT_SIZE, server.events[i].bytes, WT_WIRE_EVENT_SIZE);
        out[WT_WIRE_PAGE_COUNT] = (unsigned char)count; out[WT_WIRE_PAGE_HAS_MORE] = after + count < server.eventCount;
        FsPut(out + WT_WIRE_PAGE_END, after + count, 8);
        length = WT_WIRE_PAGE_EVENTS + count * WT_WIRE_EVENT_SIZE;
        break;
    }
    case WT_OP_SUBMIT:
        if (counter != server.nextCounter) { result = WT_RESULT_COUNTER_MISMATCH; break; }
        memcpy(server.pokemon, body + WT_WIRE_SUBMIT_POKEMON, 136); FxSha(server.pokemon, 136, server.digest);
        ++server.nextCounter;
        if (server.rejectSubmit) {
            FsAddEvent(WT_EVENT_REJECTED, NULL, request + WT_WIRE_REQUEST_ID, NULL, NULL, WT_TRADE_NONE);
            result = WT_RESULT_ILLEGAL_POKEMON; break;
        }
        memset(server.trade, 0x71, 16); memset(server.asset, 0x72, 16); server.state = WT_TRADE_QUEUED;
        /* Each offer is its own trade. */
        server.trade[0] = server.asset[0] = (unsigned char)server.requests[WT_OP_SUBMIT]; server.matched = 0;
        FsAddEvent(WT_EVENT_SUBMITTED, server.trade, request + WT_WIRE_REQUEST_ID, server.asset, NULL, WT_TRADE_QUEUED);
        FsPut(out, server.eventCount, 8); FsSnapshot(out + WT_WIRE_OUTCOME_SNAPSHOT); length = WT_WIRE_OUTCOME_SIZE;
        break;
    case WT_OP_STATUS:
        FsSnapshot(out); length = WT_WIRE_SNAPSHOT_SIZE;
        break;
    case WT_OP_CANCEL:
        if (counter != server.nextCounter) { result = WT_RESULT_COUNTER_MISMATCH; break; }
        if (server.state == WT_TRADE_NONE || memcmp(body, server.trade, 16) != 0) {
            /* As the server: a trade this save does not hold is rejected and
             * recorded, using the counter (LUNE-902's fence relies on it). */
            ++server.nextCounter; result = WT_RESULT_NOT_FOUND;
            FsAddEvent(WT_EVENT_REJECTED, NULL, request + WT_WIRE_REQUEST_ID, NULL, NULL, WT_TRADE_NONE);
            memset(server.events[server.eventCount - 1].bytes + WT_WIRE_EVENT_DIGEST, 0, 32);
            break;
        }
        if (server.state != WT_TRADE_QUEUED) { result = WT_RESULT_CONFLICT; break; }
        ++server.nextCounter; memset(server.receipt, 0x73, 16); server.state = WT_TRADE_RETURN_READY;
        FsAddEvent(WT_EVENT_CANCELLED, server.trade, request + WT_WIRE_REQUEST_ID, server.asset, server.receipt, WT_TRADE_RETURN_READY);
        FsPut(out, server.eventCount, 8); FsSnapshot(out + WT_WIRE_OUTCOME_SNAPSHOT); length = WT_WIRE_OUTCOME_SIZE;
        break;
    case WT_OP_COLLECT:
        if (server.state != WT_TRADE_RETURN_READY && server.state != WT_TRADE_MATCHED) { result = WT_RESULT_CONFLICT; break; }
        FsSnapshot(out); memcpy(out + WT_WIRE_COLLECTION_POKEMON, server.matched ? server.matchPokemon : server.pokemon, 136);
        length = WT_WIRE_COLLECTION_SIZE;
        break;
    case WT_OP_ACKNOWLEDGE:
        if (counter != server.nextCounter) { result = WT_RESULT_COUNTER_MISMATCH; break; }
        ++server.nextCounter; server.state = WT_TRADE_ACKNOWLEDGED;
        FsAddEvent(WT_EVENT_ACKNOWLEDGED, server.trade, request + WT_WIRE_REQUEST_ID, server.asset, server.receipt, WT_TRADE_ACKNOWLEDGED);
        FsPut(out, server.eventCount, 8); FsSnapshot(out + WT_WIRE_OUTCOME_SNAPSHOT); length = WT_WIRE_OUTCOME_SIZE;
        break;
    default:
        assert(0);
    }
    if (result != WT_RESULT_OK) length = 0;
    memset(reply, 0, WT_WIRE_REPLY_HEADER_SIZE);
    memcpy(reply, request, WT_WIRE_MAGIC_SIZE); reply[WT_WIRE_REPLY_OPERATION] = (unsigned char)operation;
    FsPut(reply + WT_WIRE_REPLY_BODY_LENGTH, length, 2);
    memcpy(reply + WT_WIRE_REPLY_REQUEST_ID, request + WT_WIRE_REQUEST_ID, 16);
    FsPut(reply + WT_WIRE_REPLY_RESULT, result, 2);
    FsPut(reply + WT_WIRE_REPLY_NEXT_COUNTER, server.nextCounter, 8);
    FsPut(reply + WT_WIRE_REPLY_HIGHWATER, server.eventCount, 8);
    *replySize = WT_WIRE_REPLY_HEADER_SIZE + length;
    return 0;
}

/* A partner's offer matches the queued one: Collect then delivers `pokemon`. */
static inline void FsMatch(const unsigned char pokemon[136])
{
    memcpy(server.matchPokemon, pokemon, 136); FxSha(pokemon, 136, server.matchDigest);
    memset(server.matchAsset, 0x74, 16); memset(server.receipt, 0x75, 16);
    server.matched = 1; server.state = WT_TRADE_MATCHED;
    FsAddEvent(WT_EVENT_MATCHED, server.trade, NULL, server.matchAsset, server.receipt, WT_TRADE_MATCHED);
    /* A match event carries the digest of what this save receives. */
    memcpy(server.events[server.eventCount - 1].bytes + WT_WIRE_EVENT_DIGEST, server.matchDigest, 32);
}

#endif
