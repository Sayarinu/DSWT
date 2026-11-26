/* LUNE-899: the companion stops in the middle of a step, the player then
 * loads and saves in the stock game, and only then opens the companion again.
 * Every step is cut before each of its writes and after the last one, each
 * cut is followed by one stock save of the general block, or two of both,
 * and the step resumes as the engine would resume it from the journal. The
 * offered Pokémon must end up in exactly one place: the save or the journal.
 *
 * LUNE-906: a returned offer the stock game saved over is never put back
 * again (client wonder_trade/delivery.h, the games' never-duplicate rule):
 * the save is accepted as it is. The player may have released or traded it
 * away there after loading it; the companion must not add it again. Only a
 * cut before the first retail copy that holds it was complete loses it. */
#include "stock_game.h"

#include <stdio.h>

static FxFile file;
static CpSaveImage image;
static CpFile cpFile = { &file, FxRead, FxWrite, FxSync, FxSize };
static WtGame game;
static unsigned char offered[136];
static unsigned cases, windowLosses;

enum { OFFERED_SLOT = 1 };

static void Setup(unsigned count)
{
    FxSpec specs[6];
    unsigned i;
    for (i = 0; i < count; ++i) specs[i] = FxDefault(i);
    FxBuild(&file, specs, count);
    FxCanonical(&specs[OFFERED_SLOT], offered);
    assert(CpSaveImage_Load(&image, &cpFile, CP_SAVE_MAGIC_INTERNATIONAL) == CP_SAVE_OK);
    memset(&game, 0, sizeof(game));
    game.image = &image; game.file = &cpFile;
}

static void Load(void)
{
    assert(CpSaveImage_Load(&image, &cpFile, CP_SAVE_MAGIC_INTERNATIONAL) == CP_SAVE_OK);
}

static int Step(enum WtInventoryStep step, WtJournalEntry *entry)
{
    unsigned slot = step == WT_INVENTORY_RECEIVE ? entry->destination.slot : entry->source.slot;
    return WtGame_Inventory(&game, step, entry, slot);
}

/* The patched game played: a change outside the party, saved to both copies. */
static void GameSaves(unsigned offset)
{
    unsigned char digest[32];
    CpSaveImage_Block(&image, 0)[offset] ^= 0x5a;
    assert(CpSaveImage_Hash(&image, digest) == CP_SAVE_OK);
    assert(CpSaveImage_CommitBoth(&image, &cpFile, digest) == CP_SAVE_OK);
}

/* The journal as the engine leaves it right before the step that is cut. */
enum State { ESCROW, RESTORING, APPLYING_RETURN };
static const char *const kStates[] = { "escrow (REMOVE)", "restoring (RESTORE)", "applying a return (RECEIVE)" };
static const enum WtInventoryStep kCutStep[] = { WT_INVENTORY_REMOVE, WT_INVENTORY_RESTORE, WT_INVENTORY_RECEIVE };

static void Arrange(enum State state, WtJournalEntry *journal)
{
    Setup(4);
    memset(journal, 0, sizeof(*journal));
    assert(Step(WT_INVENTORY_PREPARE_REMOVE, (journal->source.slot = OFFERED_SLOT, journal)) == 0);
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
    /* The patched game saved since: the offer returns like a delivery. */
    GameSaves(4000);
    assert(Step(WT_INVENTORY_PREPARE_RESTORE, journal) == 1);
    journal->deliveryKind = WT_DELIVERY_RETURN;
    memcpy(journal->incoming, journal->outgoing, 136);
    assert(Step(WT_INVENTORY_PREPARE_RECEIVE, journal) == 0);
}

/* The engine's Restore, from a RESTORING journal: in place while the save is
 * the escrow's, recorded as a return to no destination first; once a game
 * saved since, RECEIVE settles that return as the save is. */
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

/* Resume the cut step from the journal as WtEngine_Settle would. A return
 * leaves `copies` of the offer in the save. */
