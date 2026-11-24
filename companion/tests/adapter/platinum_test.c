/* The companion's Platinum adapter against synthetic saves: every inventory
 * step and refusal of client/src/wonder_trade/platinum/game_save.c. */
#include "save_fixture.h"

#include <stdio.h>
#include <stdlib.h>

static FxFile file;
static CpSaveImage image, expected;
static CpFile cpFile = { &file, FxRead, FxWrite, FxSync, FxSize };
static WtGame game;
static unsigned char fileBefore[CP_SAVE_FILE_SIZE];

/* The first bytes of an empty slot's encrypted blocks: zeros XORed with the
 * LCG from seed 0 (computed outside this code base). */
static const unsigned char kEmptyBlocks[8] = { 0, 0, 126, 233, 113, 82, 176, 49 };

static int FakeHoldable(void *arg, unsigned item)
{
    (void)arg;
    return item != 300; /* a key item */
}

static void Setup(const FxSpec *specs, unsigned count)
{
    FxBuild(&file, specs, count);
    assert(CpSaveImage_Load(&image, &cpFile, CP_SAVE_MAGIC_INTERNATIONAL) == CP_SAVE_OK);
    assert(image.currentHalf[0] == 1 && image.currentHalf[1] == 1);
    memset(&game, 0, sizeof(game));
    game.image = &image; game.file = &cpFile; game.holdable = FakeHoldable;
}

static void Reload(void)
{
    assert(CpSaveImage_Load(&image, &cpFile, CP_SAVE_MAGIC_INTERNATIONAL) == CP_SAVE_OK);
}

static void Defaults(FxSpec *specs, unsigned count)
{
    unsigned i;
    for (i = 0; i < count; ++i) specs[i] = FxDefault(i);
}

/* Save the image as the game would between visits: a change outside the party. */
static void GameSaves(unsigned offset)
{
    unsigned char digest[32];
    CpSaveImage_Block(&image, 0)[offset] ^= 0x5a;
    assert(CpSaveImage_Hash(&image, digest) == CP_SAVE_OK);
    assert(CpSaveImage_CommitBoth(&image, &cpFile, digest) == CP_SAVE_OK);
}

static int Step(enum WtInventoryStep step, WtJournalEntry *entry, unsigned slot)
{
    return WtGame_Inventory(&game, step, entry, slot);
}

static void Unchanged(const CpSaveImage *before)
{
    assert(memcmp(image.bytes, before->bytes, sizeof(image.bytes)) == 0);
    assert(memcmp(file.bytes, fileBefore, sizeof(file.bytes)) == 0);
}

/* PREPARE_REMOVE refuses the slot, and neither the image nor the file changes. */
static void Refused(const FxSpec *specs, unsigned count, unsigned slot, const char *why)
{
    WtJournalEntry entry;
    memset(&entry, 0, sizeof(entry));
    if (specs) Setup(specs, count);
    expected = image; memcpy(fileBefore, file.bytes, sizeof(fileBefore));
    assert(!CpAdapter_Eligible(&image, slot) || why[0] == '!');
    assert(Step(WT_INVENTORY_PREPARE_REMOVE, &entry, slot) == -1);
    Unchanged(&expected);
    printf("PASS refuse: %s\n", why[0] == '!' ? why + 1 : why);
}

