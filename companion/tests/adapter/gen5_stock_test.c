/* LUNE-881: LUNE-899's stock-game recovery on Black/White (FX5_GAME 0) and
 * Black 2/White 2 (1) saves. The companion stops in the middle of a step, the
 * player loads and saves in the stock game (modelled independently,
 * adapter/gen5_stock.h), and the step resumes from the journal. Every step is
 * cut before each of its writes and after the last; the offered Pokémon must
 * end up in exactly one place: the save or the journal.
 *
 * LUNE-906: a return the stock game saved over is never put back again (the
 * games' never-duplicate rule, client wonder_trade/delivery.h): kept, released
 * or traded away there, it is never added; only a cut before the first retail
 * copy held it loses it. */
#include "gen5_stock.h"

#include <stdio.h>

static Fx5File file;
static CpSaveImage image;
static CpFile cpFile = { &file, Fx5Read, Fx5Write, Fx5Sync, Fx5Size };
static WtGame game;
static unsigned char offered[136], ram[0x26000];
static unsigned cases, windowLosses;

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

/* Pokémon with the offered one's identity the stock game loads, in any state. */
static unsigned IdCopies(void)
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
        copies += Fx5R16(canonical + 8) && memcmp(canonical, offered, 4) == 0 && memcmp(canonical + 12, offered + 12, 4) == 0;
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
        /* The engine records it as a return to no destination first (LUNE-898). */
        journal->deliveryKind = WT_DELIVERY_RETURN;
        memcpy(journal->incoming, journal->outgoing, 136);
        return;
    }
    GameSaves(40);
    assert(Step(WT_INVENTORY_PREPARE_RESTORE, journal) == 1);
    journal->deliveryKind = WT_DELIVERY_RETURN;
    memcpy(journal->incoming, journal->outgoing, 136);
    assert(Step(WT_INVENTORY_PREPARE_RECEIVE, journal) == 0);
}

/* The engine's Restore (adapter/stock_save_test.c Restore). */
static void Restore(WtJournalEntry *entry)
{
    int inPlace = entry->deliveryKind == WT_DELIVERY_RETURN && entry->destination.kind == 0, prepared;
    if (entry->deliveryKind != WT_DELIVERY_RETURN || inPlace) {
        prepared = Step(WT_INVENTORY_PREPARE_RESTORE, entry);
        if (prepared == 0) {
            entry->deliveryKind = WT_DELIVERY_RETURN;
            memcpy(entry->incoming, entry->outgoing, 136);
            assert(Step(WT_INVENTORY_RESTORE, entry) == 0);
            return;
        }
        assert(prepared == 1);
        if (!inPlace) {
            entry->deliveryKind = WT_DELIVERY_RETURN;
            memcpy(entry->incoming, entry->outgoing, 136);
            assert(Step(WT_INVENTORY_PREPARE_RECEIVE, entry) == 0);
        }
    }
    assert(Step(WT_INVENTORY_RECEIVE, entry) == 0 && Step(WT_INVENTORY_VERIFY_RECEIVED, entry) == 0);
}

static void Resume(enum State state, WtJournalEntry *entry, unsigned copies)
{
    if (state == ESCROW) {
        assert(Step(WT_INVENTORY_REMOVE, entry) == 0 && Step(WT_INVENTORY_VERIFY_REMOVED, entry) == 0);
        entry->phase = WT_JOURNAL_SUBMIT_READY;
        assert(WtGame_CanLeave(&game, entry));
        assert(Copies() == 0);
    } else {
        Restore(entry);
        /* 2: not in the save the stock game loaded; restored if that save
         * is still the escrow's inventory, else lost (the games' window). */
        assert(copies == 2 ? IdCopies() <= 1 : IdCopies() == copies);
        entry->phase = WT_JOURNAL_APPLIED;
        assert(WtGame_CanLeave(&game, entry));
    }
}

/* LUNE-906: what the player does with the returned offer in the stock game. */
enum Fate { KEPT, RELEASED, TRADED, FATES };
static const char *const kFates[] = { "kept", "released", "traded away" };
static enum Fate fate;
static void LetGo(unsigned char *loaded) { (void)Fx5Gone(loaded, offered, fate == TRADED); }

