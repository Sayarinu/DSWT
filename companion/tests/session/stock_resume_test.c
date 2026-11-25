/* LUNE-899, end to end: the companion loses power at every write of a visit
 * (an offer, a cancel and its return, a rejected offer restored), the player
 * then loads and saves in the stock game, and only then opens
 * the companion again. The stock-first variant of the companion fault
 * harness, deterministic and in process: each cut runs on its own copy of the
 * save, the journal banks and the server while the interrupted visit carries
 * on. Every cut must settle (nothing left in escrow, restoring or applying),
 * keep the offered Pokémon in exactly one place, and finish its visit. */
#include "../adapter/stock_game.h"
#include "journal_fixture.h"

#include <stdio.h>

#include "session/visit.h"
#include "wonder_trade/wire_layout.h"

static int Hash(const unsigned char *bytes, size_t size, unsigned char digest[32]) { return FxSha(bytes, size, digest); }

/* --- The fake server of visit_test.c, with replay of counted requests. --- */
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

static int PostOnce(const unsigned char *request, size_t size, unsigned char reply[WT_WIRE_REPLY_MAX], size_t *replySize)
{
    const unsigned char *body = request + WT_WIRE_REQUEST_HEADER_SIZE;
    unsigned operation = request[WT_WIRE_REQUEST_OPERATION], result = WT_RESULT_OK;
    size_t length = 0;
    uint64_t counter = Get(request + WT_WIRE_REQUEST_COUNTER, 8);
    unsigned char *out = reply + WT_WIRE_REPLY_HEADER_SIZE;
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

/* A counted request replays its reply after a restart that lost it. */
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
    (void)arg;
    if (counted) for (i = 0; i < replayCount; ++i) {
        if (memcmp(request + WT_WIRE_REQUEST_ID, replay[i].request + WT_WIRE_REQUEST_ID, 16) == 0) {
            assert(size == replay[i].requestSize && memcmp(request, replay[i].request, size) == 0);
            memcpy(reply, replay[i].reply, replay[i].replySize); *replySize = replay[i].replySize;
            return 0;
        }
    }
    assert(PostOnce(request, size, reply, replySize) == 0);
    if (counted) {
        Replay *saved;
        assert(replayCount < 16); saved = &replay[replayCount++];
        memcpy(saved->request, request, size); saved->requestSize = size;
        memcpy(saved->reply, reply, *replySize); saved->replySize = *replySize;
    }
    return 0;
}

static const CpTransport transport = { NULL, Post, NULL, NULL, NULL };

/* --- Two worlds: the interrupted visit, and one cut's recovery. --- */

/* What survives a power cut: the save file and both journal banks. */
typedef struct World {
    unsigned char bytes[CP_SAVE_FILE_SIZE];
    unsigned char banks[2][WT_JOURNAL_RECORD_SIZE];
} World;

static World live, cut;
static FxFile stock;
static CpSaveImage liveImage, cutImage;
static WtGame liveGame, cutGame;
static unsigned randomCounter;
static int sweeping, recovering;
static void Cut(unsigned tear, uint32_t offset, const void *bytes, size_t size);

static int ReadAt(void *arg, uint32_t offset, void *data, size_t size)
{
    World *world = arg;
    if (offset > CP_SAVE_FILE_SIZE || size > CP_SAVE_FILE_SIZE - offset) return -1;
    memcpy(data, world->bytes + offset, size);
    return 0;
}

/* Outside the sweep: a commit of refusePhase fails once (cut before the
 * journal records it); after cutAfter is committed, save writes fail (cut
 * before the step writes) until cleared. */
static unsigned refusePhase, cutAfter;
static int saveCut;

static int WriteAt(void *arg, uint32_t offset, const void *data, size_t size)
{
    World *world = arg;
    if (offset > CP_SAVE_FILE_SIZE || size > CP_SAVE_FILE_SIZE - offset || (world == &live && saveCut)) return -1;
    if (world == &live) Cut(1, offset, data, size); /* the first half of this write landed */
    memcpy(world->bytes + offset, data, size);
    if (world == &live) Cut(0, 0, NULL, 0);
    return 0;
}

static int Sync(void *arg) { (void)arg; return 0; }
static int Size(void *arg, uint64_t *size) { (void)arg; *size = CP_SAVE_FILE_SIZE; return 0; }

static int ReadBank(void *arg, int bank, unsigned char record[WT_JOURNAL_RECORD_SIZE])
{
    memcpy(record, ((World *)arg)->banks[bank], WT_JOURNAL_RECORD_SIZE);
    return 0;
}