static void Eligibility(void)
{
    FxSpec specs[6];
    WtJournalEntry entry;
    Defaults(specs, 3); Setup(specs, 3);
    assert(CpAdapter_Eligible(&image, 0) && CpAdapter_Eligible(&image, 2) && !CpAdapter_Eligible(&image, 3));
    assert(Step(WT_INVENTORY_PREPARE_REMOVE, &entry, 0) == 0);

    Defaults(specs, 3); specs[0].egg = 1; Refused(specs, 3, 0, "an Egg");
    Defaults(specs, 3); specs[0].capsule = 3; Refused(specs, 3, 0, "a ball capsule");
    Defaults(specs, 3); specs[0].item = 137; Refused(specs, 3, 0, "Grass Mail");
    Defaults(specs, 3); specs[0].item = 148; Refused(specs, 3, 0, "Brick Mail");
    Defaults(specs, 3); specs[0].item = 136; Setup(specs, 3); assert(Step(WT_INVENTORY_PREPARE_REMOVE, &entry, 0) == 0);
    Defaults(specs, 3); specs[0].item = 149; Setup(specs, 3); assert(Step(WT_INVENTORY_PREPARE_REMOVE, &entry, 0) == 0);
    Defaults(specs, 3); specs[0].species = 494; Refused(specs, 3, 0, "a species past Arceus");
    Defaults(specs, 3); specs[0].species = 493; Setup(specs, 3); assert(Step(WT_INVENTORY_PREPARE_REMOVE, &entry, 0) == 0);
    Defaults(specs, 3); specs[1].hp = 0; specs[2].egg = 1; Refused(specs, 3, 0, "the last usable party member");
    Defaults(specs, 3); specs[1].hp = 0; specs[2].hp = 0; Refused(specs, 3, 0, "the last Pokémon with HP");
    /* A fainted Pokémon may go while another can still battle. */
    Defaults(specs, 3); specs[0].hp = 0; specs[1].hp = 0; Setup(specs, 3);
    assert(Step(WT_INVENTORY_PREPARE_REMOVE, &entry, 0) == 0);
    Defaults(specs, 1); Refused(specs, 1, 0, "a party of one");
    Defaults(specs, 3); Refused(specs, 3, 3, "an empty slot");
    Defaults(specs, 3); Setup(specs, 3);
    FxImageParty(&image, 0)[40] ^= 1; /* the checksum fails: the game reads an Egg */
    Refused(NULL, 0, 0, "a Pokémon whose checksum fails");
    Defaults(specs, 3); Setup(specs, 3);
    FxImageParty(&image, 0)[4] = 4; /* stored checksumFailed */
    Refused(NULL, 0, 0, "a Pokémon marked as failed");
    Defaults(specs, 3); Setup(specs, 3);
    FxImageParty(&image, 1)[40] ^= 1;
    Refused(NULL, 0, 0, "!a party whose other Pokémon cannot be read");
    Defaults(specs, 3); Setup(specs, 3);
    FxPut32(CpSaveImage_Block(&image, 0) + FX_PARTY, 5);
    Refused(NULL, 0, 0, "a party capacity the game never writes");
    Defaults(specs, 3); Setup(specs, 3);
    FxEmpty(FxImageBox(&image, 4, 4), FX_BOX_MON); FxImageBox(&image, 4, 4)[20] ^= 1;
    Refused(NULL, 0, 0, "!an empty PC slot the game's read would mark");
}

static void Duplicates(void)
{
    FxSpec specs[6];
    unsigned char canonical[136];
    Defaults(specs, 4);
    Setup(specs, 4);
    assert(CpAdapter_Unique(&image, 1));
    FxCanonical(&specs[1], canonical);
    assert(WtPokemon_Encode(canonical, FxImageBox(&image, 17, 29)) == 0);
    expected = image;
    assert(!CpAdapter_Unique(&image, 1) && CpAdapter_Unique(&image, 0));
    assert(memcmp(image.bytes, expected.bytes, sizeof(image.bytes)) == 0);
    Refused(NULL, 0, 1, "!a copy in the last PC slot");
    specs[3] = specs[0];
    Setup(specs, 4);
    assert(!CpAdapter_Unique(&image, 0));
    Refused(NULL, 0, 0, "!a copy elsewhere in the party");
}

