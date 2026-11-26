/* LUNE-899, end to end: the companion loses power at every write of a visit
 * (an offer, a cancel and its return, a rejected offer restored), the player
 * then loads and saves in the stock game, and only then opens
 * the companion again. The stock-first variant of the companion fault
 * harness, deterministic and in process: each cut runs on its own copy of the
 * save, the journal banks and the server while the interrupted visit carries
 * on. Every cut must settle (nothing left in escrow, restoring or applying),
 * keep the offered Pokémon in exactly one place, and finish its visit.
 *
 * LUNE-906: a return the stock game saved over is never put back again (the
 * games' never-duplicate rule, client wonder_trade/delivery.h). Each cut while
 * a return is in flight (RESTORING or APPLYING) is also played with the
 * player releasing or trading away the offer in the stock game, when the save
 * it loaded holds it: it must stay gone. The only accepted loss is the games'
 * window: a return in flight that no complete retail copy held yet when the
 * stock game loaded the save. */
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
        if (server.state == WT_TRADE_NONE || memcmp(body, server.trade, 16) != 0) {
            /* As the server: a trade this save does not hold is rejected and
             * recorded, using the counter (LUNE-902's fence relies on it). */
            ++server.nextCounter; result = WT_RESULT_NOT_FOUND;
            AddEvent(WT_EVENT_REJECTED, NULL, request + WT_WIRE_REQUEST_ID, NULL, NULL, WT_TRADE_NONE);
            memset(server.events[server.eventCount - 1].bytes + WT_WIRE_EVENT_DIGEST, 0, 32);
            break;
        }
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
enum Outcome { RECOVERED, WINDOW, STUCK, DUPLICATE, LOST, OUTCOMES };
static const char *const kOutcomes[] = { "recovered", "lost-in-window", "stuck", "duplicate", "lost" };
static enum Scenario scenario;
static unsigned char offered[136];
static unsigned tally[OUTCOMES], cases, writes, letGoCases;
/* What the player does with the offer in the stock game: keep it, release
 * it or trade it away. */
enum Fate { KEPT, RELEASED, TRADED, FATES };
static enum Fate fate;
static void LetGo(unsigned char *general, unsigned char *boxes) { (void)FxGone(general, boxes, offered, fate == TRADED); }

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
    /* Let go in the stock game: it must stay gone. */
    if (fate != KEPT) return copies ? 1 : 0;
    if (!copies && Transitional(entry->phase)) copies = 1;
    return copies == 1 ? 0 : copies ? 1 : -1;
}

/* The phase the journal recorded when the power went. */
static unsigned CutPhase(void)
{
    WtJournalEntry entry;
    WtEngine engine;
    unsigned phase;
    memset(&cutGame, 0, sizeof(cutGame));
    Open(&engine, &cut, &cutGame);
    entry = engine.entry;
    WtEngine_Close(&engine);
    phase = entry.phase;
    return entry.deliveryKind == WT_DELIVERY_RETURN || phase == WT_JOURNAL_RESTORING ? phase : 0;
}