static void Resume(enum State state, WtJournalEntry *entry, unsigned copies)
{
    if (state == ESCROW) {
        assert(Step(WT_INVENTORY_REMOVE, entry) == 0 && Step(WT_INVENTORY_VERIFY_REMOVED, entry) == 0);
        entry->phase = WT_JOURNAL_SUBMIT_READY;
        assert(WtGame_CanLeave(&game, entry));
        assert(FxStockCopies(&file, offered) == 0); /* the journal holds it */
    } else {
        Restore(entry);
        /* 2: not in the save the stock game loaded; restored if that save
         * is still the escrow's inventory, else lost (the games' window). */
        assert(copies == 2 ? FxStockCopies(&file, offered) <= 1 : FxStockCopies(&file, offered) == copies);
        entry->phase = WT_JOURNAL_APPLIED;
        assert(WtGame_CanLeave(&game, entry));
    }
}

/* LUNE-906: what the stock-game player does with the offer, when the save
 * the game loaded holds it, before saving: nothing, release it, or trade it
 * away. */
enum Fate { KEPT, RELEASED, TRADED, FATES };
static const char *const kFates[] = { "kept", "released", "traded away" };
static enum Fate fate;
static void LetGo(unsigned char *general, unsigned char *boxes) { (void)FxGone(general, boxes, offered, fate == TRADED); }

/* Cut the step at each write, play the stock game, resume. */
static void Sweep(enum State state)
{
    static unsigned char resumed[CP_SAVE_FILE_SIZE];
    WtJournalEntry journal, entry;
    unsigned writes, cut, saves, held, lost;
    Arrange(state, &journal);
    entry = journal; writes = file.writes;
    assert(Step(kCutStep[state], &entry) == 0);
    writes = file.writes - writes;
    /* Two passes over two blocks: body, footer tail, footer; a removal also
     * writes the general block's counter first (LUNE-902). */
    assert(writes == (state == ESCROW ? 14u : 12u));
    for (fate = KEPT; fate < (state == ESCROW ? RELEASED : FATES); ++fate) {
    lost = 0;
    for (cut = 1; cut <= writes + 1; ++cut) for (saves = 1; saves <= 2; ++saves) {
        unsigned full = saves == 2;
        Arrange(state, &journal);
        entry = journal;
        file.failWrite = file.writes + cut; /* writes + 1: the step finished, the journal had not */
        assert(Step(kCutStep[state], &entry) == (cut <= writes ? -1 : 0));
        file.failWrite = 0;
        /* A return is in the save the stock game loads once the first retail
         * copy holding it is complete: both blocks of the first pass. */
        held = FxStockCopies(&file, offered);
        if (state != ESCROW) assert(held == (cut > writes / 2));
        assert(FxStockPlay(&file, saves, (int)full, fate == KEPT ? NULL : LetGo));
        Load();
        entry = journal;
        /* Never put back once the stock game saved: kept, it is there once;
         * let go, it stays gone; cut before it was written, it is lost. */
        Resume(state, &entry, !held ? 2 : fate == KEPT);
        if (state != ESCROW && !held && !FxStockCopies(&file, offered)) { ++lost; ++windowLosses; }
        /* Cut again after the recovery wrote the save, before the journal
         * recorded it: resuming from the same journal finds it done. */
        memcpy(resumed, file.bytes, sizeof(resumed));
        Load();
        entry = journal;
        Resume(state, &entry, !held ? 2 : fate == KEPT);
        assert(memcmp(resumed, file.bytes, sizeof(resumed)) == 0);
        /* A rejected offer comes back exactly once. */
        if (state == ESCROW) {
            entry.phase = WT_JOURNAL_RESTORING;
            Restore(&entry);
            assert(FxStockCopies(&file, offered) == 1);
        }
        ++cases;
    }
    if (state == ESCROW)
        printf("PASS stock save after a cut in %s: %u cuts x 2 stock plays, one copy every time\n", kStates[state],
            writes + 1);
    else
        printf("PASS stock save after a cut in %s, offer %s: %u cuts x 2 stock plays, 0 duplicates, "
            "lost only when cut before the first retail copy (%u cases)\n", kStates[state], kFates[fate], writes + 1, lost);
    }
}

/* What the stock-game player may do with the offered Pokémon before saving. */
static unsigned char *PartySlot(unsigned char *general, unsigned slot) { return general + FX_PARTY + 8 + slot * FX_MON; }

static void SwapFirstTwo(unsigned char *general, unsigned char *boxes)
{
    unsigned char held[FX_MON];
    (void)boxes;
    memcpy(held, PartySlot(general, 0), FX_MON);
    memcpy(PartySlot(general, 0), PartySlot(general, 1), FX_MON);
    memcpy(PartySlot(general, 1), held, FX_MON);
}