/* PREPARE_REMOVE records what game_save.c records, and changes nothing. */
static void Prepare(void)
{
    FxSpec specs[6];
    WtJournalEntry entry;
    unsigned char digest[32], canonical[136], *party;
    unsigned i;
    Defaults(specs, 6); Setup(specs, 6);
    expected = image; memcpy(fileBefore, file.bytes, sizeof(fileBefore));
    memset(&entry, 0xee, sizeof(entry));
    assert(Step(WT_INVENTORY_PREPARE_REMOVE, &entry, 2) == 0);
    Unchanged(&expected);
    assert(entry.source.kind == 1 && entry.source.box == 0 && entry.source.slot == 2 && entry.source.length == 236);
    assert(memcmp(entry.original, FxImageParty(&image, 2), 236) == 0);
    FxCanonical(&specs[2], canonical);
    assert(memcmp(entry.outgoing, canonical, 136) == 0);
    FxSha(entry.original, 236, digest);
    assert(memcmp(entry.originalDigest, digest, 32) == 0);
    assert(CpSaveImage_Hash(&image, digest) == CP_SAVE_OK && memcmp(entry.inventoryBefore, digest, 32) == 0);
    /* The hash after: the party shifted down by this test's own hand. */
    party = CpSaveImage_Block(&expected, 0) + FX_PARTY;
    for (i = 2; i < 5; ++i) memcpy(party + 8 + i * FX_MON, party + 8 + (i + 1) * FX_MON, FX_MON);
    FxEmpty(party + 8 + 5 * FX_MON, FX_MON); FxPut32(party + 4, 5);
    assert(CpSaveImage_Hash(&expected, digest) == CP_SAVE_OK && memcmp(entry.inventoryAfter, digest, 32) == 0);
    assert(entry.globalCounter == 11 && entry.generalCounter == 21 && entry.boxCounter == 31);
    puts("PASS prepare: locator, original, outgoing, digests, hashes and counters, image untouched");
}

static void ShiftDownAndRestore(void)
{
    FxSpec specs[6];
    WtJournalEntry entry;
    unsigned char digest[32], party[8 + 6 * FX_MON];
    unsigned slot, i, writes;
    for (slot = 0; slot < 6; ++slot) {
        Defaults(specs, 6); Setup(specs, 6);
        memcpy(party, CpSaveImage_Block(&image, 0) + FX_PARTY, sizeof(party));
        memset(&entry, 0, sizeof(entry));
        assert(Step(WT_INVENTORY_PREPARE_REMOVE, &entry, slot) == 0);
        /* The Pokémon is still there: nothing proves it gone. */
        assert(Step(WT_INVENTORY_VERIFY_REMOVED, &entry, slot) == -1);
        assert(Step(WT_INVENTORY_REMOVE, &entry, slot) == 0);
        assert(FxImageCount(&image) == 5);
        for (i = 0; i < 5; ++i)
            assert(memcmp(FxImageParty(&image, i), party + 8 + (i < slot ? i : i + 1) * FX_MON, FX_MON) == 0);
        {
            unsigned char empty[FX_MON];
            FxEmpty(empty, FX_MON);
            assert(memcmp(FxImageParty(&image, 5), empty, FX_MON) == 0);
            assert(memcmp(FxImageParty(&image, 5) + 8, kEmptyBlocks, 8) == 0);
            assert(memcmp(FxImageParty(&image, 5) + 136, kEmptyBlocks, 8) == 0);
        }
        /* Both copies hold the removal, written twice the way the game saves. */
        assert(image.globalCounter == 13 && image.blockCounters[0] == 23 && image.blockCounters[1] == 33);
        assert(entry.globalCounter == 13 && entry.generalCounter == 23 && entry.boxCounter == 33);
        assert(CpSaveImage_Verify(&image, &cpFile, entry.inventoryAfter) == CP_SAVE_OK);
        Reload();
        assert(CpSaveImage_Hash(&image, digest) == CP_SAVE_OK && memcmp(digest, entry.inventoryAfter, 32) == 0);
        assert(Step(WT_INVENTORY_VERIFY_REMOVED, &entry, slot) == 0);
        /* A replayed REMOVE finds the removal done and writes nothing. */
        writes = file.writes;
        assert(Step(WT_INVENTORY_REMOVE, &entry, slot) == 0 && file.writes == writes);
        entry.phase = WT_JOURNAL_QUEUED;
        assert(WtGame_CanLeave(&game, &entry));
        assert(Step(WT_INVENTORY_PREPARE_RESTORE, &entry, slot) == 0);
        assert(Step(WT_INVENTORY_RESTORE, &entry, slot) == 0);
        assert(memcmp(CpSaveImage_Block(&image, 0) + FX_PARTY, party, sizeof(party)) == 0);
        Reload();
        assert(CpSaveImage_Hash(&image, digest) == CP_SAVE_OK && memcmp(digest, entry.inventoryBefore, 32) == 0);
        assert(memcmp(CpSaveImage_Block(&image, 0) + FX_PARTY, party, sizeof(party)) == 0);
        assert(entry.globalCounter == 15 && image.globalCounter == 15);
        /* Restored before; a replay finds it done. */
        assert(Step(WT_INVENTORY_PREPARE_RESTORE, &entry, slot) == 0);
        writes = file.writes;
        assert(Step(WT_INVENTORY_RESTORE, &entry, slot) == 0 && file.writes == writes);
        assert(!WtGame_CanLeave(&game, &entry));
    }
    puts("PASS remove: shift-down from each of six slots, encrypted empty slot, both copies, restore in place");
}