/* -1 when the fate does not apply to this cut. */
static int Recover(unsigned saves, int full)
{
    static unsigned char settledBytes[CP_SAVE_FILE_SIZE];
    WtEngine engine;
    WtJournalEntry settled;
    CpVisit visit;
    int slot = -1, result, ownership, window;
    unsigned phase = CutPhase(), loaded;
    /* A return in flight: RESTORING in place, or APPLYING. */
    int inFlight = phase == WT_JOURNAL_RESTORING || phase == WT_JOURNAL_APPLYING;
    memcpy(stock.bytes, cut.bytes, sizeof(stock.bytes));
    loaded = FxStockCopies(&stock, offered);
    if (fate != KEPT && !(inFlight && loaded)) return -1;
    /* The games' window: in flight, and not in the save the stock game loads. */
    window = inFlight && !loaded;
    /* The stock game, then the companion's boot settle. */
    assert(FxStockPlay(&stock, saves, full, fate == KEPT ? NULL : LetGo));
    memcpy(cut.bytes, stock.bytes, sizeof(cut.bytes));
    if (CpSaveImage_Load(&cutImage, &cutFile, CP_SAVE_MAGIC_INTERNATIONAL) != CP_SAVE_OK) return STUCK;
    memset(&cutGame, 0, sizeof(cutGame)); cutGame.image = &cutImage; cutGame.file = &cutFile;
    Open(&engine, &cut, &cutGame);
    result = WtEngine_Settle(&engine);
    settled = engine.entry;
    WtEngine_Close(&engine);
    ownership = Ownership(&settled);
    if (ownership) return ownership > 0 ? DUPLICATE : window ? WINDOW : LOST;
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
    if (ownership) return ownership > 0 ? DUPLICATE : window ? WINDOW : LOST;
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
    for (fate = KEPT; fate < FATES; ++fate) for (saves = 1; saves <= 2; ++saves) {
        unsigned full = saves == 2;
        int outcome;
        cut = live;
        if (tear) memcpy(cut.bytes + offset, bytes, size / 2);
        randomCounter = keptRandom + 1000; /* fresh entropy after the restart */
        outcome = Recover(saves, (int)full);
        server = keptServer; memcpy(replay, keptReplay, sizeof(replay)); replayCount = keptReplayCount;
        if (outcome < 0) continue;
        ++tally[outcome];
        if (fate == KEPT) ++cases; else ++letGoCases;
        if (outcome != RECOVERED && outcome != WINDOW)
            printf("FAIL %s cut %u%s, offer %s, %u stock save(s) of %s: %s\n", kNames[scenario], writes,
                tear ? " (torn)" : "", fate == KEPT ? "kept" : fate == RELEASED ? "released" : "traded away",
                saves, full ? "both blocks" : "the general block", kOutcomes[outcome]);
    }
    fate = KEPT;
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

/* The stock game: the returned offer, from the party into PC box 0 slot 0. */
static void DepositOffered(unsigned char *general, unsigned char *boxes)
{
    unsigned char canonical[136], *party = general + FX_PARTY;
    unsigned count = Fx32(party + 4), slot, i;
    for (slot = 0; slot < count; ++slot) {
        assert(WtPokemon_Decode(party + 8 + slot * FX_MON, canonical) == 0);
        if (memcmp(canonical, offered, 136) == 0) break;
    }
    assert(slot < count);
    memcpy(FxPcSlot(boxes, 0), party + 8 + slot * FX_MON, FX_BOX_MON);
    for (i = slot; i + 1 < count; ++i) memcpy(party + 8 + i * FX_MON, party + 8 + (i + 1) * FX_MON, FX_MON);
    FxEmpty(party + 8 + (count - 1) * FX_MON, FX_MON);
    FxPut32(party + 4, count - 1);
}

/* A cancelled offer collected for the party: cut once APPLYING is recorded
 * (before the return is written) or before APPLIED is (after), then the stock
 * game, then the companion. The settle records APPLIED and the ACK ends the
 * trade; the return is never added again (LUNE-906). */
static void CollectCut(unsigned phase, FxStockAction action)
{
    WtEngine engine;
    CpVisit visit;
    scenario = CANCEL_RETURN;
    Setup();
    if (phase == WT_JOURNAL_APPLYING) cutAfter = phase; else refusePhase = phase;
    Open(&engine, &live, &liveGame);
    assert(CpVisit_Run(&engine, &liveGame, 7, -1, WT_OP_CANCEL, &transport, &visit) != 0);
    WtEngine_Close(&engine);
    saveCut = 0;
    memcpy(stock.bytes, live.bytes, sizeof(stock.bytes));
    assert(FxStockCopies(&stock, offered) == (phase == WT_JOURNAL_APPLIED));
    assert(FxStockPlay(&stock, 1, 0, action));
    memcpy(live.bytes, stock.bytes, sizeof(live.bytes));
}

static void Acknowledged(unsigned copies)
{
    WtEngine engine;
    CpVisit visit;
    Open(&engine, &live, &liveGame);
    assert(CpVisit_Run(&engine, &liveGame, 7, -1, 0, &transport, &visit) == 0 && visit.phase == WT_JOURNAL_IDLE);
    WtEngine_Close(&engine);
    assert(server.state == WT_TRADE_ACKNOWLEDGED);
    memcpy(stock.bytes, live.bytes, sizeof(stock.bytes));
    assert(FxStockCopies(&stock, offered) == copies);
}

static void RedirectThroughJournal(void)
{
    WtEngine engine;
    WtJournalEntry entry;
    /* Written, then deposited in the PC: APPLIED records it there. */
    CollectCut(WT_JOURNAL_APPLIED, DepositOffered);
    Bind(&liveGame, &liveImage, &liveFile);
    Open(&engine, &live, &liveGame);
    assert(engine.entry.phase == WT_JOURNAL_APPLYING && engine.entry.destination.kind == 1);
    assert(WtEngine_Settle(&engine) == 0 && engine.entry.phase == WT_JOURNAL_APPLIED);
    entry = engine.entry;
    WtEngine_Close(&engine);
    assert(entry.destination.kind == 2 && entry.destination.box == 0 && entry.destination.slot == 0
        && entry.destination.length == 136);
    Acknowledged(1);
    puts("PASS stock save during a return: one deposited since commits APPLIED in the PC and is acknowledged");
    /* Not written, and the party filled: APPLIED without it, never added. */
    CollectCut(WT_JOURNAL_APPLYING, FillParty);
    Bind(&liveGame, &liveImage, &liveFile);
    Open(&engine, &live, &liveGame);
    assert(engine.entry.phase == WT_JOURNAL_APPLYING);
    assert(WtEngine_Settle(&engine) == 0 && engine.entry.phase == WT_JOURNAL_APPLIED);
    WtEngine_Close(&engine);
    Acknowledged(0);
    puts("PASS stock save during a return: cut before it was written, APPLIED and acknowledged without adding it");
}

/* LUNE-902: what the player does with the offer in the stock game. */
static void TrainOffered(unsigned char *general, unsigned char *boxes)
{
    (void)boxes;
    FxTrain(general + FX_PARTY + 8 + OFFERED_SLOT * FX_MON);
}

static void DepositOfferedToPc(unsigned char *general, unsigned char *boxes)
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
    static const FxStockAction actions[] = { TrainOffered, DepositOfferedToPc };
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
        {
            /* The games' REMOVE (the adapter's, without the companion's
             * unmasking) cannot match the masked inventoryBefore: it fails and
             * writes nothing, so only the companion removes this offer, moving
             * the counter. With the true hash it would remove it. */
            WtJournalEntry stored;
            static unsigned char untouched[CP_SAVE_FILE_SIZE];
            uint32_t recorded;
            assert(WtJournal_Decode(live.banks[0], Hash, &stored) == 0 && stored.phase == WT_JOURNAL_ESCROW);
            Bind(&liveGame, &liveImage, &liveFile);
            memcpy(untouched, live.bytes, sizeof(untouched));
            assert(WtGame_Inventory(&liveGame, WT_INVENTORY_REMOVE, &stored, stored.source.slot) == -1);
            assert(memcmp(untouched, live.bytes, sizeof(untouched)) == 0);
            assert(WtEngine_EscrowInventoryBefore(&stored, Hash, stored.inventoryBefore) == 1);
            recorded = stored.globalCounter;
            Bind(&liveGame, &liveImage, &liveFile);
            assert(WtGame_Inventory(&liveGame, WT_INVENTORY_REMOVE, &stored, stored.source.slot) == 0);
            assert(liveImage.globalCounter - recorded >= WT_ABANDON_STEP);
            live = escrow;
        }
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
        /* The fence: a Cancel of a trade nobody holds used the Submit's counter. */
        assert(!server.requests[WT_OP_SUBMIT] && server.requests[WT_OP_CANCEL] == 1 && server.state == WT_TRADE_NONE);
        assert(server.nextCounter == 2);
        memcpy(stock.bytes, live.bytes, sizeof(stock.bytes));
        assert(FxStockCopies(&stock, offered) == 1);
        {
            /* Another copy of the save still holding the escrow (the one cut
             * before play) can never have its Submit accepted now: it is
             * refused at the fenced counter, and that copy restores its
             * Pokémon; the server holds nothing. */
            static World kept;
            kept = live;
            live = escrow;
            Bind(&liveGame, &liveImage, &liveFile);
            Open(&engine, &live, &liveGame);
            assert(CpVisit_Run(&engine, &liveGame, 7, -1, 0, &transport, &visit) == 0);
            WtEngine_Close(&engine);
            assert(visit.phase == WT_JOURNAL_IDLE && server.state == WT_TRADE_NONE);
            assert(server.requests[WT_OP_SUBMIT] == 1);
            memcpy(stock.bytes, live.bytes, sizeof(stock.bytes));
            assert(FxStockCopies(&stock, offered) == 1);
            live = kept;
        }
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
        /* The fence found the counter used: the escrow stays, the save untouched. */
        assert(memcmp(live.bytes, before, sizeof(before)) == 0 && server.state == WT_TRADE_QUEUED);
        Open(&engine, &live, &liveGame);
        assert(engine.entry.phase == WT_JOURNAL_ESCROW && engine.entry.request[5] == WT_OP_CANCEL);
        assert(CpVisit_Run(&engine, &liveGame, 7, -1, 0, &transport, &visit) != 0 && engine.blocked);
        WtEngine_Close(&engine);
        assert(memcmp(live.bytes, before, sizeof(before)) == 0 && server.state == WT_TRADE_QUEUED);
        printf("PASS unsent offer %s in the stock game: a server holding it is never abandoned\n", kActions[action]);
    }
}

