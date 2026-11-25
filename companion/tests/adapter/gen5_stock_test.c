/* LUNE-881: LUNE-899's stock-game recovery on Black/White (FX5_GAME 0) and
 * Black 2/White 2 (1) saves. The companion stops in the middle of a step, the
 * player loads and saves in the stock game (modelled independently,
 * adapter/gen5_stock.h), and the step resumes from the journal. Every step is
 * cut before each of its writes and after the last; the offered Pokémon must
 * end up in exactly one place: the save or the journal. */
#include "gen5_stock.h"

#include <stdio.h>

static Fx5File file;
static CpSaveImage image;
static CpFile cpFile = { &file, Fx5Read, Fx5Write, Fx5Sync, Fx5Size };
static WtGame game;
static unsigned char offered[136], ram[0x26000];
static unsigned cases;

enum { OFFERED_SLOT = 1 };

typedef void (*Action)(unsigned char *ram);

/* The stock game loads the card, the player plays (`action`), and it saves
 * `saves` times. */
static void StockPlay(unsigned saves, Action action)
{
    unsigned i;
    RetailLoad5(file.bytes, ram);
    if (action) action(ram);
    for (i = 0; i < saves; ++i) StockSave5(file.bytes, ram);
}

/* Copies of the offered Pokémon the stock game loads: party, PC, Battle Box. */
static unsigned Copies(void)
{
    unsigned char canonical[136];
    const unsigned char *party;
    unsigned count, mon, copies = 0;
    RetailLoad5(file.bytes, ram);
    party = ram + Rl5Offset(FX5_PARTY);
    count = Fx5R32(party + 4);
    for (mon = 0; mon < count + 720 + 6; ++mon) {
        const unsigned char *slot = mon < count ? party + 8 + mon * FX5_MON
            : mon < count + 720 ? ram + Rl5Offset(1 + (mon - count) / 30) + (mon - count) % 30 * FX5_BOX_MON
            : ram + Rl5Offset(FX5_BATTLE_BOX) + (mon - count - 720) * FX5_BOX_MON;
        assert(WtPokemon_Decode(slot, canonical) == 0);
        copies += memcmp(canonical, offered, 136) == 0;
    }
    return copies;
}

static void Load(void)
{
    assert(Fx5Load(&image, &cpFile) == CP_SAVE_OK);
}

static void Setup(unsigned count)
{
    Fx5Spec specs[6];
    unsigned i;
    for (i = 0; i < count; ++i) specs[i] = Fx5Default(i);
    Fx5Build(&file, specs, count, 40);
    Fx5Canonical(&specs[OFFERED_SLOT], offered);
    Load();
    memset(&game, 0, sizeof(game));
    game.image = &image; game.file = &cpFile;
}

static int Step(enum WtInventoryStep step, WtJournalEntry *entry)
{
    unsigned slot = step == WT_INVENTORY_RECEIVE ? entry->destination.slot : entry->source.slot;
    return WtGame_Inventory(&game, step, entry, slot);
}

/* The patched game played: a change outside the party, saved to both copies. */
static void GameSaves(unsigned block)
{
    unsigned char digest[32];
    CpSaveImage_Block(&image, block)[5] ^= 0x5A;
    assert(CpSaveImage_Hash(&image, digest) == CP_SAVE_OK);
    assert(CpSaveImage_CommitBoth(&image, &cpFile, digest) == CP_SAVE_OK);
}

enum State { ESCROW, RESTORING, APPLYING_RETURN };
static const char *const kStates[] = { "escrow (REMOVE)", "restoring (RESTORE)", "applying a return (RECEIVE)" };
static const enum WtInventoryStep kCutStep[] = { WT_INVENTORY_REMOVE, WT_INVENTORY_RESTORE, WT_INVENTORY_RECEIVE };

static void Arrange(enum State state, WtJournalEntry *journal)
{
    Setup(4);
    memset(journal, 0, sizeof(*journal));
    journal->source.slot = OFFERED_SLOT;
    assert(Step(WT_INVENTORY_PREPARE_REMOVE, journal) == 0);
    journal->phase = WT_JOURNAL_ESCROW;
    if (state == ESCROW) return;
    assert(Step(WT_INVENTORY_REMOVE, journal) == 0 && Step(WT_INVENTORY_VERIFY_REMOVED, journal) == 0);
    journal->phase = WT_JOURNAL_RESTORING;
    if (state == RESTORING) {
        assert(Step(WT_INVENTORY_PREPARE_RESTORE, journal) == 0);
        return;
    }
    GameSaves(40);
    assert(Step(WT_INVENTORY_PREPARE_RESTORE, journal) == 1);
    journal->deliveryKind = WT_DELIVERY_RETURN;
    memcpy(journal->incoming, journal->outgoing, 136);
    assert(Step(WT_INVENTORY_PREPARE_RECEIVE, journal) == 0);
}

