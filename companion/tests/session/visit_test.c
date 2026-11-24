/* End to end on a synthetic save: the companion enrolls, offers a party
 * Pokémon, checks its status and cancels it, through the engine, the visit
 * driver and the Platinum adapter, against an in-memory fake server. A
 * rejected offer is restored to its own slot. */
#include "../adapter/save_fixture.h"
#include "journal_fixture.h"

#include <stdio.h>

#include "session/visit.h"
#include "wonder_trade/wire_layout.h"

static FxFile file;
static CpSaveImage image;
static CpFile cpFile = { &file, FxRead, FxWrite, FxSync, FxSize };
static WtGame game;
static unsigned char banks[2][WT_JOURNAL_RECORD_SIZE];
static unsigned randomCounter;

static int Hash(const unsigned char *bytes, size_t size, unsigned char digest[32]) { return FxSha(bytes, size, digest); }

static int ReadBank(void *arg, int bank, unsigned char record[WT_JOURNAL_RECORD_SIZE])
{
    (void)arg; memcpy(record, banks[bank], WT_JOURNAL_RECORD_SIZE); return 0;
}

static int WriteBank(void *arg, int bank, const unsigned char record[WT_JOURNAL_RECORD_SIZE])
{
    (void)arg; memcpy(banks[bank], record, WT_JOURNAL_RECORD_SIZE); return 0;
}

static int Random(void *arg, unsigned char *bytes, size_t size)
{
    unsigned char input[4], digest[32];
    (void)arg;
    FxPut32(input, ++randomCounter); FxSha(input, 4, digest);
    memcpy(bytes, digest, size); return 0;
}

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

