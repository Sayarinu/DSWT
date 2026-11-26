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
    assert(writes == 12); /* two passes over two blocks: body, footer tail, footer */
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
    for (cut = 1; cut <= 13; cut += 12) {
        Arrange(ESCROW, &journal);
        entry = journal;
        file.failWrite = file.writes + cut;
        assert(Step(WT_INVENTORY_REMOVE, &entry) == (cut <= 12 ? -1 : 0));
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
    printf("PASS stock save recovery: %u cut and stock-save cases, 0 stuck, 0 duplicates, %u lost "
        "(each cut before the first retail copy held the return, the games' window)\n", cases, windowLosses);
    return 0;
}