static void Restore(WtJournalEntry *entry)
{
    int prepared;
    if (entry->deliveryKind != WT_DELIVERY_RETURN) {
        prepared = Step(WT_INVENTORY_PREPARE_RESTORE, entry);
        if (prepared == 0) { assert(Step(WT_INVENTORY_RESTORE, entry) == 0); return; }
        assert(prepared == 1);
        entry->deliveryKind = WT_DELIVERY_RETURN;
        memcpy(entry->incoming, entry->outgoing, 136);
        assert(Step(WT_INVENTORY_PREPARE_RECEIVE, entry) == 0);
    }
    assert(Step(WT_INVENTORY_RECEIVE, entry) == 0 && Step(WT_INVENTORY_VERIFY_RECEIVED, entry) == 0);
}

static void Resume(enum State state, WtJournalEntry *entry)
{
    if (state == ESCROW) {
        assert(Step(WT_INVENTORY_REMOVE, entry) == 0 && Step(WT_INVENTORY_VERIFY_REMOVED, entry) == 0);
        entry->phase = WT_JOURNAL_SUBMIT_READY;
        assert(WtGame_CanLeave(&game, entry));
        assert(Copies() == 0);
    } else {
        Restore(entry);
        assert(Copies() == 1);
    }
}

static void Sweep(enum State state)
{
    static unsigned char resumed[CP_SAVE_FILE_SIZE];
    WtJournalEntry journal, entry;
    unsigned writes, cut, saves;
    Arrange(state, &journal);
    entry = journal; writes = file.writes;
    assert(Step(kCutStep[state], &entry) == 0);
    writes = file.writes - writes;
    /* Two passes: the party, the CRC table, the footer and its last byte. */
    assert(writes == 8);
    for (cut = 1; cut <= writes + 1; ++cut) for (saves = 1; saves <= 2; ++saves) {
        Arrange(state, &journal);
        entry = journal;
        file.failWrite = file.writes + cut;
        assert(Step(kCutStep[state], &entry) == (cut <= writes ? -1 : 0));
        file.failWrite = 0;
        StockPlay(saves, NULL);
        Load();
        entry = journal;
        Resume(state, &entry);
        memcpy(resumed, file.bytes, sizeof(resumed));
        Load();
        entry = journal;
        Resume(state, &entry);
        assert(memcmp(resumed, file.bytes, sizeof(resumed)) == 0);
        if (state == ESCROW) {
            entry.phase = WT_JOURNAL_RESTORING;
            Restore(&entry);
            assert(Copies() == 1);
        }
        ++cases;
    }
    printf("PASS %s stock save after a cut in %s: %u cuts x 2 stock plays, one copy every time\n", FX5_NAME,
           kStates[state], writes + 1);
}

static unsigned char *PartySlot(unsigned char *loaded, unsigned slot)
{
    return loaded + Rl5Offset(FX5_PARTY) + 8 + slot * FX5_MON;
}

static void SwapFirstTwo(unsigned char *loaded)
{
    unsigned char held[FX5_MON];
    memcpy(held, PartySlot(loaded, 0), FX5_MON);
    memcpy(PartySlot(loaded, 0), PartySlot(loaded, 1), FX5_MON);
    memcpy(PartySlot(loaded, 1), held, FX5_MON);
}

static void MoveOffered(unsigned char *loaded, unsigned char *to)
{
    unsigned char *party = loaded + Rl5Offset(FX5_PARTY);
    unsigned count = Fx5R32(party + 4), i;
    memcpy(to, PartySlot(loaded, OFFERED_SLOT), FX5_BOX_MON);
    for (i = OFFERED_SLOT; i + 1 < count; ++i) memcpy(PartySlot(loaded, i), PartySlot(loaded, i + 1), FX5_MON);
    Fx5Empty(PartySlot(loaded, count - 1), FX5_MON);
    Fx5W32(party + 4, count - 1);
}

static void DepositOffered(unsigned char *loaded) { MoveOffered(loaded, loaded + Rl5Offset(3) + 5 * FX5_BOX_MON); }
static void BattleBoxOffered(unsigned char *loaded) { MoveOffered(loaded, loaded + Rl5Offset(FX5_BATTLE_BOX) + 2 * FX5_BOX_MON); }