/* The only Pokémon left with the offer's identity is a twin: the offer
 * removed, another Pokémon sharing its identity and every field play never
 * changes, differing in experience, in the PC. */
static void TwinOnly(unsigned char *general, unsigned char *boxes)
{
    DepositOfferedToPc(general, boxes);
    FxTrain(FxPcSlot(boxes, 7));
}

/* LUNE-902: an escrow deposited by a game or an older companion (no
 * identity-unique mark) is never abandoned, whatever the save holds: the
 * twin left could be another Pokémon and the offer only in the journal. The
 * visit blocks; the save, the journal and the server are untouched. */
static void LegacyTwin(void)
{
    static World played, marked;
    WtEngine engine;
    WtJournalEntry entry;
    CpVisit visit;
    unsigned bank, submits, cancels;
    scenario = OFFER;
    Setup();
    cutAfter = WT_JOURNAL_ESCROW;
    Open(&engine, &live, &liveGame);
    assert(CpVisit_Run(&engine, &liveGame, 7, OFFERED_SLOT, 0, &transport, &visit) != 0);
    WtEngine_Close(&engine);
    saveCut = 0;
    assert(visit.phase == WT_JOURNAL_ESCROW);
    memcpy(marked.banks, live.banks, sizeof(marked.banks));
    assert(WtJournal_Decode(live.banks[0], Hash, &entry) == 0);
    /* As a game or an older companion records it: inventoryBefore unmasked, no mark. */
    assert(WtEngine_EscrowInventoryBefore(&entry, Hash, entry.inventoryBefore) == 1);
    entry.operation[9] ^= 0x5A; entry.request[8 + 9] ^= 0x5A;
    assert(WtEngine_EscrowInventoryBefore(&entry, Hash, entry.inventoryBefore) == 0);
    for (bank = 0; bank < 2; ++bank) assert(WtJournal_Encode(&entry, Hash, live.banks[bank]) == 0);
    memcpy(stock.bytes, live.bytes, sizeof(stock.bytes));
    assert(FxStockPlay(&stock, 1, 0, TwinOnly));
    memcpy(live.bytes, stock.bytes, sizeof(live.bytes));
    played = live; submits = server.requests[WT_OP_SUBMIT]; cancels = server.requests[WT_OP_CANCEL];
    Bind(&liveGame, &liveImage, &liveFile);
    Open(&engine, &live, &liveGame);
    assert(WtEngine_Settle(&engine) == -1 && engine.blocked);
    WtEngine_Close(&engine);
    Open(&engine, &live, &liveGame);
    assert(CpVisit_Run(&engine, &liveGame, 7, -1, 0, &transport, &visit) != 0 && !visit.canLeave);
    WtEngine_Close(&engine);
    assert(memcmp(&live, &played, sizeof(live)) == 0);
    assert(server.requests[WT_OP_SUBMIT] == submits && server.requests[WT_OP_CANCEL] == cancels);
    /* The companion's own escrow, marked unique at its deposit: that Pokémon
     * can only be the offer, trained and deposited, and it is abandoned. */
    memcpy(marked.bytes, played.bytes, sizeof(marked.bytes));
    live = marked;
    Bind(&liveGame, &liveImage, &liveFile);
    Open(&engine, &live, &liveGame);
    assert(CpVisit_Run(&engine, &liveGame, 7, -1, 0, &transport, &visit) == 0);
    WtEngine_Close(&engine);
    assert(visit.phase == WT_JOURNAL_IDLE && visit.canLeave && server.requests[WT_OP_SUBMIT] == submits);
    assert(memcmp(live.bytes, played.bytes, sizeof(live.bytes)) == 0);
    puts("PASS legacy escrow with only a twin left: blocked, save, journal and server untouched; marked, abandoned");
}

