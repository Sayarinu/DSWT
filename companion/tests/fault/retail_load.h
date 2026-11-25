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

static void SelectedHash(const unsigned char *bytes, const unsigned selected[2], unsigned char digest[32])
{
    CpSaveSha256 hash;
    unsigned block;
    CpSaveSha256_Init(&hash);
    for (block = 0; block < 2; ++block)
        CpSaveSha256_Update(&hash, bytes + selected[block] * CP_SAVE_HALF_SIZE + blockOffsets[block],
            blockSizes[block] - 20);
    CpSaveSha256_Finish(&hash, digest);
}

/* Journal images retained after a save transition are replay evidence, not
 * another owned Pokémon. ESCROW/RESTORING/APPLYING choose ownership by the
 * recorded before/after inventory hashes; published phases require one side.
 * After playing changes those hashes, transitional escrow is retained only for
 * an absent original; Resume also rejects any engine stuck in those phases. */
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
            : boxes + FX_PC + (mon - count) * FX_BOX_MON;
        assert(WtPokemon_Decode(slot, canonical) == 0);
        FxSha(canonical, sizeof(canonical), digest);
        for (i = 0; i < 4; ++i) if (memcmp(digest, digests[i], 32) == 0) ++counts[i];
    }
    SelectedHash(bytes, selected, inventory);
    switch (entry->phase) {
    case WT_JOURNAL_ESCROW: case WT_JOURNAL_RESTORING:
        if (played) { escrow = counts[1] == 0; break; }
        assert(memcmp(inventory, entry->inventoryBefore, 32) == 0
            || memcmp(inventory, entry->inventoryAfter, 32) == 0);
        escrow = memcmp(inventory, entry->inventoryAfter, 32) == 0;
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
    for (i = 0; i < 4; ++i) if (counts[i] + (i == 1 ? escrow : 0) > 1) return 1;
    for (i = 0; i < 4; ++i) if (counts[i] + (i == 1 ? escrow : 0) < 1) return -1;
    return 0;
}

static void Conservation(const unsigned char *bytes, const WtJournalEntry *entry,
    unsigned char digests[4][32])
{
    assert(Ownership(bytes, entry, digests, 0) == 0);
}

#endif
