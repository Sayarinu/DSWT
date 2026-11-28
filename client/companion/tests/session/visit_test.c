/* End to end on a synthetic save: the companion enrolls, offers a party
 * Pokémon, checks its status and cancels it, through the engine, the visit
 * driver and the Platinum adapter, against an in-memory fake server. A
 * rejected offer is restored to its own slot. */
#include "../adapter/save_fixture.h"
#include "fake_server.h"
#include "journal_fixture.h"

#include <stdio.h>

#include "session/visit.h"

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

static const CpTransport transport = { NULL, FsPost, NULL, NULL, NULL };

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
    /* Enroll, reconcile, offer slot 1, use the durable submit snapshot. */
    Open(&engine);
    assert(CpVisit_Run(&engine, &game, 7, 1, 0, &transport, &visit) == 0);
    assert(visit.phase == WT_JOURNAL_QUEUED && visit.canLeave && !visit.completed);
    assert(server.requests[WT_OP_ENROLL] == 1 && server.requests[WT_OP_SUBMIT] == 1 && server.requests[WT_OP_STATUS] == 0 && visit.serverTradeState == WT_TRADE_QUEUED);
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

static int MatchBeforeSubmitReply(void *arg, const unsigned char *request, size_t size,
    unsigned char reply[WT_WIRE_REPLY_MAX], size_t *replySize)
{
    int result = FsPost(arg, request, size, reply, replySize);
    if (!result && request[WT_WIRE_REQUEST_OPERATION] == WT_OP_SUBMIT) {
        FxSpec partner = FxDefault(5);
        unsigned char canonical[136];
        FxCanonical(&partner, canonical);
        FsMatch(canonical);
        FsPut(reply + WT_WIRE_REPLY_HIGHWATER, server.eventCount, 8);
    }
    return result;
}

static void AlreadyMatchedOfferChecksStatus(void)
{
    CpTransport immediate = transport;
    WtEngine engine;
    CpVisit visit;
    immediate.post = MatchBeforeSubmitReply;
    Fresh(4);
    Open(&engine);
    assert(CpVisit_Run(&engine, &game, 7, 1, 0, &immediate, &visit) == 0);
    assert(visit.canLeave && visit.serverTradeState == WT_TRADE_MATCHED);
    assert(server.requests[WT_OP_SUBMIT] == 1 && server.requests[WT_OP_STATUS] == 1);
    WtEngine_Close(&engine);
    puts("PASS a newer event on the submit reply still shows the match immediately");
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
    AlreadyMatchedOfferChecksStatus();
    RejectedOfferRestoresInPlace();
    Failures();
    puts("PASS visit driver");
    return 0;
}