int main(void)
{
    unsigned total[OUTCOMES] = { 0, 0, 0, 0, 0 }, i, letGo = 0;
    for (scenario = OFFER; scenario < SCENARIOS; ++scenario) {
        WtEngine engine;
        CpVisit visit;
        memset(tally, 0, sizeof(tally)); cases = writes = letGoCases = 0;
        Setup();
        sweeping = 1;
        Open(&engine, &live, &liveGame);
        assert(CpVisit_Run(&engine, &liveGame, 7, scenario == CANCEL_RETURN ? -1 : OFFERED_SLOT,
            scenario == CANCEL_RETURN ? WT_OP_CANCEL : 0, &transport, &visit) == 0);
        assert(visit.phase == (scenario == OFFER ? WT_JOURNAL_QUEUED : WT_JOURNAL_IDLE));
        WtEngine_Close(&engine);
        sweeping = 0;
        printf("%s: %u cuts (each write, and each save write torn), %u stock-save cases, %u more with the offer "
            "released or traded away while a return was in flight: recovered=%u lost-in-window=%u stuck=%u "
            "duplicate=%u lost=%u\n", kNames[scenario], writes, cases, letGoCases,
            tally[RECOVERED], tally[WINDOW], tally[STUCK], tally[DUPLICATE], tally[LOST]);
        assert(writes > 0 && cases == 2 * writes);
        for (i = 0; i < OUTCOMES; ++i) total[i] += tally[i];
        letGo += letGoCases;
    }
    assert(total[STUCK] == 0 && total[DUPLICATE] == 0 && total[LOST] == 0);
    RedirectThroughJournal();
    UnsentOffer();
    LegacyTwin();
    assert(letGo > 0 && total[WINDOW] > 0);
    printf("PASS stock save before resume: %u cases recovered (%u with the offer released or traded away), 0 stuck, "
        "0 duplicates, %u lost only in the games' window\n", total[RECOVERED], letGo, total[WINDOW]);
    return 0;
}