static void Refusals(void)
{
    FxSpec specs[6];
    WtJournalEntry entry, bad;
    unsigned char digest[32];
    unsigned writes;
    Defaults(specs, 4); Setup(specs, 4);
    assert(Step(WT_INVENTORY_PREPARE_REMOVE, &entry, 1) == 0);
    /* The save changed after the escrow was recorded. */
    CpSaveImage_Block(&image, 0)[3000] ^= 1;
    expected = image; memcpy(fileBefore, file.bytes, sizeof(fileBefore));
    assert(Step(WT_INVENTORY_REMOVE, &entry, 1) == -1);
    Unchanged(&expected);
    assert(Step(WT_INVENTORY_PREPARE_RESTORE, &entry, 1) == 1);
    CpSaveImage_Block(&image, 0)[3000] ^= 1;
    /* The wrong slot, a broken binding and a locator the game would refuse. */
    expected = image;
    assert(Step(WT_INVENTORY_REMOVE, &entry, 2) == -1 && Step(WT_INVENTORY_REMOVE, &entry, 6) == -1);
    bad = entry; bad.outgoing[30] ^= 1;
    assert(Step(WT_INVENTORY_REMOVE, &bad, 1) == -1 && Step(WT_INVENTORY_VERIFY_REMOVED, &bad, 1) == -1);
    assert(Step(WT_INVENTORY_RESTORE, &bad, 1) == -1 && Step(WT_INVENTORY_PREPARE_RESTORE, &bad, 1) == -1);
    bad = entry; bad.source.kind = 2;
    assert(Step(WT_INVENTORY_REMOVE, &bad, 1) == -1);
    bad = entry; bad.source.length = 136;
    assert(Step(WT_INVENTORY_REMOVE, &bad, 1) == -1);
    Unchanged(&expected);
    /* RESTORE refuses a save that is not the escrow's. */
    assert(Step(WT_INVENTORY_RESTORE, &entry, 1) == 0); /* before: only checkpoints */
    assert(Step(WT_INVENTORY_REMOVE, &entry, 1) == 0);
    bad = entry; bad.inventoryAfter[0] ^= 1;
    assert(Step(WT_INVENTORY_RESTORE, &bad, 1) == -1);
    puts("PASS refuse: changed save, wrong slot, broken binding, foreign locator");

    /* VERIFY_REMOVED: counters behind the journal, and the Pokémon back in the PC. */
    bad = entry; bad.globalCounter += 1;
    assert(Step(WT_INVENTORY_VERIFY_REMOVED, &bad, 1) == -1);
    bad = entry; bad.generalCounter += 1;
    assert(Step(WT_INVENTORY_VERIFY_REMOVED, &bad, 1) == -1);
    bad = entry; bad.boxCounter += 1;
    assert(Step(WT_INVENTORY_VERIFY_REMOVED, &bad, 1) == -1);
    bad.phase = WT_JOURNAL_QUEUED;
    assert(!WtGame_CanLeave(&game, &bad));
    /* Counters ahead of the journal (the game saved since) are fine. */
    GameSaves(4000);
    assert(Step(WT_INVENTORY_VERIFY_REMOVED, &entry, 1) == 0);
    assert(WtPokemon_Encode(entry.outgoing, FxImageBox(&image, 0, 0)) == 0);
    assert(CpSaveImage_Hash(&image, digest) == CP_SAVE_OK && CpSaveImage_CommitBoth(&image, &cpFile, digest) == CP_SAVE_OK);
    assert(Step(WT_INVENTORY_VERIFY_REMOVED, &entry, 1) == -1);
    puts("PASS refuse: verify behind each journal counter and a copy back in the PC");

    /* Checkpoint writes only when the two copies differ. */
    Defaults(specs, 2); Setup(specs, 2);
    writes = file.writes;
    assert(WtGame_Checkpoint(&game) == 0 && file.writes > writes);
    writes = file.writes;
    assert(WtGame_Checkpoint(&game) == 0 && file.writes == writes);
    assert(CpSaveImage_Hash(&image, digest) == CP_SAVE_OK && CpSaveImage_Verify(&image, &cpFile, digest) == CP_SAVE_OK);
    /* A failed write unloads the image: nothing more runs until it is loaded again. */
    Defaults(specs, 3); Setup(specs, 3);
    assert(Step(WT_INVENTORY_PREPARE_REMOVE, &entry, 0) == 0);
    file.failWrite = file.writes + 2;
    assert(Step(WT_INVENTORY_REMOVE, &entry, 0) == -1 && !image.loaded);
    assert(Step(WT_INVENTORY_VERIFY_REMOVED, &entry, 0) == -1 && !WtGame_CanLeave(&game, &entry));
    puts("PASS checkpoint: writes only differing copies; a failed write stops every step");
}

