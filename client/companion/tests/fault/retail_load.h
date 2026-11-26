#ifndef COMPANION_FAULT_RETAIL_LOAD_H
#define COMPANION_FAULT_RETAIL_LOAD_H

/* Independent CompareSectors/LoadCheck oracle, following LUNE-866's
 * save_image_test.c. CORRUPT still selects blocks in the retail game. */
static const unsigned blockOffsets[2] = { FX_GENERAL, FX_BOXES };
static const unsigned blockSizes[2] = { FX_GENERAL_SIZE, FX_BOXES_SIZE };

static int RetailCompare(uint32_t a, uint32_t b)
{
    if (a == UINT32_MAX && b == 0) return -1;
    if (a == 0 && b == UINT32_MAX) return 1;
    return (a > b) - (a < b);
}

#if FX_SHARED
/* LUNE-880: HeartGold/SoulSilver's Save_GetSaveFilesStatus. One counter per
 * half; the newer half when its blocks agree, else the older one. */
static void RetailLoad(const unsigned char *bytes, unsigned selected[2])
{
    unsigned block, half, valid[2] = { 0, 0 }, newer, older, chosen = 2;
    uint32_t counts[2][2];
    for (block = 0; block < 2; ++block) {
        for (half = 0; half < 2; ++half) {
            const unsigned char *body = bytes + half * CP_SAVE_HALF_SIZE + blockOffsets[block];
            const unsigned char *footer = body + blockSizes[block] - FX_FOOTER;
            if (Fx32(footer + 4) == blockSizes[block] && Fx32(footer + 8) == CP_SAVE_MAGIC_INTERNATIONAL
                && (unsigned)(footer[12] | footer[13] << 8) == block
                && (unsigned)(footer[14] | footer[15] << 8) == FxCrc(body, blockSizes[block] - FX_FOOTER))
                valid[block] |= 1u << half;
            counts[block][half] = valid[block] >> half & 1 ? Fx32(footer) : 0;
        }
        assert(valid[block] != 0);
    }
    newer = valid[0] == 3 ? RetailCompare(counts[0][0], counts[0][1]) < 0 : valid[0] == 2;
    older = valid[0] == 3 ? !newer : 2;
    if (valid[0] != 3 && valid[1] != 3) {
        if ((valid[0] & valid[1]) != 0) chosen = newer;
    } else if (counts[0][newer] == counts[1][newer]) chosen = newer;
    else if (older != 2 && counts[0][older] == counts[1][older]) chosen = older;
    assert(chosen != 2 && (valid[0] & valid[1] & (1u << chosen)));
    selected[0] = selected[1] = chosen;
}
#else
static void RetailLoad(const unsigned char *bytes, unsigned selected[2])
{
    unsigned block, half, valid[2] = { 0, 0 };
    uint32_t globals[2][2], counters[2][2];
    for (block = 0; block < 2; ++block) {
        for (half = 0; half < 2; ++half) {
            const unsigned char *body = bytes + half * CP_SAVE_HALF_SIZE + blockOffsets[block];
            const unsigned char *footer = body + blockSizes[block] - 20;
            unsigned crc = footer[18] | ((unsigned)footer[19] << 8);
            globals[block][half] = Fx32(footer); counters[block][half] = Fx32(footer + 4);
            if (Fx32(footer + 8) == blockSizes[block]
                && Fx32(footer + 12) == CP_SAVE_MAGIC_INTERNATIONAL
                && footer[16] == block && crc == FxCrc(body, blockSizes[block] - 20))
                valid[block] |= 1u << half;
        }
        assert(valid[block] != 0);
        if (valid[block] == 3) {
            int global = RetailCompare(globals[block][0], globals[block][1]);
            int counter = RetailCompare(counters[block][0], counters[block][1]);
            selected[block] = (global ? global : counter) < 0;
        } else selected[block] = valid[block] == 2;
    }
    if (globals[0][selected[0]] == globals[1][selected[1]]) return;
    if (valid[0] == 3) selected[0] ^= 1;
    else if (valid[1] == 3) {
        selected[1] ^= 1;
        assert(globals[0][selected[0]] == globals[1][selected[1]]);
    }
}
#endif