static void DepositOffered(unsigned char *general, unsigned char *boxes)
{
    unsigned count = Fx32(general + FX_PARTY + 4), i;
    memcpy(boxes + FX_PC + 5 * FX_BOX_MON, PartySlot(general, OFFERED_SLOT), FX_BOX_MON);
    for (i = OFFERED_SLOT; i + 1 < count; ++i) memcpy(PartySlot(general, i), PartySlot(general, i + 1), FX_MON);
    FxEmpty(PartySlot(general, count - 1), FX_MON);
    FxPut32(general + FX_PARTY + 4, count - 1);
}

static void TrainOffered(unsigned char *general, unsigned char *boxes)
{
    unsigned char canonical[136], *slot = PartySlot(general, OFFERED_SLOT);
    unsigned sum = 0, i;
    (void)boxes;
    assert(WtPokemon_Decode(slot, canonical) == 0);
    FxPut32(canonical + 0x10, Fx32(canonical + 0x10) + 100); /* experience */
    for (i = 8; i < 136; i += 2) sum += canonical[i] | (canonical[i + 1] << 8);
    FxPut16(canonical + 6, sum & 65535);
    assert(WtPokemon_Encode(canonical, slot) == 0);
}

/* An escrow whose removal never reached the card, after the player moved the
 * Pokémon: within the party it is removed from where it is now; deposited in
 * the PC or changed, the companion cannot know it is the one offered and
 * refuses, changing nothing (the save keeps it, the journal stays in escrow). */
static void EscrowAfterPlay(void)
{
    WtJournalEntry journal, entry;
    static unsigned char before[CP_SAVE_FILE_SIZE];
    unsigned char digest[32];
    Arrange(ESCROW, &journal);
    assert(FxStockPlay(&file, 1, 0, SwapFirstTwo));
    Load();
    entry = journal;
    assert(Step(WT_INVENTORY_REMOVE, &entry) == 0 && Step(WT_INVENTORY_VERIFY_REMOVED, &entry) == 0);
    assert(entry.source.slot == 0 && FxStockCopies(&file, offered) == 0 && FxImageCount(&image) == 3);
    /* Recorded against this save: a rejection puts it back in its new slot. */
    assert(CpSaveImage_Hash(&image, digest) == CP_SAVE_OK && memcmp(digest, entry.inventoryAfter, 32) == 0);
    assert(Step(WT_INVENTORY_PREPARE_RESTORE, &entry) == 0 && Step(WT_INVENTORY_RESTORE, &entry) == 0);
    assert(FxStockCopies(&file, offered) == 1);
    Load();
    {
        unsigned char canonical[136];
        assert(WtPokemon_Decode(FxImageParty(&image, 0), canonical) == 0 && memcmp(canonical, offered, 136) == 0);
    }
    puts("PASS stock save in escrow: moved within the party, removed from its new slot and restored there");

    Arrange(ESCROW, &journal);
    assert(FxStockPlay(&file, 1, 0, DepositOffered));
    Load();
    memcpy(before, file.bytes, sizeof(before));
    entry = journal;
    assert(Step(WT_INVENTORY_REMOVE, &entry) == -1);
    assert(memcmp(before, file.bytes, sizeof(before)) == 0 && FxStockCopies(&file, offered) == 1);
    puts("PASS stock save in escrow: deposited in the PC, refused and the save untouched");

    Arrange(ESCROW, &journal);
    assert(FxStockPlay(&file, 1, 0, TrainOffered));
    Load();
    memcpy(before, file.bytes, sizeof(before));
    entry = journal;
    assert(Step(WT_INVENTORY_REMOVE, &entry) == -1);
    assert(memcmp(before, file.bytes, sizeof(before)) == 0 && FxStockCopies(&file, offered) == 1);
    puts("PASS stock save in escrow: trained in the stock game, refused and the save untouched");
}

/* Submitted, then the stock game: the removal is already proven. */
static void SubmitReadyAfterPlay(void)
{
    WtJournalEntry journal;
    Arrange(RESTORING, &journal);
    journal.phase = WT_JOURNAL_SUBMIT_READY;
    assert(FxStockPlay(&file, 2, 1, NULL));
    Load();
    assert(Step(WT_INVENTORY_VERIFY_REMOVED, &journal) == 0 && WtGame_CanLeave(&game, &journal));
    assert(FxStockCopies(&file, offered) == 0);
    puts("PASS stock save once submitted: still proven removed");
}