static int Post(void *arg, const unsigned char *request, size_t size,
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

static const CpTransport transport = { NULL, Post, NULL, NULL };

static void Open(WtEngine *engine)
{
    WtJournalIO journal = { NULL, ReadBank, WriteBank, Hash };
    WtEngineIO io = { &game, Random, WtGame_Inventory, WtGame_CanHold };
    assert(WtEngine_Open(engine, journal, io) == 0);
}

static void Fresh(unsigned count)
{
    FxSpec specs[6];
    WtJournalEntry entry;
    unsigned i;
    for (i = 0; i < count; ++i) specs[i] = FxDefault(i);
    FxBuild(&file, specs, count);
    assert(CpSaveImage_Load(&image, &cpFile, CP_SAVE_MAGIC_INTERNATIONAL) == CP_SAVE_OK);
    memset(&game, 0, sizeof(game)); game.image = &image; game.file = &cpFile;
    memset(&server, 0, sizeof(server));
    assert(WtTest_JournalEntry(WT_JOURNAL_EMPTY, Hash, &entry) == 0);
    assert(WtJournal_Encode(&entry, Hash, banks[0]) == 0);
    memcpy(banks[1], banks[0], sizeof(banks[0]));
}

static void Party(unsigned char party[8 + 6 * FX_MON])
{
    memcpy(party, CpSaveImage_Block(&image, 0) + FX_PARTY, 8 + 6 * FX_MON);
}

static void OfferStatusCancel(void)
{
    WtEngine engine;
    CpVisit visit;
    unsigned char before[8 + 6 * FX_MON], digest[32], original[FX_MON];
    Fresh(4);
    Party(before);
    memcpy(original, before + 8 + 1 * FX_MON, FX_MON);
    /* Enroll, reconcile, offer slot 1, then the status of the queued offer. */
    Open(&engine);
    assert(CpVisit_Run(&engine, &game, 7, 1, 0, &transport, &visit) == 0);
    assert(visit.phase == WT_JOURNAL_QUEUED && visit.canLeave && !visit.completed);
    assert(server.requests[WT_OP_ENROLL] == 1 && server.requests[WT_OP_SUBMIT] == 1 && server.requests[WT_OP_STATUS] == 1);
    assert(engine.entry.source.kind == 1 && engine.entry.source.slot == 1 && engine.entry.source.length == 236);
    assert(memcmp(engine.entry.original, original, FX_MON) == 0);
    assert(FxImageCount(&image) == 3);
    WtEngine_Close(&engine);
    /* The save on the card holds the removal in both copies. */
    assert(CpSaveImage_Load(&image, &cpFile, CP_SAVE_MAGIC_INTERNATIONAL) == CP_SAVE_OK);
    assert(FxImageCount(&image) == 3 && memcmp(FxImageParty(&image, 1), before + 8 + 2 * FX_MON, FX_MON) == 0);
    puts("PASS visit: enroll, offer and status leave the offer queued and the save without it");

    /* A later visit cancels: the offer returns to the end of the party. */
    Open(&engine);
    assert(CpVisit_Run(&engine, &game, 7, -1, WT_OP_CANCEL, &transport, &visit) == 0);
    assert(visit.completed && visit.returned && visit.phase == WT_JOURNAL_IDLE && visit.canLeave);
    assert(server.requests[WT_OP_CANCEL] == 1 && server.requests[WT_OP_COLLECT] == 1 && server.requests[WT_OP_ACKNOWLEDGE] == 1);
    WtEngine_Close(&engine);
    assert(CpSaveImage_Load(&image, &cpFile, CP_SAVE_MAGIC_INTERNATIONAL) == CP_SAVE_OK);
    assert(FxImageCount(&image) == 4 && memcmp(FxImageParty(&image, 3), original, FX_MON) == 0);
    assert(memcmp(FxImageParty(&image, 0), before + 8, FX_MON) == 0);
    assert(CpSaveImage_Hash(&image, digest) == CP_SAVE_OK && CpSaveImage_Verify(&image, &cpFile, digest) == CP_SAVE_OK);
    /* An ordinary visit after that has nothing to do. */
    Open(&engine);
    assert(CpVisit_Run(&engine, &game, 7, -1, 0, &transport, &visit) == 0 && visit.phase == WT_JOURNAL_IDLE);
    WtEngine_Close(&engine);
    puts("PASS visit: cancel collects the return and acknowledges it");
}

static void RejectedOfferRestoresInPlace(void)
{
    WtEngine engine;
    CpVisit visit;
    unsigned char before[8 + 6 * FX_MON];
    Fresh(3);
    Party(before);
    server.rejectSubmit = 1;
    Open(&engine);
    assert(CpVisit_Run(&engine, &game, 7, 0, 0, &transport, &visit) == 0);
    assert(visit.rejected && visit.phase == WT_JOURNAL_IDLE && visit.canLeave && !visit.completed);
    assert(server.requests[WT_OP_SUBMIT] == 1 && server.requests[WT_OP_RECONCILE] == 2);
    WtEngine_Close(&engine);
    /* Removed, then put back in its own slot: the party is byte for byte as before. */
    assert(CpSaveImage_Load(&image, &cpFile, CP_SAVE_MAGIC_INTERNATIONAL) == CP_SAVE_OK);
    assert(memcmp(CpSaveImage_Block(&image, 0) + FX_PARTY, before, sizeof(before)) == 0);
    puts("PASS visit: a rejected offer is restored to its own slot");
}

static void Failures(void)
{
    WtEngine engine;
    CpVisit visit;
    Fresh(1);
    /* The only Pokémon cannot be offered: the deposit fails and nothing is sent. */
    Open(&engine);
    assert(CpVisit_Run(&engine, &game, 7, 0, 0, &transport, &visit) == CP_VISIT_ERROR_DEPOSIT);
    assert(server.requests[WT_OP_SUBMIT] == 0 && FxImageCount(&image) == 1);
    WtEngine_Close(&engine);
    puts("PASS visit: an ineligible offer sends nothing");
}

int main(void)
{
    OfferStatusCancel();
    RejectedOfferRestoresInPlace();
    Failures();
    puts("PASS visit driver");
    return 0;
}