static int WriteBank(void *arg, int bank, const unsigned char record[WT_JOURNAL_RECORD_SIZE])
{
    World *world = arg;
    WtJournalEntry entry;
    assert(WtJournal_Decode(record, Hash, &entry) == 0);
    if (refusePhase && entry.phase == refusePhase) { refusePhase = 0; return -1; }
    memcpy(world->banks[bank], record, WT_JOURNAL_RECORD_SIZE);
    if (cutAfter && entry.phase == cutAfter) { cutAfter = 0; saveCut = 1; }
    if (world == &live) Cut(0, 0, NULL, 0);
    return 0;
}

static CpFile liveFile = { &live, ReadAt, WriteAt, Sync, Size }, cutFile = { &cut, ReadAt, WriteAt, Sync, Size };

static int Random(void *arg, unsigned char *bytes, size_t size)
{
    unsigned char input[4], digest[32];
    (void)arg;
    FxPut32(input, ++randomCounter); FxSha(input, 4, digest);
    memcpy(bytes, digest, size);
    return 0;
}

static void Open(WtEngine *engine, World *world, WtGame *game)
{
    WtJournalIO journal = { world, ReadBank, WriteBank, Hash };
    WtEngineIO io = { game, Random, WtGame_Inventory, WtGame_CanHold };
    assert(WtEngine_Open(engine, journal, io) == 0);
}

static void Bind(WtGame *game, CpSaveImage *image, const CpFile *file)
{
    memset(game, 0, sizeof(*game));
    game->image = image; game->file = file;
    assert(CpSaveImage_Load(image, file, CP_SAVE_MAGIC_INTERNATIONAL) == CP_SAVE_OK);
}

/* --- The sweep. --- */

enum { OFFERED_SLOT = 1 };
enum Scenario { OFFER, CANCEL_RETURN, REJECT_RESTORE, SCENARIOS };
static const char *const kNames[] = { "offer", "cancel-return", "reject-restore" };
enum Outcome { RECOVERED, STUCK, DUPLICATE, LOST, OUTCOMES };
static const char *const kOutcomes[] = { "recovered", "stuck", "duplicate", "lost" };
static enum Scenario scenario;
static unsigned char offered[136];
static unsigned tally[OUTCOMES], cases, writes;

static int Transitional(unsigned phase)
{
    return phase == WT_JOURNAL_ESCROW || phase == WT_JOURNAL_RESTORING || phase == WT_JOURNAL_APPLYING;
}

/* -1 lost, 0 exactly one, 1 duplicated: copies in the blocks the game would
 * load, plus the journal while it holds the offer. A step left unfinished
 * (stuck) holds it in the journal's escrow when the save does not. */
static int Ownership(const WtJournalEntry *entry)
{
    unsigned held = entry->phase == WT_JOURNAL_SUBMIT_READY || entry->phase == WT_JOURNAL_QUEUED
        || entry->phase == WT_JOURNAL_RECEIVED;
    unsigned copies;
    memcpy(stock.bytes, cut.bytes, sizeof(stock.bytes));
    copies = FxStockCopies(&stock, offered) + held;
    if (!copies && Transitional(entry->phase)) copies = 1;
    return copies == 1 ? 0 : copies ? 1 : -1;
}

static enum Outcome Recover(unsigned saves, int full)
{
    static unsigned char settledBytes[CP_SAVE_FILE_SIZE];
    WtEngine engine;
    WtJournalEntry settled;
    CpVisit visit;
    int slot = -1, result, ownership;
    /* The stock game, then the companion's boot settle. */
    memcpy(stock.bytes, cut.bytes, sizeof(stock.bytes));
    assert(FxStockPlay(&stock, saves, full, NULL));
    memcpy(cut.bytes, stock.bytes, sizeof(cut.bytes));
    if (CpSaveImage_Load(&cutImage, &cutFile, CP_SAVE_MAGIC_INTERNATIONAL) != CP_SAVE_OK) return STUCK;
    memset(&cutGame, 0, sizeof(cutGame)); cutGame.image = &cutImage; cutGame.file = &cutFile;
    Open(&engine, &cut, &cutGame);
    result = WtEngine_Settle(&engine);
    settled = engine.entry;
    WtEngine_Close(&engine);
    ownership = Ownership(&settled);
    if (ownership) return ownership > 0 ? DUPLICATE : LOST;
    if (result != 0 || Transitional(settled.phase)) return STUCK;
    /* Settling again changes nothing. */
    memcpy(settledBytes, cut.bytes, sizeof(settledBytes));
    Bind(&cutGame, &cutImage, &cutFile);
    Open(&engine, &cut, &cutGame);
    assert(WtEngine_Settle(&engine) == 0);
    assert(memcmp(&settled, &engine.entry, sizeof(settled)) == 0);
    assert(memcmp(settledBytes, cut.bytes, sizeof(settledBytes)) == 0);
    WtEngine_Close(&engine);
    /* The visit the player started finishes. */
    if (scenario != CANCEL_RETURN && settled.phase <= WT_JOURNAL_IDLE && server.eventCount <= 1) slot = OFFERED_SLOT;
    Open(&engine, &cut, &cutGame);
    result = CpVisit_Run(&engine, &cutGame, 7, slot, scenario == CANCEL_RETURN ? WT_OP_CANCEL : 0, &transport, &visit);
    settled = engine.entry;
    WtEngine_Close(&engine);
    ownership = Ownership(&settled);
    if (ownership) return ownership > 0 ? DUPLICATE : LOST;
    if (result != 0 || !visit.canLeave || settled.phase != (scenario == OFFER ? WT_JOURNAL_QUEUED : WT_JOURNAL_IDLE))
        return STUCK;
    return RECOVERED;
}