/* Another Pokémon that shares the offered one's personality and trainer ID
 * (a different species, as an evolution is): never proof that an offer is
 * gone. A return the stock game saved over takes the one Pokémon with its
 * identity for itself, in whatever state play left it, as the games do
 * (delivery.h); two are never told apart. */
static void Impostor(unsigned char *general, unsigned char *boxes)
{
    FxSpec spec = FxDefault(OFFERED_SLOT);
    unsigned char canonical[136];
    (void)general;
    spec.species = 300;
    FxCanonical(&spec, canonical);
    assert(WtPokemon_Encode(canonical, boxes + FX_PC + 7 * FX_BOX_MON) == 0);
}

/* The PC's Pokémon sharing the offer's identity, and all of them. */
static unsigned Identities(void)
{
    unsigned char canonical[136];
    unsigned i, count = FxImageCount(&image), found = 0;
    for (i = 0; i < count + 18 * 30; ++i) {
        const unsigned char *at = i < count ? FxImageParty(&image, i) : FxImageBox(&image, (i - count) / 30, (i - count) % 30);
        if (WtPokemon_Decode(at, canonical) == 0 && (canonical[8] | canonical[9])
            && memcmp(canonical, offered, 4) == 0 && memcmp(canonical + 12, offered + 12, 4) == 0) ++found;
    }
    return found;
}

static void IdentityCollisions(void)
{
    static unsigned char before[CP_SAVE_FILE_SIZE];
    WtJournalEntry journal, entry;
    unsigned cut;
    /* A returned offer not back yet, one other Pokémon with its identity:
     * recorded as the return where it is, nothing added. */
    Arrange(APPLYING_RETURN, &journal);
    assert(FxStockPlay(&file, 1, 0, Impostor));
    Load();
    entry = journal;
    assert(Step(WT_INVENTORY_RECEIVE, &entry) == 0 && Step(WT_INVENTORY_VERIFY_RECEIVED, &entry) == 0);
    Load();
    assert(FxStockCopies(&file, offered) == 1 && Identities() == 1);
    assert(entry.destination.kind == 2 && entry.destination.box == 0 && entry.destination.slot == 7);
    /* A restore in place cut before it wrote: RECEIVE takes it the same way. */
    Arrange(RESTORING, &journal);
    assert(FxStockPlay(&file, 1, 0, Impostor));
    Load();
    entry = journal;
    Restore(&entry);
    Load();
    assert(FxStockCopies(&file, offered) == 1 && Identities() == 1 && entry.destination.kind == 2);
    /* Back and another with its identity: two, refused and nothing written. */
    for (cut = 0; cut <= 1; ++cut) {
        Arrange(cut ? RESTORING : APPLYING_RETURN, &journal);
        entry = journal;
        if (cut) Restore(&entry); /* in place, finished, the journal not yet IDLE */
        else assert(Step(WT_INVENTORY_RECEIVE, &entry) == 0);
        assert(FxStockPlay(&file, 1, 0, Impostor));
        Load();
        assert(Identities() == 2);
        memcpy(before, file.bytes, sizeof(before));
        entry = journal;
        assert(Step(cut ? WT_INVENTORY_PREPARE_RESTORE : WT_INVENTORY_RECEIVE, &entry) == (cut ? 1 : -1));
        if (cut) assert(Step(WT_INVENTORY_RECEIVE, &entry) == -1);
        assert(memcmp(before, file.bytes, sizeof(before)) == 0);
    }
    /* An escrow, removed or not: refused either way. */
    for (cut = 1; cut <= 15; cut += 14) {
        Arrange(ESCROW, &journal);
        entry = journal;
        file.failWrite = file.writes + cut;
        assert(Step(WT_INVENTORY_REMOVE, &entry) == (cut <= 14 ? -1 : 0));
        file.failWrite = 0;
        assert(FxStockPlay(&file, 1, 0, Impostor));
        Load();
        memcpy(before, file.bytes, sizeof(before));
        entry = journal;
        assert(Step(WT_INVENTORY_REMOVE, &entry) == -1);
        assert(memcmp(before, file.bytes, sizeof(before)) == 0);
    }
    puts("PASS stock save: one Pokemon with the identity is taken as the return, two are refused untouched");
}

/* ---- LUNE-913: held where no locator reaches ------------------------------- */