static void TrainOffered(unsigned char *loaded)
{
    unsigned char canonical[136], *slot = PartySlot(loaded, OFFERED_SLOT);
    unsigned sum = 0, i;
    assert(WtPokemon_Decode(slot, canonical) == 0);
    Fx5W32(canonical + 0x10, Fx5R32(canonical + 0x10) + 100);
    for (i = 8; i < 136; i += 2) sum += Fx5R16(canonical + i);
    Fx5W16(canonical + 6, sum & 0xFFFF);
    assert(WtPokemon_Encode(canonical, slot) == 0);
}

static void Impostor(unsigned char *loaded)
{
    Fx5Spec spec = Fx5Default(OFFERED_SLOT);
    unsigned char canonical[136];
    spec.species = 300;
    Fx5Canonical(&spec, canonical);
    assert(WtPokemon_Encode(canonical, loaded + Rl5Offset(4) + 7 * FX5_BOX_MON) == 0);
}

/* An escrow whose removal never reached the card, after the player moved the
 * Pokémon: within the party it is removed from where it is now; deposited in
 * the PC or the Battle Box, or changed, it is kept (LUNE-902): REMOVE answers
 * KEPT, changing nothing, and the engine abandons the unsent escrow. */
static void EscrowAfterPlay(void)
{
    static const Action kept[3] = { DepositOffered, BattleBoxOffered, TrainOffered };
    static const char *const kinds[3] = { "deposited in the PC", "put in the Battle Box", "trained" };
    static unsigned char before[CP_SAVE_FILE_SIZE];
    WtJournalEntry journal, entry;
    unsigned char digest[32], canonical[136];
    unsigned i;
    Arrange(ESCROW, &journal);
    StockPlay(1, SwapFirstTwo);
    Load();
    entry = journal;
    assert(Step(WT_INVENTORY_REMOVE, &entry) == 0 && Step(WT_INVENTORY_VERIFY_REMOVED, &entry) == 0);
    assert(entry.source.slot == 0 && Copies() == 0 && Fx5ImageCount(&image) == 3);
    assert(CpSaveImage_Hash(&image, digest) == CP_SAVE_OK && memcmp(digest, entry.inventoryAfter, 32) == 0);
    assert(Step(WT_INVENTORY_PREPARE_RESTORE, &entry) == 0 && Step(WT_INVENTORY_RESTORE, &entry) == 0);
    assert(Copies() == 1);
    Load();
    assert(WtPokemon_Decode(Fx5ImageParty(&image, 0), canonical) == 0 && memcmp(canonical, offered, 136) == 0);
    printf("PASS %s stock save in escrow: moved within the party, removed from its new slot and restored there\n",
           FX5_NAME);
    for (i = 0; i < 3; ++i) {
        Arrange(ESCROW, &journal);
        StockPlay(1, kept[i]);
        Load();
        memcpy(before, file.bytes, sizeof(before));
        entry = journal;
        assert(Step(WT_INVENTORY_REMOVE, &entry) == WT_INVENTORY_KEPT);
        /* The save keeps it (a trained one no longer matches its image). */
        assert(memcmp(before, file.bytes, sizeof(before)) == 0 && Copies() == (i < 2 ? 1u : 0u));
        printf("PASS %s stock save in escrow: %s, kept and the save untouched\n", FX5_NAME, kinds[i]);
    }
}

/* The returned Pokémon, wherever it is in the party, moved to Battle Box slot 4. */
static void BattleBoxReturned(unsigned char *loaded)
{
    unsigned char canonical[136], *party = loaded + Rl5Offset(FX5_PARTY);
    unsigned count = Fx5R32(party + 4), slot, i;
    for (slot = 0; slot < count; ++slot) {
        assert(WtPokemon_Decode(PartySlot(loaded, slot), canonical) == 0);
        if (memcmp(canonical, offered, 136) == 0) break;
    }
    if (slot == count) return;
    memcpy(loaded + Rl5Offset(FX5_BATTLE_BOX) + 4 * FX5_BOX_MON, PartySlot(loaded, slot), FX5_BOX_MON);
    for (i = slot; i + 1 < count; ++i) memcpy(PartySlot(loaded, i), PartySlot(loaded, i + 1), FX5_MON);
    Fx5Empty(PartySlot(loaded, count - 1), FX5_MON);
    Fx5W32(party + 4, count - 1);
}

/* A return cut at every write, then the stock game moves the Pokémon, if the
 * save it loaded holds it, to the Battle Box: RECEIVE records it there (journal
 * locator kind 3) and never adds it again; a save without it receives it once. */