/* At a write of the interrupted visit: recover from what reached the card,
 * after one stock save of the general block, or two of both blocks. */
static void Cut(unsigned tear, uint32_t offset, const void *bytes, size_t size)
{
    static Server keptServer;
    static Replay keptReplay[16];
    unsigned keptReplayCount, keptRandom = randomCounter, saves;
    if (!sweeping || recovering || (tear && size < 2)) return;
    ++writes;
    keptServer = server; memcpy(keptReplay, replay, sizeof(replay)); keptReplayCount = replayCount;
    recovering = 1;
    for (saves = 1; saves <= 2; ++saves) {
        unsigned full = saves == 2;
        enum Outcome outcome;
        cut = live;
        if (tear) memcpy(cut.bytes + offset, bytes, size / 2);
        randomCounter = keptRandom + 1000; /* fresh entropy after the restart */
        outcome = Recover(saves, (int)full);
        ++tally[outcome]; ++cases;
        if (outcome != RECOVERED)
            printf("FAIL %s cut %u%s, %u stock save(s) of %s: %s\n", kNames[scenario], writes, tear ? " (torn)" : "",
                saves, full ? "both blocks" : "the general block", kOutcomes[outcome]);
        server = keptServer; memcpy(replay, keptReplay, sizeof(replay)); replayCount = keptReplayCount;
    }
    randomCounter = keptRandom;
    recovering = 0;
}

static void Setup(void)
{
    FxSpec specs[4];
    WtJournalEntry entry;
    unsigned i;
    static FxFile built;
    for (i = 0; i < 4; ++i) specs[i] = FxDefault(i);
    FxCanonical(&specs[OFFERED_SLOT], offered);
    FxBuild(&built, specs, 4);
    memcpy(live.bytes, built.bytes, sizeof(live.bytes));
    assert(WtTest_JournalEntry(WT_JOURNAL_EMPTY, Hash, &entry) == 0);
    assert(WtJournal_Encode(&entry, Hash, live.banks[0]) == 0);
    memcpy(live.banks[1], live.banks[0], sizeof(live.banks[0]));
    memset(&server, 0, sizeof(server)); memset(replay, 0, sizeof(replay)); replayCount = 0;
    randomCounter = 0;
    Bind(&liveGame, &liveImage, &liveFile);
    if (scenario == CANCEL_RETURN) {
        WtEngine engine;
        CpVisit visit;
        Open(&engine, &live, &liveGame);
        assert(CpVisit_Run(&engine, &liveGame, 7, OFFERED_SLOT, 0, &transport, &visit) == 0);
        assert(visit.phase == WT_JOURNAL_QUEUED);
        WtEngine_Close(&engine);
        Bind(&liveGame, &liveImage, &liveFile);
    }
    server.rejectSubmit = scenario == REJECT_RESTORE;
}

static void FillParty(unsigned char *general, unsigned char *boxes)
{
    unsigned count;
    (void)boxes;
    while ((count = Fx32(general + FX_PARTY + 4)) < 6) {
        FxSpec extra = FxDefault(20 + count);
        FxPartyMon(&extra, general + FX_PARTY + 8 + count * FX_MON);
        FxPut32(general + FX_PARTY + 4, count + 1);
    }
}

/* A cancelled offer collected for the party, cut once APPLYING is recorded;
 * the stock game fills the party; the recovery returns it to the PC and is
 * cut before APPLIED is recorded. The next settle must record APPLIED (a PC
 * destination, padded as the journal requires) and the ACK end the trade. */
