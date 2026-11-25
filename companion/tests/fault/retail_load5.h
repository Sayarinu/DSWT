#ifndef COMPANION_FAULT_RETAIL_LOAD5_H
#define COMPANION_FAULT_RETAIL_LOAD5_H

/* LUNE-881: an independent model of Black/White and Black 2/White 2's load
 * (BACKUP_SYSTEM_LoadDoubleCopy) for the fault harness, as in
 * tests/save/gen5_save_test.c: what RAM the game builds from the card, block
 * by block, and a stock save of it. */

static unsigned Rl5Table(void) { return FX5_USED - 16 - Fx5TableSize(); }
static unsigned Rl5Offset(unsigned id) { static unsigned at[FX5_BLOCKS + 1]; if (!at[FX5_BLOCKS]) { unsigned i; for (i = 0; i < FX5_BLOCKS; ++i) at[i] = Fx5Offset(i); at[FX5_BLOCKS] = 1; } return at[id]; }

static int Rl5Footer(const unsigned char *copy)
{
    const unsigned char *f = copy + FX5_USED - 16;
    if (Fx5R32(f + 4) != FX5_USED || Fx5R32(f + 8) != CP_SAVE_MAGIC_GEN5) return 2;
    return Fx5R16(f + 14) == Fx5Crc(copy + Rl5Table(), Fx5TableSize()) ? 0 : 1;
}

static int Rl5Good(const unsigned char *copy, unsigned id, unsigned want)
{
    const unsigned char *f = copy + Rl5Offset(id) + Fx5Size_(id);
    return Fx5R16(f + 2) == want && Fx5Crc(copy + Rl5Offset(id), Fx5Size_(id)) == want;
}

/* The RAM the game loads (blocks with footers, CRC table, footer); asserts
 * the card loads at all. */
static void RetailLoad5(const unsigned char *bytes, unsigned char *ram)
{
    const unsigned char *copy[2] = { bytes, bytes + FX5_BACKUP }, *tables[2];
    int state[2] = { Rl5Footer(copy[0]), Rl5Footer(copy[1]) }, order[2], tries, id;
    uint32_t counts[2] = { Fx5R32(copy[0] + FX5_USED - 16), Fx5R32(copy[1] + FX5_USED - 16) };
    assert(!state[0] || !state[1]);
    tables[0] = state[0] ? copy[1] : copy[0];
    tables[1] = state[1] ? copy[0] : copy[1];
    if (!state[0] && (state[1] || counts[0] >= counts[1] || (counts[0] == 0 && counts[1] == UINT32_MAX))) {
        order[0] = 0; order[1] = 1;
    } else {
        order[0] = 1; order[1] = 0;
    }
    for (tries = 0; tries < 2; ++tries) {
        const unsigned char *table = tables[order[tries]] + Rl5Table();
        int ok = 1;
        for (id = 0; id < FX5_BLOCKS && ok; ++id) {
            unsigned want = Fx5R16(table + 2 * id), end = Rl5Offset((unsigned)id) + Fx5Size_((unsigned)id);
            int good[2] = { Rl5Good(copy[0], (unsigned)id, want), Rl5Good(copy[1], (unsigned)id, want) }, fromBackup;
            if (!good[0] && !good[1]) { ok = 0; break; }
            if (good[0] && good[1]) {
                unsigned c = Fx5R16(copy[order[tries]] + end);
                int mainOff = Fx5R16(copy[0] + end) != c, backupOff = Fx5R16(copy[1] + end) != c;
                if (mainOff && backupOff) { ok = 0; break; }
                fromBackup = mainOff;
            } else {
                fromBackup = !good[0];
            }
            memcpy(ram + Rl5Offset((unsigned)id), copy[fromBackup] + Rl5Offset((unsigned)id), Fx5Size_((unsigned)id) + 4);
        }
        if (ok) {
            memcpy(ram + Rl5Table(), tables[order[tries]] + Rl5Table(), Fx5TableSize() + 16);
            return;
        }
    }
    assert(!"the game would call this save broken");
}

/* A stock save after play: the RAM's play time changes, then every block is
 * written to both copies with a new count, as the game's full save does. */
static void StockSave5(unsigned char *bytes, unsigned char *ram)
{
    unsigned id;
    uint32_t count = Fx5R32(ram + FX5_USED - 16) + 1;
    ram[Rl5Offset(FX5_TRAINER) + 0x28] ^= 0x5A; /* the play time */
    for (id = 0; id < FX5_BLOCKS; ++id) {
        unsigned char *footer = ram + Rl5Offset(id) + Fx5Size_(id);
        Fx5W16(footer, Fx5R16(footer) + 1);
    }
    memcpy(bytes, ram, FX5_USED);
    memset(bytes + FX5_USED - 16, 0, 16);
    Fx5Seal((Fx5File *)(void *)bytes, 0, count);
    memcpy(bytes + FX5_BACKUP, bytes, FX5_USED);
    memcpy(ram, bytes, FX5_USED);
}

static void SelectedHash5(const unsigned char *ram, unsigned char digest[32])
{
    CpSaveSha256 hash;
    unsigned id;
    CpSaveSha256_Init(&hash);
    for (id = 0; id < FX5_BLOCKS; ++id) CpSaveSha256_Update(&hash, ram + Rl5Offset(id), Fx5Size_(id));
    CpSaveSha256_Finish(&hash, digest);
}

/* As retail_load.h Ownership, over the party, the PC and the Battle Box. */
static int Ownership(const unsigned char *bytes, const WtJournalEntry *entry,
    unsigned char digests[4][32], int played)
{
    static unsigned char ram[0x26000];
    unsigned counts[4] = { 0, 0, 0, 0 }, i, mon, escrow = 0, count;
    const unsigned char *party;
    unsigned char canonical[136], digest[32], inventory[32];
    RetailLoad5(bytes, ram);
    party = ram + Rl5Offset(FX5_PARTY);
    count = Fx5R32(party + 4);
    assert(Fx5R32(party) == 6 && count <= 6);
    for (mon = 0; mon < count + 24 * 30 + 6; ++mon) {
        const unsigned char *slot = mon < count ? party + 8 + mon * FX5_MON
            : mon < count + 720 ? ram + Rl5Offset(1 + (mon - count) / 30) + (mon - count) % 30 * FX5_BOX_MON
            : ram + Rl5Offset(FX5_BATTLE_BOX) + (mon - count - 720) * FX5_BOX_MON;
        assert(WtPokemon_Decode(slot, canonical) == 0);
        FxSha(canonical, sizeof(canonical), digest);
        for (i = 0; i < 4; ++i) if (memcmp(digest, digests[i], 32) == 0) ++counts[i];
    }
    SelectedHash5(ram, inventory);
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