static void ReturnInBattleBox(void)
{
    static unsigned char resumed[CP_SAVE_FILE_SIZE];
    WtJournalEntry journal, entry;
    unsigned cut, writes = 8, inBox = 0;
    for (cut = 1; cut <= writes + 1; ++cut) {
        Arrange(APPLYING_RETURN, &journal);
        entry = journal;
        file.failWrite = file.writes + cut;
        assert(Step(WT_INVENTORY_RECEIVE, &entry) == (cut <= writes ? -1 : 0));
        file.failWrite = 0;
        StockPlay(1, BattleBoxReturned);
        Load();
        entry = journal;
        assert(Step(WT_INVENTORY_RECEIVE, &entry) == 0 && Step(WT_INVENTORY_VERIFY_RECEIVED, &entry) == 0);
        assert(Copies() == 1);
        if (entry.destination.kind == 3) {
            assert(entry.destination.slot == 4 && entry.destination.length == FX5_BOX_MON);
            ++inBox;
        } else {
            assert(entry.destination.kind == 1);
        }
        memcpy(resumed, file.bytes, sizeof(resumed));
        /* Resumed again from the recorded entry: nothing changes. */
        Load();
        assert(Step(WT_INVENTORY_RECEIVE, &entry) == 0 && Step(WT_INVENTORY_VERIFY_RECEIVED, &entry) == 0);
        assert(memcmp(resumed, file.bytes, sizeof(resumed)) == 0 && Copies() == 1);
        ++cases;
    }
    /* At least the completed write reached the Battle Box. */
    assert(inBox >= 1);
    printf("PASS %s stock save after a return: moved to the Battle Box, recorded there in %u of %u cuts, one copy every time\n",
           FX5_NAME, inBox, writes + 1);
}

static void SubmitReadyAfterPlay(void)
{
    WtJournalEntry journal;
    Arrange(RESTORING, &journal);
    journal.phase = WT_JOURNAL_SUBMIT_READY;
    StockPlay(2, NULL);
    Load();
    assert(Step(WT_INVENTORY_VERIFY_REMOVED, &journal) == 0 && WtGame_CanLeave(&game, &journal));
    assert(Copies() == 0);
    printf("PASS %s stock save once submitted: still proven removed\n", FX5_NAME);
}

static void IdentityCollisions(void)
{
    static unsigned char before[CP_SAVE_FILE_SIZE];
    WtJournalEntry journal, entry;
    unsigned cut;
    Arrange(APPLYING_RETURN, &journal);
    StockPlay(1, Impostor);
    Load();
    memcpy(before, file.bytes, sizeof(before));
    entry = journal;
    assert(Step(WT_INVENTORY_RECEIVE, &entry) == -1);
    assert(memcmp(before, file.bytes, sizeof(before)) == 0);
    Arrange(RESTORING, &journal);
    StockPlay(1, Impostor);
    Load();
    memcpy(before, file.bytes, sizeof(before));
    entry = journal;
    assert(Step(WT_INVENTORY_PREPARE_RESTORE, &entry) == 1);
    entry.deliveryKind = WT_DELIVERY_RETURN; memcpy(entry.incoming, entry.outgoing, 136);
    assert(Step(WT_INVENTORY_PREPARE_RECEIVE, &entry) == -1);
    assert(memcmp(before, file.bytes, sizeof(before)) == 0);
    for (cut = 1; cut <= 9; cut += 8) {
        Arrange(ESCROW, &journal);
        entry = journal;
        file.failWrite = file.writes + cut;
        assert(Step(WT_INVENTORY_REMOVE, &entry) == (cut <= 8 ? -1 : 0));
        file.failWrite = 0;
        StockPlay(1, Impostor);
        Load();
        memcpy(before, file.bytes, sizeof(before));
        entry = journal;
        /* Not removed: two Pokémon with the identity, refused. Removed: the
         * save's only one is taken as the offer as play left it (LUNE-902). */
        assert(Step(WT_INVENTORY_REMOVE, &entry) == (cut <= 8 ? -1 : WT_INVENTORY_KEPT));
        assert(memcmp(before, file.bytes, sizeof(before)) == 0);
    }
    printf("PASS %s stock save: another Pokemon with the same identity is never taken as the offer\n", FX5_NAME);
}

int main(void)
{
    Sweep(ESCROW);
    Sweep(RESTORING);
    Sweep(APPLYING_RETURN);
    EscrowAfterPlay();
    SubmitReadyAfterPlay();
    ReturnInBattleBox();
    IdentityCollisions();
    printf("PASS %s stock save recovery: %u cut and stock-save cases, 0 stuck, 0 duplicates, 0 lost\n", FX5_NAME, cases);
    return 0;
}