static void RedirectThroughJournal(void)
{
    WtEngine engine;
    CpVisit visit;
    WtJournalEntry entry;
    scenario = CANCEL_RETURN;
    Setup();
    cutAfter = WT_JOURNAL_APPLYING;
    Open(&engine, &live, &liveGame);
    assert(CpVisit_Run(&engine, &liveGame, 7, -1, WT_OP_CANCEL, &transport, &visit) != 0);
    WtEngine_Close(&engine);
    saveCut = 0;
    memcpy(stock.bytes, live.bytes, sizeof(stock.bytes));
    assert(FxStockPlay(&stock, 1, 0, FillParty));
    memcpy(live.bytes, stock.bytes, sizeof(live.bytes));
    refusePhase = WT_JOURNAL_APPLIED;
    Bind(&liveGame, &liveImage, &liveFile);
    Open(&engine, &live, &liveGame);
    assert(engine.entry.phase == WT_JOURNAL_APPLYING && engine.entry.destination.kind == 1);
    assert(WtEngine_Settle(&engine) != 0);
    WtEngine_Close(&engine);
    memcpy(stock.bytes, live.bytes, sizeof(stock.bytes));
    assert(FxStockCopies(&stock, offered) == 1);
    Bind(&liveGame, &liveImage, &liveFile);
    Open(&engine, &live, &liveGame);
    assert(WtEngine_Settle(&engine) == 0 && engine.entry.phase == WT_JOURNAL_APPLIED);
    entry = engine.entry;
    WtEngine_Close(&engine);
    assert(entry.destination.kind == 2 && entry.destination.length == 136);
    Open(&engine, &live, &liveGame);
    assert(CpVisit_Run(&engine, &liveGame, 7, -1, 0, &transport, &visit) == 0 && visit.phase == WT_JOURNAL_IDLE);
    WtEngine_Close(&engine);
    assert(server.state == WT_TRADE_ACKNOWLEDGED);
    memcpy(stock.bytes, live.bytes, sizeof(stock.bytes));
    assert(FxStockCopies(&stock, offered) == 1);
    puts("PASS stock save during a return: a recovery into the PC commits APPLIED and is acknowledged");
}

/* LUNE-902: what the player does with the offer in the stock game. */
static void TrainOffered(unsigned char *general, unsigned char *boxes)
{
    (void)boxes;
    FxTrain(general + FX_PARTY + 8 + OFFERED_SLOT * FX_MON);
}

static void DepositOffered(unsigned char *general, unsigned char *boxes)
{
    unsigned char *party = general + FX_PARTY + 8;
    unsigned count = Fx32(general + FX_PARTY + 4), i;
    memcpy(FxPcSlot(boxes, 7), party + OFFERED_SLOT * FX_MON, FX_BOX_MON);
    for (i = OFFERED_SLOT; i + 1 < count; ++i) memcpy(party + i * FX_MON, party + (i + 1) * FX_MON, FX_MON);
    FxEmpty(party + i * FX_MON, FX_MON);
    FxPut32(general + FX_PARTY + 4, count - 1);
}

/* An offer cut once ESCROW is recorded, before its removal wrote anything,
 * then changed in the stock game: the next visit abandons the escrow, which
 * was never sent, and leaves the Pokémon as the stock game saved it. The same
 * escrow restored after another copy of the save did send its Submit finds
 * the offer on the server and is never abandoned. */