static void Sweep(enum State state)
{
    static unsigned char resumed[CP_SAVE_FILE_SIZE];
    WtJournalEntry journal, entry;
    unsigned writes, cut, saves, held, lost;
    Arrange(state, &journal);
    entry = journal; writes = file.writes;
    assert(Step(kCutStep[state], &entry) == 0);
    writes = file.writes - writes;
    /* Two passes: the party, the CRC table, the footer and its last byte; a
     * removal first zeroes each footer's magic and writes its count (LUNE-902). */
    assert(writes == (state == ESCROW ? 12u : 8u));
    for (fate = KEPT; fate < (state == ESCROW ? RELEASED : FATES); ++fate) {
    lost = 0;
    for (cut = 1; cut <= writes + 1; ++cut) for (saves = 1; saves <= 2; ++saves) {
        Arrange(state, &journal);
        entry = journal;
        file.failWrite = file.writes + cut;
        assert(Step(kCutStep[state], &entry) == (cut <= writes ? -1 : 0));
        file.failWrite = 0;
        /* The stock game loads a return once the first copy holding it is complete. */
        held = IdCopies();
        if (state != ESCROW) assert(held == (cut > writes / 2));
        StockPlay(saves, fate == KEPT ? NULL : LetGo);
        Load();
        entry = journal;
        Resume(state, &entry, !held ? 2 : fate == KEPT);
        if (state != ESCROW && !held && !IdCopies()) { ++lost; ++windowLosses; }
        memcpy(resumed, file.bytes, sizeof(resumed));
        Load();
        entry = journal;
        Resume(state, &entry, !held ? 2 : fate == KEPT);
        assert(memcmp(resumed, file.bytes, sizeof(resumed)) == 0);
        if (state == ESCROW) {
            entry.phase = WT_JOURNAL_RESTORING;
            Restore(&entry);
            assert(Copies() == 1);
        }
        ++cases;
    }
    if (state == ESCROW)
        printf("PASS %s stock save after a cut in %s: %u cuts x 2 stock plays, one copy every time\n", FX5_NAME,
               kStates[state], writes + 1);
    else
        printf("PASS %s stock save after a cut in %s, offer %s: %u cuts x 2 stock plays, 0 duplicates, "
               "lost only when cut before the first retail copy (%u cases)\n", FX5_NAME, kStates[state], kFates[fate],
               writes + 1, lost);
    }
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
 * the PC or the Battle Box, or changed, the companion refuses, changing nothing. */
static void EscrowAfterPlay(void)
{
    static const Action refused[3] = { DepositOffered, BattleBoxOffered, TrainOffered };
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
        StockPlay(1, refused[i]);
        Load();
        memcpy(before, file.bytes, sizeof(before));
        entry = journal;
        assert(Step(WT_INVENTORY_REMOVE, &entry) == -1);
        /* The save keeps it (a trained one no longer matches its image). */
        assert(memcmp(before, file.bytes, sizeof(before)) == 0 && Copies() == (i < 2 ? 1u : 0u));
        printf("PASS %s stock save in escrow: %s, refused and the save untouched\n", FX5_NAME, kinds[i]);
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
 * locator kind 3) and never adds it again; a save without it accepts the loss
 * (LUNE-906: a cut before the first copy held it). */
static void ReturnInBattleBox(void)
{
    static unsigned char resumed[CP_SAVE_FILE_SIZE];
    WtJournalEntry journal, entry;
    unsigned cut, writes = 8, inBox = 0, held;
    for (cut = 1; cut <= writes + 1; ++cut) {
        Arrange(APPLYING_RETURN, &journal);
        entry = journal;
        file.failWrite = file.writes + cut;
        assert(Step(WT_INVENTORY_RECEIVE, &entry) == (cut <= writes ? -1 : 0));
        file.failWrite = 0;
        held = Copies();
        StockPlay(1, BattleBoxReturned);
        Load();
        entry = journal;
        assert(Step(WT_INVENTORY_RECEIVE, &entry) == 0 && Step(WT_INVENTORY_VERIFY_RECEIVED, &entry) == 0);
        /* Not in the save the game loaded: received if that save is still the
         * recorded inventory, else lost (the games' window). */
        held = held ? 1 : Copies();
        assert(Copies() == held);
        if (entry.destination.kind == 3) {
            assert(held && entry.destination.slot == 4 && entry.destination.length == FX5_BOX_MON);
            ++inBox;
        } else {
            assert(entry.destination.kind == 1);
            if (!held) ++windowLosses;
        }
        memcpy(resumed, file.bytes, sizeof(resumed));
        /* Resumed again from the journal, as a cut before it recorded the
         * recovery leaves it: nothing changes. */
        Load();
        entry = journal;
        assert(Step(WT_INVENTORY_RECEIVE, &entry) == 0 && Step(WT_INVENTORY_VERIFY_RECEIVED, &entry) == 0);
        assert(memcmp(resumed, file.bytes, sizeof(resumed)) == 0 && Copies() == held);
        ++cases;
    }
    /* At least the completed write reached the Battle Box. */
    assert(inBox >= 1);
    printf("PASS %s stock save after a return: moved to the Battle Box, recorded there in %u of %u cuts, never added\n",
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

/* LUNE-906: a return the stock game saved over takes the one Pokémon with its
 * identity for itself, in whatever state play left it (an evolution changes
 * the species), as the games do; two are never told apart (LUNE-914). */
static void IdentityCollisions(void)
{
    static unsigned char before[CP_SAVE_FILE_SIZE];
    WtJournalEntry journal, entry;
    unsigned cut;
    Arrange(APPLYING_RETURN, &journal);
    StockPlay(1, Impostor);
    Load();
    entry = journal;
    assert(Step(WT_INVENTORY_RECEIVE, &entry) == 0 && Step(WT_INVENTORY_VERIFY_RECEIVED, &entry) == 0);
    assert(IdCopies() == 1 && Copies() == 0);
    assert(entry.destination.kind == 2 && entry.destination.box == 3 && entry.destination.slot == 7);
    Arrange(RESTORING, &journal);
    StockPlay(1, Impostor);
    Load();
    entry = journal;
    Restore(&entry);
    assert(IdCopies() == 1 && Copies() == 0 && entry.destination.kind == 2);
    for (cut = 0; cut <= 1; ++cut) {
        Arrange(cut ? RESTORING : APPLYING_RETURN, &journal);
        entry = journal;
        if (cut) Restore(&entry);
        else assert(Step(WT_INVENTORY_RECEIVE, &entry) == 0);
        StockPlay(1, Impostor);
        Load();
        assert(IdCopies() == 2);
        memcpy(before, file.bytes, sizeof(before));
        entry = journal;
        assert(Step(cut ? WT_INVENTORY_PREPARE_RESTORE : WT_INVENTORY_RECEIVE, &entry) == (cut ? 1 : -1));
        if (cut) assert(Step(WT_INVENTORY_RECEIVE, &entry) == -1);
        assert(memcmp(before, file.bytes, sizeof(before)) == 0);
    }
    for (cut = 1; cut <= 13; cut += 12) {
        Arrange(ESCROW, &journal);
        entry = journal;
        file.failWrite = file.writes + cut;
        assert(Step(WT_INVENTORY_REMOVE, &entry) == (cut <= 12 ? -1 : 0));
        file.failWrite = 0;
        StockPlay(1, Impostor);
        Load();
        memcpy(before, file.bytes, sizeof(before));
        entry = journal;
        assert(Step(WT_INVENTORY_REMOVE, &entry) == -1);
        assert(memcmp(before, file.bytes, sizeof(before)) == 0);
    }
    printf("PASS %s stock save: one Pokemon with the identity is taken as the return, two are refused untouched\n", FX5_NAME);
}

/* ---- LUNE-913: the Day Care, the Dream World, the GTS, DNA Splicers ------- */

/* Where the stock game may hold the offered Pokémon outside the party, the PC
 * and the Battle Box: block, image offset, flag offset and size (0: none). */
typedef struct Place {
    const char *name;
    unsigned block, mon, flag, flagSize;
} Place;

static const Place kPlaces[] = {
    { "left at the Day Care (first slot)", FX5_DAYCARE, 0x04, 0x00, 4 },
    { "left at the Day Care (second slot)", FX5_DAYCARE, FX5_DAYCARE_SLOT + 0x04, FX5_DAYCARE_SLOT, 4 },
    { "tucked in for the Dream World", FX5_DREAM_WORLD, 0x08, 0x1A3, 1 },
    { "deposited at the GTS", FX5_GTS, 0x00, 0xEC, 2 },
#if FX5_GAME == 1
    { "fused into Kyurem with the DNA Splicers", FX5_FUSION, 0x04, 0, 0 },
#endif
};
enum { PLACES = sizeof(kPlaces) / sizeof(kPlaces[0]) };
static unsigned place, healed;

static unsigned char *PlaceMon(unsigned char *loaded, const Place *at) { return loaded + Rl5Offset(at->block) + at->mon; }

static void SetFlag(unsigned char *loaded, const Place *at, unsigned value)
{
    unsigned char *flag = loaded + Rl5Offset(at->block) + at->flag;
    if (at->flagSize == 1) flag[0] = (unsigned char)value;
    else if (at->flagSize == 2) Fx5W16(flag, value);
    else if (at->flagSize == 4) Fx5W32(flag, value);
}

/* A fused Kyurem (Black Kyurem, form 1) in PC tray 3, slot 9. */
static void FusedKyurem(unsigned char *loaded, int present)
{
    unsigned char *slot = loaded + Rl5Offset(3) + 9 * FX5_BOX_MON;
    if (present) {
        Fx5Spec spec = Fx5Default(5);
        unsigned char canonical[136];
        spec.species = 646; spec.form = 1; spec.pid = 0x6460001;
        Fx5Canonical(&spec, canonical);
        assert(WtPokemon_Encode(canonical, slot) == 0);
    } else Fx5Empty(slot, FX5_BOX_MON);
}

/* The offered Pokémon's party slot in the loaded save, or the count. */
static unsigned OfferedSlot(unsigned char *loaded)
{
    unsigned char canonical[136];
    unsigned count = Fx5R32(loaded + Rl5Offset(FX5_PARTY) + 4), slot;
    for (slot = 0; slot < count; ++slot) {
        assert(WtPokemon_Decode(PartySlot(loaded, slot), canonical) == 0);
        if (memcmp(canonical, offered, 4) == 0 && memcmp(canonical + 12, offered + 12, 4) == 0) break;
    }
    return slot;
}

/* The game moves the whole party image there, sets the flag and removes it
 * from the party. `healed`: the Day Care's deposit restored its PP first. */
static void HoldOffered(unsigned char *loaded)
{
    const Place *at = &kPlaces[place];
    unsigned char *party = loaded + Rl5Offset(FX5_PARTY), *to = PlaceMon(loaded, at);
    unsigned count = Fx5R32(party + 4), slot = OfferedSlot(loaded), i;
    if (slot == count) return;
    memcpy(to, PartySlot(loaded, slot), FX5_MON);
    if (healed) {
        unsigned char canonical[136];
        unsigned sum = 0;
        assert(WtPokemon_Decode(to, canonical) == 0);
        canonical[0x30] = 35; /* Tackle's PP */
        for (i = 8; i < 136; i += 2) sum += Fx5R16(canonical + i);
        Fx5W16(canonical + 6, sum & 0xFFFF);
        assert(WtPokemon_Encode(canonical, to) == 0);
    }
    SetFlag(loaded, at, 1);
    if (!at->flagSize) FusedKyurem(loaded, 1);
    for (i = slot; i + 1 < count; ++i) memcpy(PartySlot(loaded, i), PartySlot(loaded, i + 1), FX5_MON);
    Fx5Empty(PartySlot(loaded, count - 1), FX5_MON);
    Fx5W32(party + 4, count - 1);
}

/* Taken back to the end of the party as it went in; the place keeps its old
 * image, only the flag clears (the fused Kyurem splits back). */
static void WithdrawOffered(unsigned char *loaded)
{
    const Place *at = &kPlaces[place];
    unsigned char *party = loaded + Rl5Offset(FX5_PARTY);
    unsigned count = Fx5R32(party + 4);
    assert(count < 6);
    memcpy(PartySlot(loaded, count), PlaceMon(loaded, at), FX5_MON);
    Fx5W32(party + 4, count + 1);
    SetFlag(loaded, at, 0);
    if (!at->flagSize) FusedKyurem(loaded, 0);
}

/* Whether the place holds the offered Pokémon in the save the game loads. */
static int Held(void)
{
    const Place *at = &kPlaces[place];
    const unsigned char *flag;
    unsigned char canonical[136];
    RetailLoad5(file.bytes, ram);
    flag = ram + Rl5Offset(at->block) + at->flag;
    if (at->flagSize == 0) {
        assert(WtPokemon_Decode(ram + Rl5Offset(3) + 9 * FX5_BOX_MON, canonical) == 0);
        if (Fx5R16(canonical + 8) != 646) return 0;
    } else if (!(at->flagSize == 1 ? flag[0] : at->flagSize == 2 ? Fx5R16(flag) : Fx5R32(flag))) return 0;
    assert(WtPokemon_Decode(PlaceMon(ram, at), canonical) == 0);
    return memcmp(canonical, offered, 4) == 0 && memcmp(canonical + 12, offered + 12, 4) == 0;
}

/* Every cut of REMOVE in escrow and of RECEIVE for a return, then the stock
 * game moves the offered Pokémon there (when the save it loaded holds it): the
 * step refuses and writes nothing, the Pokémon is never sent nor added twice.
 * Once the player takes it back, the step finishes with one copy; healed, the
 * escrow stays refused and the return is recorded where it is (LUNE-906). A
 * return the loaded save did not hold yet is never added. */
static void HeldElsewhere(void)
{
    static unsigned char before[CP_SAVE_FILE_SIZE];
    WtJournalEntry journal, entry;
    unsigned cut, held, state, wasHeld;
    for (place = 0; place < PLACES; ++place) for (healed = 0; healed <= (place < 2); ++healed)
    for (state = ESCROW; state <= APPLYING_RETURN; state += APPLYING_RETURN - ESCROW) {
        enum WtInventoryStep cutStep = kCutStep[state];
        held = 0;
        unsigned writes = state == ESCROW ? 12u : 8u;
        for (cut = 1; cut <= writes + 1; ++cut) {
            Arrange((enum State)state, &journal);
            entry = journal;
            file.failWrite = file.writes + cut;
            assert(Step(cutStep, &entry) == (cut <= writes ? -1 : 0));
            file.failWrite = 0;
            StockPlay(1, HoldOffered);
            Load();
            entry = journal;
            wasHeld = 0;
            if (Held()) {
                ++held; wasHeld = 1;
                memcpy(before, file.bytes, sizeof(before));
                assert(Step(cutStep, &entry) == -1);
                assert(memcmp(before, file.bytes, sizeof(before)) == 0 && Copies() == 0);
                StockPlay(1, WithdrawOffered);
                Load();
                entry = journal;
                if (healed && state == ESCROW) {
                    /* Back changed: never proof either way, still refused. */
                    memcpy(before, file.bytes, sizeof(before));
                    assert(Step(cutStep, &entry) == -1 && memcmp(before, file.bytes, sizeof(before)) == 0);
                    ++cases;
                    continue;
                }
            }
            /* Taken back (or never there): the step finishes once. */
            assert(Step(cutStep, &entry) == 0);
            if (state == ESCROW) {
                assert(Step(WT_INVENTORY_VERIFY_REMOVED, &entry) == 0 && Copies() == 0 && !Held());
            } else {
                assert(Step(WT_INVENTORY_VERIFY_RECEIVED, &entry) == 0 && (wasHeld ? IdCopies() == 1 : IdCopies() <= 1) && !Held());
            }
            ++cases;
        }
        /* Every cut before the step's first write leaves the Pokémon in the party. */
        assert(held >= 1);
        printf("PASS %s stock save in %s, then %s%s: refused untouched in %u of 9 cuts, %s once taken back\n",
            FX5_NAME, kStates[state], kPlaces[place].name, healed ? " (healed)" : "", held,
            healed && state == ESCROW ? "still refused (changed)" : state == ESCROW ? "finished" : "recorded where it is");
    }
}

/* The offer screen: a Pokémon held elsewhere with the offered one's identity
 * makes it not unique; an old image the place kept after it left does not. */
static void HeldUnique(void)
{
    for (place = 0; place < PLACES; ++place) {
        Setup(4);
        RetailLoad5(file.bytes, ram);
        memcpy(PlaceMon(ram, &kPlaces[place]), PartySlot(ram, OFFERED_SLOT), FX5_MON);
        StockSave5(file.bytes, ram);
        Load();
        assert(CpGen5_Unique(&image, OFFERED_SLOT));
        RetailLoad5(file.bytes, ram);
        SetFlag(ram, &kPlaces[place], 1);
        if (!kPlaces[place].flagSize) FusedKyurem(ram, 1);
        StockSave5(file.bytes, ram);
        Load();
        assert(!CpGen5_Unique(&image, OFFERED_SLOT));
        printf("PASS %s offer screen: a copy %s is not unique; an old image there is ignored\n", FX5_NAME,
               kPlaces[place].name);
    }
}

/* LUNE-902: abandoning an escrow is proved by the save's history alone. */
static void ReleaseOffered(unsigned char *loaded)
{
    unsigned char *party = loaded + Rl5Offset(FX5_PARTY);
    unsigned count = Fx5R32(party + 4), i;
    for (i = OFFERED_SLOT; i + 1 < count; ++i) memcpy(PartySlot(loaded, i), PartySlot(loaded, i + 1), FX5_MON);
    Fx5Empty(PartySlot(loaded, count - 1), FX5_MON);
    Fx5W32(party + 4, count - 1);
}

/* A twin from the stock game (an N's Pokémon, say): the offer's identity and
 * every field play leaves alone, differing in experience. */
static void AddTwin(unsigned char *loaded)
{
    unsigned char *slot = loaded + Rl5Offset(4) + 9 * FX5_BOX_MON;
    unsigned char canonical[136];
    unsigned sum = 0, i;
    memcpy(canonical, offered, 136);
    Fx5W32(canonical + 0x10, Fx5R32(canonical + 0x10) + 100);
    for (i = 8; i < 136; i += 2) sum += Fx5R16(canonical + i);
    Fx5W16(canonical + 6, sum & 0xFFFF);
    assert(WtPokemon_Encode(canonical, slot) == 0);
}

/* The twin left at the Day Care (LUNE-913: Locate sees it there). */
static void TwinInDayCare(unsigned char *loaded)
{
    unsigned char canonical[136];
    unsigned sum = 0, i;
    memcpy(canonical, offered, 136);
    Fx5W32(canonical + 0x10, Fx5R32(canonical + 0x10) + 100);
    for (i = 8; i < 136; i += 2) sum += Fx5R16(canonical + i);
    Fx5W16(canonical + 6, sum & 0xFFFF);
    assert(WtPokemon_Encode(canonical, PlaceMon(loaded, &kPlaces[0])) == 0);
    SetFlag(loaded, &kPlaces[0], 1);
}

/* Traded away in the stock game: another Pokémon takes its party slot. */
static void TradeOffered(unsigned char *loaded)
{
    Fx5Spec spec = Fx5Default(30);
    Fx5PartyMon(&spec, PartySlot(loaded, OFFERED_SLOT));
}

static void TwinAndTrain(unsigned char *loaded)
{
    AddTwin(loaded);
    TrainOffered(loaded);
}

/* As stock_save_test.c History, on this generation's save: the removal cut
 * at each write and after the last, the stock game played and saved over it.
 * WT_INVENTORY_HISTORY writes nothing and answers 1 only when what the stock game
 * loaded still held the offer exactly as offered (always, when play then
 * moved it, unless play left exactly the recorded removal); a twin after the removal is refused by REMOVE too, untouched. */
/* LUNE-902: an interrupted stock save left one copy's footer not loading: the
 * current save count, and a CRC that is the removal's CRC table's (a 1-in-65536
 * coincidence in life). staleCopy 1 is the main copy, 2 the backup. */
static unsigned staleCrc, staleCopy;
static void StaleFooter(void)
{
    unsigned char *base = file.bytes + (staleCopy == 2 ? FX5_BACKUP : 0);
    Fx5W32(base + FX5_USED - 16, image.globalCounter);
    Fx5W32(base + FX5_USED - 12, FX5_USED);
    Fx5W32(base + FX5_USED - 8, CP_SAVE_MAGIC_GEN5);
    Fx5W16(base + FX5_USED - 2, staleCrc);
}

static void History(unsigned stale)
{
    static const Action actions[] = { NULL, TrainOffered, DepositOffered, BattleBoxOffered, ReleaseOffered, TradeOffered,
                                      AddTwin, TwinAndTrain, TwinInDayCare };
    enum { TWINS = 6 };
    static unsigned char before[CP_SAVE_FILE_SIZE];
    WtJournalEntry journal, entry;
    unsigned writes, cut, saves, action, abandoned = 0, refused = 0, held;
    int answer;
    staleCopy = stale;
    Arrange(ESCROW, &journal);
    entry = journal; writes = file.writes;
    assert(Step(WT_INVENTORY_REMOVE, &entry) == 0);
    writes = file.writes - writes;
    assert(image.globalCounter - journal.globalCounter >= WT_ABANDON_STEP);
    if (stale) {
        const unsigned char *base = file.bytes + (stale == 2 ? FX5_BACKUP : 0);
        staleCrc = Fx5R16(base + FX5_USED - 2);
        Arrange(ESCROW, &journal);
        StaleFooter(); Load(); /* the companion loads the save as it is */
        entry = journal; writes = file.writes;
        assert(Step(WT_INVENTORY_REMOVE, &entry) == 0);
        writes = file.writes - writes;
    }
    for (cut = 1; cut <= writes + 1; ++cut) for (saves = 1; saves <= 2; ++saves)
        for (action = 0; action < sizeof(actions) / sizeof(*actions); ++action) {
            Arrange(ESCROW, &journal);
            if (stale) { StaleFooter(); Load(); }
            entry = journal;
            file.failWrite = file.writes + cut;
            assert(Step(WT_INVENTORY_REMOVE, &entry) == (cut <= writes ? -1 : 0));
            file.failWrite = 0;
            held = Copies();
            assert(held <= 1);
            StockPlay(saves, actions[action]);
            Load();
            memcpy(before, file.bytes, sizeof(before));
            entry = journal;
            answer = Step(WT_INVENTORY_HISTORY, &entry);
            assert(memcmp(before, file.bytes, sizeof(before)) == 0);
            /* Never abandoned unless what the stock game loaded held the
             * offer; never taken as removed when it did. */
            if (held) assert(answer == WT_HISTORY_UNCHANGED || answer == WT_HISTORY_LEFT);
            else assert(answer == WT_HISTORY_REMOVED || answer == WT_HISTORY_AFTER);
            /* Once play moved, released or traded it, it is left: never sent. */
            if (held && action) assert(answer == WT_HISTORY_LEFT);
            abandoned += answer == WT_HISTORY_LEFT;
            /* A twin after the removal: taken as removed, so REMOVE never runs
             * and the twin is never taken for the offer. */
            if (!held && action >= TWINS) ++refused;
            ++cases;
        }
    assert(abandoned > 0 && refused > 0);
    printf("PASS %s stock save after a cut removal%s: %u cuts x 2 stock plays x 9 plays, abandonable %u times, "
           "only from a save that held the offer; %u twins after the removal refused\n", FX5_NAME,
           stale ? " over a stale footer with the removal's table CRC" : "", writes + 1,
           abandoned, refused);
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
    HeldElsewhere();
    HeldUnique();
    History(0);
    History(1);
    History(2);
    printf("PASS %s stock save recovery: %u cut and stock-save cases, 0 stuck, 0 duplicates, %u lost "
        "(each cut before the first retail copy held the return, the games' window)\n", FX5_NAME, cases, windowLosses);
    return 0;
}