static void HeldArrange(int returning, WtJournalEntry *journal) { Arrange(returning ? APPLYING_RETURN : ESCROW, journal); }
static int HeldStep(enum WtInventoryStep step, WtJournalEntry *entry) { return Step(step, entry); }
static unsigned HeldCopies(void) { return FxStockCopies(&file, offered); }
static void (*heldAction)(unsigned char *general);
static void HeldAct(unsigned char *general, unsigned char *boxes) { (void)boxes; if (heldAction) heldAction(general); }
static void HeldStock(void (*action)(unsigned char *general))
{
    heldAction = action;
    assert(FxStockPlay(&file, 1, 0, HeldAct));
    Load();
}
#include "gen4_held.h"

/* LUNE-902: abandoning an escrow is proved by the save's history alone. */
static void ReleaseOffered(unsigned char *general, unsigned char *boxes)
{
    unsigned count = Fx32(general + FX_PARTY + 4), i;
    (void)boxes;
    for (i = OFFERED_SLOT; i + 1 < count; ++i) memcpy(PartySlot(general, i), PartySlot(general, i + 1), FX_MON);
    FxEmpty(PartySlot(general, count - 1), FX_MON);
    FxPut32(general + FX_PARTY + 4, count - 1);
}

/* Another Pokémon caught or traded for in the stock game that shares the
 * offer's identity and every field play leaves alone (an N's Pokémon, say),
 * differing in experience. */
static void AddTwin(unsigned char *general, unsigned char *boxes)
{
    unsigned char *slot = boxes + FX_PC + 9 * FX_BOX_MON;
    (void)general;
    assert(WtPokemon_Encode(offered, slot) == 0);
    FxTrain(slot);
}

/* The twin left at the Day Care (LUNE-913: Locate sees it there). */
static void TwinInDayCare(unsigned char *general, unsigned char *boxes)
{
    unsigned char canonical[136];
    unsigned sum = 0, i;
    (void)boxes;
    memcpy(canonical, offered, 136);
    FxPut32(canonical + 0x10, Fx32(canonical + 0x10) + 100);
    for (i = 8; i < 136; i += 2) sum += canonical[i] | (canonical[i + 1] << 8);
    FxPut16(canonical + 6, sum & 65535);
    assert(WtPokemon_Encode(canonical, general + FX_DAYCARE) == 0);
}

/* Traded away in the stock game: another Pokémon takes its party slot. */
static void TradeOffered(unsigned char *general, unsigned char *boxes)
{
    FxSpec spec = FxDefault(30);
    (void)boxes;
    FxPartyMon(&spec, PartySlot(general, OFFERED_SLOT));
}

static void TwinAndTrain(unsigned char *general, unsigned char *boxes)
{
    AddTwin(general, boxes);
    TrainOffered(general, boxes);
}

/* The offer exactly as offered in the party, PC or anywhere the stock game
 * loads it from. */
static unsigned ExactCopies(void)
{
    uint32_t global, counters[2];
    int selected[2], full;
    unsigned count, i, copies = 0;
    const unsigned char *general, *boxes;
    unsigned char found[136];
    assert(FxStockLoad(&file, selected, &full, &global, counters));
    general = file.bytes + selected[0] * CP_SAVE_HALF_SIZE + FX_GENERAL;
    boxes = file.bytes + selected[1] * CP_SAVE_HALF_SIZE + FX_BOXES;
    count = Fx32(general + FX_PARTY + 4);
    for (i = 0; i < count + 18 * 30; ++i) {
        const unsigned char *slot = i < count ? general + FX_PARTY + 8 + i * FX_MON : boxes + FX_PC + (i - count) * FX_BOX_MON;
        if (WtPokemon_Decode(slot, found) == 0 && memcmp(found, offered, 136) == 0) ++copies;
    }
    return copies;
}

/* The removal cut at each write, and after the last; the stock game loads,
 * the player plays (nothing, trains the offer, deposits it, releases it,
 * finds a twin, or both) and saves once or twice. WT_INVENTORY_HISTORY writes nothing,
 * and answers 1 only when what the stock game loaded still held the offer;
 * when it did and play moved the offer from where REMOVE takes it, it always
 * answers 1. A save the removal reached, with a twin, is refused by REMOVE
 * too, untouched: never taken for the offer. */