static void UnsentOffer(void)
{
    static World escrow, played;
    static unsigned char before[CP_SAVE_FILE_SIZE];
    static const FxStockAction actions[] = { TrainOffered, DepositOffered };
    static const char *const kActions[] = { "trained", "deposited in the PC" };
    Server fresh;
    WtEngine engine;
    CpVisit visit;
    unsigned action;
    scenario = OFFER;
    for (action = 0; action < 2; ++action) {
        Setup();
        cutAfter = WT_JOURNAL_ESCROW;
        Open(&engine, &live, &liveGame);
        assert(CpVisit_Run(&engine, &liveGame, 7, OFFERED_SLOT, 0, &transport, &visit) != 0);
        WtEngine_Close(&engine);
        saveCut = 0;
        assert(visit.phase == WT_JOURNAL_ESCROW && server.state == WT_TRADE_NONE);
        escrow = live; fresh = server;
        /* The stock game: the offer is changed, and is the save's only copy. */
        memcpy(stock.bytes, live.bytes, sizeof(stock.bytes));
        assert(FxStockPlay(&stock, 1, 0, actions[action]));
        memcpy(live.bytes, stock.bytes, sizeof(live.bytes));
        played = live;
        assert(FxStockCopies(&stock, offered) == 1);
        Bind(&liveGame, &liveImage, &liveFile);
        Open(&engine, &live, &liveGame);
        assert(WtEngine_Settle(&engine) == WT_INVENTORY_KEPT && engine.entry.phase == WT_JOURNAL_ESCROW);
        WtEngine_Close(&engine);
        Open(&engine, &live, &liveGame);
        assert(CpVisit_Run(&engine, &liveGame, 7, -1, 0, &transport, &visit) == 0);
        WtEngine_Close(&engine);
        assert(visit.phase == WT_JOURNAL_IDLE && visit.canLeave);
        assert(memcmp(live.bytes, played.bytes, sizeof(live.bytes)) == 0);
        assert(!server.requests[WT_OP_SUBMIT] && server.state == WT_TRADE_NONE);
        memcpy(stock.bytes, live.bytes, sizeof(stock.bytes));
        assert(FxStockCopies(&stock, offered) == 1);
        /* The save trades again: the changed Pokémon is offered and leaves it. */
        Bind(&liveGame, &liveImage, &liveFile);
        Open(&engine, &live, &liveGame);
        assert(CpVisit_Run(&engine, &liveGame, 7, 0, 0, &transport, &visit) == 0 && visit.phase == WT_JOURNAL_QUEUED);
        WtEngine_Close(&engine);
        printf("PASS unsent offer %s in the stock game: abandoned, kept in the save, nothing sent\n", kActions[action]);

        /* Another copy of the escrow finished the removal and sent the Submit. */
        live = escrow; server = fresh; memset(replay, 0, sizeof(replay)); replayCount = 0;
        Bind(&liveGame, &liveImage, &liveFile);
        Open(&engine, &live, &liveGame);
        assert(CpVisit_Run(&engine, &liveGame, 7, -1, 0, &transport, &visit) == 0 && visit.phase == WT_JOURNAL_QUEUED);
        WtEngine_Close(&engine);
        assert(server.state == WT_TRADE_QUEUED);
        /* This copy, changed in the stock game, is never abandoned. */
        live = played;
        Bind(&liveGame, &liveImage, &liveFile);
        memcpy(before, live.bytes, sizeof(before));
        Open(&engine, &live, &liveGame);
        assert(CpVisit_Run(&engine, &liveGame, 7, -1, 0, &transport, &visit) != 0);
        assert(engine.blocked && engine.entry.phase == WT_JOURNAL_ESCROW && !visit.canLeave);
        WtEngine_Close(&engine);
        assert(memcmp(live.banks, played.banks, sizeof(live.banks)) == 0);
        assert(memcmp(live.bytes, before, sizeof(before)) == 0 && server.state == WT_TRADE_QUEUED);
        printf("PASS unsent offer %s in the stock game: a server holding it is never abandoned\n", kActions[action]);
    }
}

int main(void)
{
    unsigned total[OUTCOMES] = { 0, 0, 0, 0 }, i;
    for (scenario = OFFER; scenario < SCENARIOS; ++scenario) {
        WtEngine engine;
        CpVisit visit;
        memset(tally, 0, sizeof(tally)); cases = writes = 0;
        Setup();
        sweeping = 1;
        Open(&engine, &live, &liveGame);
        assert(CpVisit_Run(&engine, &liveGame, 7, scenario == CANCEL_RETURN ? -1 : OFFERED_SLOT,
            scenario == CANCEL_RETURN ? WT_OP_CANCEL : 0, &transport, &visit) == 0);
        assert(visit.phase == (scenario == OFFER ? WT_JOURNAL_QUEUED : WT_JOURNAL_IDLE));
        WtEngine_Close(&engine);
        sweeping = 0;
        printf("%s: %u cuts (each write, and each save write torn), %u stock-save cases: "
            "recovered=%u stuck=%u duplicate=%u lost=%u\n", kNames[scenario], writes, cases,
            tally[RECOVERED], tally[STUCK], tally[DUPLICATE], tally[LOST]);
        assert(writes > 0 && cases == 2 * writes);
        for (i = 0; i < OUTCOMES; ++i) total[i] += tally[i];
    }
    assert(total[STUCK] == 0 && total[DUPLICATE] == 0 && total[LOST] == 0);
    RedirectThroughJournal();
    UnsentOffer();
    printf("PASS stock save before resume: %u cases recovered, 0 stuck, 0 duplicates, 0 lost\n", total[RECOVERED]);
    return 0;
}