/* LUNE-913: the places a Gen 4 save holds a Pokémon outside the party and
 * PC that play can fill (adapter/platinum.c Held): the box part (or party
 * image) offset in the general block and the u16 flag (0: none). */
typedef struct FxHeldPlace { unsigned mon, flag, size; } FxHeldPlace;
static const FxHeldPlace kFxHeld[] = {
    { FX_DAYCARE, 0, FX_BOX_MON }, { FX_DAYCARE + FX_DAYCARE_SLOT, 0, FX_BOX_MON }, { FX_GTS, FX_GTS_FLAG, FX_MON },
#if FX_GAME == 2
    { FX_WALKER_MON, 0, FX_BOX_MON },
#endif
};
enum { FX_HELD_PLACES = sizeof(kFxHeld) / sizeof(kFxHeld[0]) };

static int FxHeldOccupied(const unsigned char *general, unsigned place)
{
    static const unsigned char zeros[FX_BOX_MON];
    unsigned char canonical[136];
    const unsigned char *slot = general + kFxHeld[place].mon;
    if (kFxHeld[place].flag) return (general[kFxHeld[place].flag] | general[kFxHeld[place].flag + 1]) != 0;
    if (memcmp(slot, zeros, sizeof(zeros)) == 0) return 0;
    assert(WtPokemon_Decode(slot, canonical) == 0);
    return (canonical[8] | canonical[9]) != 0;
}

static void SelectedHash(const unsigned char *bytes, const unsigned selected[2], unsigned char digest[32])
{
    CpSaveSha256 hash;
    unsigned block;
    CpSaveSha256_Init(&hash);
    for (block = 0; block < 2; ++block) {
        const unsigned char *body = bytes + selected[block] * CP_SAVE_HALF_SIZE + blockOffsets[block];
#if FX_SHARED
        /* The PC's modified flag reads as zero (hgss/game_save.c Update). */
        static const unsigned char zero[4] = { 0, 0, 0, 0 };
        if (block == 1) {
            CpSaveSha256_Update(&hash, body, 0x12004);
            CpSaveSha256_Update(&hash, zero, 4);
            CpSaveSha256_Update(&hash, body + 0x12008, blockSizes[block] - FX_FOOTER - 0x12008);
            continue;
        }
#endif
        CpSaveSha256_Update(&hash, body, blockSizes[block] - FX_FOOTER);
    }
    CpSaveSha256_Finish(&hash, digest);
}

/* LUNE-902: scenarios where the stock game changes the offered Pokémon
 * (digests[1]) count it as offered or as trained (trainedDigest), never a
 * twin that only shares its identity; once the stock game saved over an
 * escrow (savedOver), the save is neither of its inventories. The journal
 * keeps the companion's escrow inventoryBefore masked (engine.c). */
/* goneByPlay: the player released or traded the offer away from a save that
 * held it, so it may be in neither the save nor the journal. */
static int changedOffer, savedOver, goneByPlay;
static unsigned char trainedDigest[32];

/* Journal images retained after a save transition are replay evidence, not
 * another owned Pokémon. ESCROW/RESTORING/APPLYING choose ownership by the
 * recorded before/after inventory hashes; published phases require one side.
 * After playing changes those hashes, transitional escrow is retained only for
 * an absent original; Resume also rejects any engine stuck in those phases. */
/* LUNE-906: what Ownership expects of the offered Pokémon (digests[1]):
 * exactly one copy, none (released or traded away in the stock game), or none
 * or one (a return in flight the stock game saved over before any retail copy
 * held it: the games' window). */
enum { FX_OFFER_ONE, FX_OFFER_GONE, FX_OFFER_WINDOW };
static unsigned fxOffer;

