#ifndef COMPANION_TEST_GEN5_STOCK_H
#define COMPANION_TEST_GEN5_STOCK_H

/* LUNE-881: an independent model of Black/White and Black 2/White 2's load
 * (BACKUP_SYSTEM_LoadDoubleCopy), as in tests/save/gen5_save_test.c: the RAM
 * the game builds from the card, block by block, and a stock save of it. */
#include "gen5_fixture.h"

static inline unsigned Rl5Table(void) { return FX5_USED - 16 - Fx5TableSize(); }
static inline unsigned Rl5Offset(unsigned id) { static unsigned at[FX5_BLOCKS + 1]; if (!at[FX5_BLOCKS]) { unsigned i; for (i = 0; i < FX5_BLOCKS; ++i) at[i] = Fx5Offset(i); at[FX5_BLOCKS] = 1; } return at[id]; }

static inline int Rl5Footer(const unsigned char *copy)
{
    const unsigned char *f = copy + FX5_USED - 16;
    if (Fx5R32(f + 4) != FX5_USED || Fx5R32(f + 8) != CP_SAVE_MAGIC_GEN5) return 2;
    return Fx5R16(f + 14) == Fx5Crc(copy + Rl5Table(), Fx5TableSize()) ? 0 : 1;
}

static inline int Rl5Good(const unsigned char *copy, unsigned id, unsigned want)
{
    const unsigned char *f = copy + Rl5Offset(id) + Fx5Size_(id);
    return Fx5R16(f + 2) == want && Fx5Crc(copy + Rl5Offset(id), Fx5Size_(id)) == want;
}

/* The RAM the game loads (blocks with footers, CRC table, footer); asserts
 * the card loads at all. */
static inline void RetailLoad5(const unsigned char *bytes, unsigned char *ram)
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

/* A stock save after play: the RAM's play time advances (it never comes back
 * to an earlier value), then every block is written to both copies with a new
 * count, as the game's full save does. */
static inline void StockSave5(unsigned char *bytes, unsigned char *ram)
{
    unsigned id;
    uint32_t count = Fx5R32(ram + FX5_USED - 16) + 1;
    { /* the play time */
        unsigned char *time = ram + Rl5Offset(FX5_TRAINER) + 0x28;
        Fx5W16(time, Fx5R16(time) + 1);
    }
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

/* LUNE-906: on the RAM the stock game loaded, the player lets go of the
 * Pokémon with the target's identity: released (the party closes up behind
 * it, a PC or Battle Box slot empties) or, with trade, traded away for another
 * Pokémon (Fx5Default(40)) that takes its slot. 0 when the RAM does not hold it. */
static inline int Fx5Gone(unsigned char *ram, const unsigned char target[136], int trade)
{
    unsigned char canonical[136], *party = ram + Rl5Offset(FX5_PARTY);
    Fx5Spec other = Fx5Default(40);
    unsigned count = Fx5R32(party + 4), mon, i;
    for (mon = 0; mon < count + 720 + 6; ++mon) {
        unsigned char *at = mon < count ? party + 8 + mon * FX5_MON
            : mon < count + 720 ? ram + Rl5Offset(1 + (mon - count) / 30) + (mon - count) % 30 * FX5_BOX_MON
            : ram + Rl5Offset(FX5_BATTLE_BOX) + (mon - count - 720) * FX5_BOX_MON;
        if (WtPokemon_Decode(at, canonical) != 0 || !Fx5R16(canonical + 8)
            || memcmp(canonical, target, 4) != 0 || memcmp(canonical + 12, target + 12, 4) != 0) continue;
        if (mon >= count) {
            if (trade) { Fx5Canonical(&other, canonical); assert(WtPokemon_Encode(canonical, at) == 0); }
            else Fx5Empty(at, FX5_BOX_MON);
            return 1;
        }
        if (trade) { Fx5PartyMon(&other, at); return 1; }
        for (i = mon; i + 1 < count; ++i) memcpy(party + 8 + i * FX5_MON, party + 8 + (i + 1) * FX5_MON, FX5_MON);
        Fx5Empty(party + 8 + (count - 1) * FX5_MON, FX5_MON);
        Fx5W32(party + 4, count - 1);
        return 1;
    }
    return 0;
}

static inline void SelectedHash5(const unsigned char *ram, unsigned char digest[32])
{
    CpSaveSha256 hash;
    unsigned id;
    CpSaveSha256_Init(&hash);
    for (id = 0; id < FX5_BLOCKS; ++id) CpSaveSha256_Update(&hash, ram + Rl5Offset(id), Fx5Size_(id));
    CpSaveSha256_Finish(&hash, digest);
}

#endif