static void History(void)
{
    static const FxStockAction actions[] = { NULL, TrainOffered, DepositOffered, ReleaseOffered, TradeOffered, AddTwin,
                                             TwinAndTrain, TwinInDayCare };
    enum { TWINS = 5 };
    static unsigned char before[CP_SAVE_FILE_SIZE];
    WtJournalEntry journal, entry;
    unsigned writes, cut, saves, action, abandoned = 0, refused = 0, held;
    int answer;
    Arrange(ESCROW, &journal);
    entry = journal; writes = file.writes;
    assert(Step(WT_INVENTORY_REMOVE, &entry) == 0);
    writes = file.writes - writes;
    /* The removal moved the save counter WT_ABANDON_STEP ahead. */
    assert(image.globalCounter - journal.globalCounter >= WT_ABANDON_STEP);
    for (cut = 1; cut <= writes + 1; ++cut) for (saves = 1; saves <= 2; ++saves)
        for (action = 0; action < sizeof(actions) / sizeof(*actions); ++action) {
            Arrange(ESCROW, &journal);
            entry = journal;
            file.failWrite = file.writes + cut;
            assert(Step(WT_INVENTORY_REMOVE, &entry) == (cut <= writes ? -1 : 0));
            file.failWrite = 0;
            held = ExactCopies();
            assert(held <= 1);
            assert(FxStockPlay(&file, saves, saves == 2, actions[action]));
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
    printf("PASS stock save after a cut removal: %u cuts x 2 stock plays x 8 plays, abandonable %u times, "
           "only from a save that held the offer; %u twins after the removal refused\n", writes + 1, abandoned, refused);
}

/* LUNE-902: the counter ranges are bounded. Fewer than WT_ABANDON_SAVES saves
 * past the escrow's counter: before the removal; WT_ABANDON_STEP to
 * WT_ABANDON_STEP + WT_ABANDON_SAVES past it: after; anything else, or a
 * counter recorded within reach of the top of its range: unproved. */
static void HistoryBounds(void)
{
    static const struct { uint32_t past; int answer; } cases[] = {
        { 1, WT_HISTORY_LEFT }, { WT_ABANDON_SAVES - 1, WT_HISTORY_LEFT }, { WT_ABANDON_SAVES, WT_HISTORY_UNPROVEN },
        { WT_ABANDON_STEP - 1, WT_HISTORY_UNPROVEN }, { WT_ABANDON_STEP, WT_HISTORY_REMOVED },
        { WT_ABANDON_STEP + WT_ABANDON_SAVES - 1, WT_HISTORY_REMOVED },
        { WT_ABANDON_STEP + WT_ABANDON_SAVES, WT_HISTORY_UNPROVEN }, { UINT32_C(0xFFFFFFFF), WT_HISTORY_UNPROVEN }
    };
    WtJournalEntry journal, entry;
    unsigned i;
    for (i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
        Arrange(ESCROW, &journal);
        assert(FxStockPlay(&file, 1, 0, TrainOffered));
        Load();
        entry = journal;
        image.globalCounter = entry.globalCounter + cases[i].past;
        image.blockCounters[0] = entry.generalCounter + 1; image.blockCounters[1] = entry.boxCounter + 1;
        assert(Step(WT_INVENTORY_HISTORY, &entry) == cases[i].answer);
    }
    /* Recorded too near the top: the removal never moved it, nothing is proved. */
    Arrange(ESCROW, &journal);
    assert(FxStockPlay(&file, 1, 0, TrainOffered));
    Load();
    entry = journal;
    entry.globalCounter = WT_ABANDON_LIMIT + 1;
    image.globalCounter = entry.globalCounter + 1;
    assert(Step(WT_INVENTORY_HISTORY, &entry) == WT_HISTORY_UNPROVEN);
    puts("PASS the save counter's ranges: before the removal, after it, and nothing proved between or past them");
}

int main(void)
{
    Sweep(ESCROW);
    Sweep(RESTORING);
    Sweep(APPLYING_RETURN);
    EscrowAfterPlay();
    SubmitReadyAfterPlay();
    IdentityCollisions();
    cases += FxHeldSweep();
    FxHeldUnique();
    History();
    HistoryBounds();
    printf("PASS stock save recovery: %u cut and stock-save cases, 0 stuck, 0 duplicates, %u lost "
        "(each cut before the first retail copy held the return, the games' window)\n", cases, windowLosses);
    return 0;
}