/* A cancelled or rejected offer after the game saved: it returns like a
 * delivery, to the end of the party or the first empty PC slot. */
static void ReturnAfterChange(void)
{
    FxSpec specs[6];
    WtJournalEntry entry, match;
    unsigned char digest[32], empty[FX_MON];
    unsigned box, slot;
    FxEmpty(empty, FX_MON);
    Defaults(specs, 3); Setup(specs, 3);
    assert(Step(WT_INVENTORY_PREPARE_REMOVE, &entry, 0) == 0 && Step(WT_INVENTORY_REMOVE, &entry, 0) == 0);
    GameSaves(5000);
    assert(Step(WT_INVENTORY_PREPARE_RESTORE, &entry, 0) == 1);
    entry.deliveryKind = WT_DELIVERY_RETURN; memcpy(entry.incoming, entry.outgoing, 136);
    match = entry; match.deliveryKind = WT_DELIVERY_MATCH;
    assert(Step(WT_INVENTORY_PREPARE_RECEIVE, &match, 0) == -1); /* LUNE-873 */
    expected = image; memcpy(fileBefore, file.bytes, sizeof(fileBefore));
    assert(Step(WT_INVENTORY_PREPARE_RECEIVE, &entry, 0) == 0);
    Unchanged(&expected);
    assert(entry.destination.kind == 1 && entry.destination.box == 0 && entry.destination.slot == 2 && entry.destination.length == 236);
    assert(memcmp(entry.destinationBefore, empty, FX_MON) == 0 && memcmp(entry.destinationAfter, entry.original, FX_MON) == 0);
    FxSha(entry.outgoing, 136, digest);
    assert(memcmp(entry.appliedDigest, digest, 32) == 0);
    assert(CpSaveImage_Hash(&image, digest) == CP_SAVE_OK && memcmp(entry.inventoryBefore, digest, 32) == 0);
    assert(Step(WT_INVENTORY_RECEIVE, &entry, 2) == 0);
    assert(FxImageCount(&image) == 3 && memcmp(FxImageParty(&image, 2), entry.original, FX_MON) == 0);
    Reload();
    assert(Step(WT_INVENTORY_VERIFY_RECEIVED, &entry, 2) == 0);
    entry.phase = WT_JOURNAL_APPLIED;
    assert(WtGame_CanLeave(&game, &entry));
    assert(Step(WT_INVENTORY_RECEIVE, &entry, 2) == 0); /* replay */
    puts("PASS return: a changed save gets its offer back at the end of the party");

    /* A full party: the first empty PC slot, after an occupied one. */
    Defaults(specs, 6); Setup(specs, 6);
    assert(Step(WT_INVENTORY_PREPARE_REMOVE, &entry, 4) == 0 && Step(WT_INVENTORY_REMOVE, &entry, 4) == 0);
    {
        FxSpec extra = FxDefault(9);
        unsigned char canonical[136];
        FxPartyMon(&extra, FxImageParty(&image, 5));
        FxPut32(CpSaveImage_Block(&image, 0) + FX_PARTY + 4, 6);
        extra = FxDefault(10); FxCanonical(&extra, canonical);
        assert(WtPokemon_Encode(canonical, FxImageBox(&image, 0, 0)) == 0);
        GameSaves(6000);
    }
    entry.deliveryKind = WT_DELIVERY_RETURN; memcpy(entry.incoming, entry.outgoing, 136);
    assert(Step(WT_INVENTORY_PREPARE_RESTORE, &entry, 4) == 1);
    assert(Step(WT_INVENTORY_PREPARE_RECEIVE, &entry, 0) == 0);
    assert(entry.destination.kind == 2 && entry.destination.box == 0 && entry.destination.slot == 1 && entry.destination.length == 136);
    assert(Step(WT_INVENTORY_RECEIVE, &entry, 1) == 0);
    assert(memcmp(FxImageBox(&image, 0, 1), entry.original, 136) == 0);
    Reload();
    assert(Step(WT_INVENTORY_VERIFY_RECEIVED, &entry, 1) == 0);
    puts("PASS return: a full party gets its offer back in the first empty PC slot");

    /* No room anywhere: 1, and nothing changes. */
    for (box = 0; box < 18; ++box) for (slot = 0; slot < 30; ++slot) {
        FxSpec filler = FxDefault(100 + box * 30 + slot);
        unsigned char canonical[136];
        FxCanonical(&filler, canonical);
        assert(WtPokemon_Encode(canonical, FxImageBox(&image, box, slot)) == 0);
    }
    GameSaves(7000);
    entry.deliveryKind = WT_DELIVERY_RETURN;
    expected = image; memcpy(fileBefore, file.bytes, sizeof(fileBefore));
    assert(Step(WT_INVENTORY_PREPARE_RECEIVE, &entry, 0) == 1);
    Unchanged(&expected);
    puts("PASS return: no room keeps it in the journal");
}

