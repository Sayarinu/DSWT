#ifndef COMPANION_FAULT_RETAIL_LOAD5_H
#define COMPANION_FAULT_RETAIL_LOAD5_H

/* LUNE-881: who owns the offered Pokémon on a Black/White or Black 2/White 2
 * card, by the independent load model of adapter/gen5_stock.h. */
#include "../adapter/gen5_stock.h"

/* As retail_load.h: LUNE-902's changed offer, as offered or trained. */
/* goneByPlay: the player released or traded the offer away from a save that
 * held it, so it may be in neither the save nor the journal. */
static int changedOffer, savedOver, goneByPlay;
static unsigned char trainedDigest[32];
/* LUNE-913: the Day Care's two slots, the Dream World and the GTS: block,
 * party image offset, flag offset and size (adapter/gen5.c Held). */
typedef struct Fx5HeldPlace { unsigned block, mon, flag, flagSize; } Fx5HeldPlace;
static const Fx5HeldPlace kFx5Held[] = {
    { FX5_DAYCARE, 0x04, 0x00, 4 }, { FX5_DAYCARE, FX5_DAYCARE_SLOT + 0x04, FX5_DAYCARE_SLOT, 4 },
    { FX5_DREAM_WORLD, 0x08, 0x1A3, 1 }, { FX5_GTS, 0x00, 0xEC, 2 }
};
enum { FX5_HELD_PLACES = sizeof(kFx5Held) / sizeof(kFx5Held[0]) };

static int Fx5HeldOccupied(const unsigned char *ram, unsigned place)
{
    const unsigned char *flag = ram + Rl5Offset(kFx5Held[place].block) + kFx5Held[place].flag;
    return kFx5Held[place].flagSize == 1 ? flag[0] != 0 : kFx5Held[place].flagSize == 2 ? Fx5R16(flag) != 0
        : Fx5R32(flag) != 0;
}

/* LUNE-906: as retail_load.h fxOffer. */
enum { FX_OFFER_ONE, FX_OFFER_GONE, FX_OFFER_WINDOW };
static unsigned fxOffer;

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
        for (i = 0; i < 4; ++i) {
            if (memcmp(digest, digests[i], 32) == 0) ++counts[i];
            else if (i == 1 && changedOffer && memcmp(digest, trainedDigest, 32) == 0) ++counts[1];
        }
    }
    played = played || savedOver;
    /* LUNE-913: and where no locator reaches, while each place's flag is set. */
    for (mon = 0; mon < FX5_HELD_PLACES; ++mon) {
        if (!Fx5HeldOccupied(ram, mon)) continue;
        assert(WtPokemon_Decode(ram + Rl5Offset(kFx5Held[mon].block) + kFx5Held[mon].mon, canonical) == 0);
        FxSha(canonical, sizeof(canonical), digest);
        for (i = 0; i < 4; ++i) if (memcmp(digest, digests[i], 32) == 0) ++counts[i];
    }
    SelectedHash5(ram, inventory);
    switch (entry->phase) {
    case WT_JOURNAL_ESCROW: case WT_JOURNAL_RESTORING:
        if (played) { escrow = counts[1] == 0; break; }
        {
            unsigned char before[32];
            assert(WtEngine_EscrowInventoryBefore(entry, Hash, before) >= 0);
            assert(memcmp(inventory, before, 32) == 0 || memcmp(inventory, entry->inventoryAfter, 32) == 0);
        }
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