static int Ownership(const unsigned char *bytes, const WtJournalEntry *entry,
    unsigned char digests[4][32], int played)
{
    unsigned selected[2], counts[4] = { 0, 0, 0, 0 }, i, mon, escrow = 0;
    const unsigned char *general, *boxes;
    unsigned char canonical[136], digest[32], inventory[32];
    RetailLoad(bytes, selected);
    general = bytes + selected[0] * CP_SAVE_HALF_SIZE;
    boxes = bytes + selected[1] * CP_SAVE_HALF_SIZE + FX_BOXES;
    assert(Fx32(general + FX_PARTY) == 6 && Fx32(general + FX_PARTY + 4) <= 6);
    for (mon = 0; mon < Fx32(general + FX_PARTY + 4) + 540; ++mon) {
        unsigned count = Fx32(general + FX_PARTY + 4);
        const unsigned char *slot = mon < count ? general + FX_PARTY + 8 + mon * FX_MON
            : FxPcSlotConst(boxes, mon - count);
        assert(WtPokemon_Decode(slot, canonical) == 0);
        FxSha(canonical, sizeof(canonical), digest);
        for (i = 0; i < 4; ++i) {
            if (memcmp(digest, digests[i], 32) == 0) ++counts[i];
            else if (i == 1 && changedOffer && memcmp(digest, trainedDigest, 32) == 0) ++counts[1];
        }
    }
    played = played || savedOver;
    /* LUNE-913: and where no locator reaches: the Day Care, the GTS while
     * pokemonListed, the Pokéwalker. */
    for (mon = 0; mon < FX_HELD_PLACES; ++mon) {
        const unsigned char *slot = general + kFxHeld[mon].mon;
        if (!FxHeldOccupied(general, mon)) continue;
        assert(WtPokemon_Decode(slot, canonical) == 0);
        FxSha(canonical, sizeof(canonical), digest);
        for (i = 0; i < 4; ++i) if (memcmp(digest, digests[i], 32) == 0) ++counts[i];
    }
    SelectedHash(bytes, selected, inventory);
    switch (entry->phase) {
    case WT_JOURNAL_ESCROW: case WT_JOURNAL_RESTORING:
        if (played) { escrow = counts[1] == 0; break; }
        {
            unsigned char before[32], after[32];
            assert(WtEngine_EscrowInventories(entry, Hash, before, after) >= 0);
            assert(memcmp(inventory, before, 32) == 0 || memcmp(inventory, after, 32) == 0);
            escrow = memcmp(inventory, after, 32) == 0;
        }
        break;
    case WT_JOURNAL_SUBMIT_READY: case WT_JOURNAL_QUEUED: case WT_JOURNAL_RECEIVED:
        escrow = 1;
        break;
    case WT_JOURNAL_APPLYING:
        if (played) { escrow = counts[1] == 0; break; }
        assert(memcmp(inventory, entry->inventoryBefore, 32) == 0
            || memcmp(inventory, entry->inventoryAfter, 32) == 0);
        escrow = memcmp(inventory, entry->inventoryBefore, 32) == 0;
        break;
    case WT_JOURNAL_EMPTY: case WT_JOURNAL_ENROLLING: case WT_JOURNAL_IDLE: case WT_JOURNAL_APPLIED:
        break;
    default: assert(0);
    }
    if (escrow) {
        assert(WtPokemon_Decode(entry->original, canonical) == 0);
        assert(memcmp(canonical, entry->outgoing, 136) == 0);
        FxSha(canonical, sizeof(canonical), digest);
        assert(memcmp(digest, digests[1], 32) == 0);
    }
    /* LUNE-906 (fxOffer): the offer let go in the stock game must stay gone;
     * in the games' window it may be lost, never doubled. */
    for (i = 0; i < 4; ++i)
        if (counts[i] + (i == 1 ? escrow : 0) > (i == 1 && fxOffer == FX_OFFER_GONE ? 0u : 1u)) return 1;
    for (i = 0; i < 4; ++i)
        if (counts[i] + (i == 1 ? escrow : 0) < (i == 1 && (fxOffer != FX_OFFER_ONE || goneByPlay) ? 0u : 1u)) return -1;
    return 0;
}

static void Conservation(const unsigned char *bytes, const WtJournalEntry *entry,
    unsigned char digests[4][32])
{
    assert(Ownership(bytes, entry, digests, 0) == 0);
}

#endif