static void CanHold(void)
{
    static const struct { unsigned species, form, item, ball; int fits; } cases[] = {
        { 25, 0, 0, 4, 1 }, { 0, 0, 0, 4, 0 }, { 493, 17, 0, 4, 1 }, { 493, 18, 0, 4, 0 },
        { 494, 0, 0, 4, 0 }, { 201, 27, 0, 4, 1 }, { 201, 28, 0, 4, 0 }, { 25, 1, 0, 4, 0 },
        { 386, 3, 0, 4, 1 }, { 386, 4, 0, 4, 0 }, { 412, 2, 0, 4, 1 }, { 413, 3, 0, 4, 0 },
        { 422, 1, 0, 4, 1 }, { 423, 2, 0, 4, 0 }, { 479, 5, 0, 4, 1 }, { 479, 6, 0, 4, 0 },
        { 487, 1, 0, 4, 1 }, { 492, 1, 0, 4, 1 }, { 492, 2, 0, 4, 0 }, { 351, 3, 0, 4, 1 },
        { 421, 1, 0, 4, 1 }, { 421, 2, 0, 4, 0 },
        { 25, 0, 467, 4, 1 }, { 25, 0, 468, 4, 0 }, { 25, 0, 300, 4, 0 }, { 25, 0, 112, 4, 1 },
        { 25, 0, 0, 0, 0 }, { 25, 0, 0, 16, 1 }, { 25, 0, 0, 17, 0 }
    };
    unsigned i;
    unsigned char canonical[136];
    FxSpec spec = FxDefault(0);
    Setup(&spec, 1);
    assert(CpAdapter_Forms(201) == 28 && CpAdapter_Forms(493) == 18 && CpAdapter_Forms(172) == 1);
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        spec.species = cases[i].species; spec.form = cases[i].form; spec.item = cases[i].item; spec.ball = cases[i].ball;
        FxCanonical(&spec, canonical);
        assert(WtGame_CanHold(&game, canonical) == cases[i].fits);
    }
    spec = FxDefault(0); FxCanonical(&spec, canonical); canonical[50] ^= 1;
    assert(!WtGame_CanHold(&game, canonical)); /* checksum */
    spec.item = 1; FxCanonical(&spec, canonical);
    game.holdable = NULL;
    assert(!WtGame_CanHold(&game, canonical));
    spec.item = 0; FxCanonical(&spec, canonical);
    assert(WtGame_CanHold(&game, canonical));
    puts("PASS can hold: species, forms, items, hold flags and balls");
}

int main(void)
{
    Eligibility();
    Duplicates();
    Prepare();
    ShiftDownAndRestore();
    Refusals();
    ReturnAfterChange();
    CanHold();
    puts("PASS platinum adapter");
    return 0;
}
