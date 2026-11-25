#ifndef COMPANION_TEST_GEN4_HELD_H
#define COMPANION_TEST_GEN4_HELD_H

/* LUNE-913: the stock game moves the offered Pokémon where no journal locator
 * reaches: the Day Care, the GTS and (HeartGold/SoulSilver) the Pokéwalker.
 * Layouts from the decomps' save tables (adapter/platinum.c Held); each place
 * is written the way the game writes it. The including test supplies:
 * - file, image and offered (the offer's canonical image);
 * - HeldArrange(returning, journal): an escrow of party slot 1 (returning 0),
 *   or that offer, rejected after a game save, prepared as a return (1);
 * - HeldStep(step, entry), HeldCopies() (exact copies in the party and PC);
 * - HeldStock(action): the stock game loads, runs action on its general
 *   block, saves once, and the image is loaded again. */

typedef void (*FxHeldAction)(unsigned char *general);

typedef struct FxPlace {
    const char *name;
    /* The image's offset, the u16 flag's (0: none) and the image's size. */
    unsigned mon, flag, size;
} FxPlace;

static const FxPlace kFxPlaces[] = {
    { "left at the Day Care (first slot)", FX_DAYCARE, 0, FX_BOX_MON },
    { "left at the Day Care (second slot)", FX_DAYCARE + FX_DAYCARE_SLOT, 0, FX_BOX_MON },
    { "deposited at the GTS", FX_GTS, FX_GTS_FLAG, FX_MON },
#if FX_GAME == 2
    { "taken on a Pokéwalker walk", FX_WALKER_MON, 0, FX_BOX_MON },
#endif
};
enum { FX_PLACES = sizeof(kFxPlaces) / sizeof(kFxPlaces[0]) };
static unsigned fxPlace, fxChanged;
/* The party image that went in, which a withdrawal with no steps rebuilds. */
static unsigned char fxHeldParty[FX_MON];

static unsigned char *FxHeldSlot(unsigned char *general, unsigned slot) { return general + FX_PARTY + 8 + slot * FX_MON; }

/* The offered Pokémon's party slot (by identity), or the count. */
static unsigned FxHeldOfferedSlot(unsigned char *general)
{
    unsigned char canonical[136];
    unsigned count = Fx32(general + FX_PARTY + 4), slot;
    for (slot = 0; slot < count; ++slot) {
        assert(WtPokemon_Decode(FxHeldSlot(general, slot), canonical) == 0);
        if (memcmp(canonical, offered, 4) == 0 && memcmp(canonical + 12, offered + 12, 4) == 0) break;
    }
    return slot;
}

/* From the party into the place: the Day Care and the Pokéwalker keep the box
 * part, the GTS the party image and sets pokemonListed. fxChanged: the image
 * changed on the way (as HeartGold/SoulSilver's Day Care clears the mood). */
static void FxHoldOffered(unsigned char *general)
{
    const FxPlace *at = &kFxPlaces[fxPlace];
    unsigned count = Fx32(general + FX_PARTY + 4), slot = FxHeldOfferedSlot(general), i;
    unsigned char *to = general + at->mon;
    if (slot == count) return;
    memcpy(fxHeldParty, FxHeldSlot(general, slot), FX_MON);
    memcpy(to, fxHeldParty, at->size);
    if (fxChanged) {
        unsigned char canonical[136];
        unsigned sum = 0;
        assert(WtPokemon_Decode(to, canonical) == 0);
        canonical[0x30] = 35; /* a move's PP */
        for (i = 8; i < 136; i += 2) sum += canonical[i] | (canonical[i + 1] << 8);
        FxPut16(canonical + 6, sum & 65535);
        assert(WtPokemon_Encode(canonical, to) == 0);
        assert(WtPokemon_Encode(canonical, fxHeldParty) == 0); /* and so comes back */
    }
    if (at->flag) FxPut16(general + at->flag, 1);
    for (i = slot; i + 1 < count; ++i) {
        memcpy(FxHeldSlot(general, i), FxHeldSlot(general, i + 1), FX_MON);
#if FX_GAME == 2
        memcpy(general + FX_APRIJUICE + 5 * i, general + FX_APRIJUICE + 5 * (i + 1), 5);
#endif
    }
    FxEmpty(FxHeldSlot(general, count - 1), FX_MON);
#if FX_GAME == 2
    memset(general + FX_APRIJUICE + 5 * (count - 1), 0, 5);
#endif
    FxPut32(general + FX_PARTY + 4, count - 1);
}

/* Taken back to the end of the party. The Day Care re-inits its slot, the
 * Pokéwalker zeroes it, the GTS only clears pokemonListed. */
static void FxWithdrawOffered(unsigned char *general)
{
    const FxPlace *at = &kFxPlaces[fxPlace];
    unsigned count = Fx32(general + FX_PARTY + 4);
    assert(count < 6);
    memcpy(FxHeldSlot(general, count), fxHeldParty, FX_MON);
    FxPut32(general + FX_PARTY + 4, count + 1);
    if (at->flag) FxPut16(general + at->flag, 0);
#if FX_GAME == 2
    else if (at->mon == FX_WALKER_MON) memset(general + at->mon, 0, FX_BOX_MON);
#endif
    else FxEmpty(general + at->mon, FX_BOX_MON);
}

/* Whether the loaded save holds the offered Pokémon there. */
static int FxHeld(void)
{
    const FxPlace *at = &kFxPlaces[fxPlace];
    const unsigned char *general = CpSaveImage_Block(&image, 0);
    unsigned char canonical[136];
    if (at->flag && !(general[at->flag] | general[at->flag + 1])) return 0;
    if (WtPokemon_Decode(general + at->mon, canonical) != 0) return 0;
    return memcmp(canonical, offered, 4) == 0 && memcmp(canonical + 12, offered + 12, 4) == 0;
}

/* Every cut of REMOVE in escrow and of RECEIVE for a return, then the stock
 * game moves the offered Pokémon there when the save it loaded holds it: the
 * step refuses and writes nothing, so it is neither sent nor added twice.
 * Taken back unchanged, the step finishes once; changed, it stays refused. */
static unsigned FxHeldSweep(void)
{
    static unsigned char before[CP_SAVE_FILE_SIZE];
    WtJournalEntry journal, entry;
    unsigned cut, held, writes, returning, cases = 0;
    for (fxPlace = 0; fxPlace < FX_PLACES; ++fxPlace) for (fxChanged = 0; fxChanged <= 1; ++fxChanged)
    for (returning = 0; returning <= 1; ++returning) {
        enum WtInventoryStep cutStep = returning ? WT_INVENTORY_RECEIVE : WT_INVENTORY_REMOVE;
        HeldArrange((int)returning, &journal);
        entry = journal; writes = file.writes;
        assert(HeldStep(cutStep, &entry) == 0);
        writes = file.writes - writes;
        held = 0;
        for (cut = 1; cut <= writes + 1; ++cut) {
            HeldArrange((int)returning, &journal);
            entry = journal;
            file.failWrite = file.writes + cut;
            assert(HeldStep(cutStep, &entry) == (cut <= writes ? -1 : 0));
            file.failWrite = 0;
            HeldStock(FxHoldOffered);
            entry = journal;
            ++cases;
            if (FxHeld()) {
                ++held;
                memcpy(before, file.bytes, sizeof(before));
                assert(HeldStep(cutStep, &entry) == -1);
                assert(memcmp(before, file.bytes, sizeof(before)) == 0 && HeldCopies() == 0);
                HeldStock(FxWithdrawOffered);
                entry = journal;
                if (fxChanged) {
                    memcpy(before, file.bytes, sizeof(before));
                    assert(HeldStep(cutStep, &entry) == -1 && memcmp(before, file.bytes, sizeof(before)) == 0);
                    continue;
                }
            }
            assert(HeldStep(cutStep, &entry) == 0);
            if (!returning) assert(HeldStep(WT_INVENTORY_VERIFY_REMOVED, &entry) == 0 && HeldCopies() == 0);
            else assert(HeldStep(WT_INVENTORY_VERIFY_RECEIVED, &entry) == 0 && HeldCopies() == 1);
            assert(!FxHeld());
        }
        assert(held >= 1);
        printf("PASS %s stock save in %s, then %s%s: refused untouched in %u of %u cuts, %s once taken back\n",
            FX_GAME_NAME, returning ? "applying a return (RECEIVE)" : "escrow (REMOVE)", kFxPlaces[fxPlace].name,
            fxChanged ? " (changed)" : "", held, writes + 1, fxChanged ? "still refused" : "finished");
    }
    return cases;
}

/* The offer screen: a Pokémon there with the offered one's identity makes it
 * not unique; the GTS's old image after pokemonListed clears does not. */
static void FxHeldUnique(void)
{
    WtJournalEntry journal;
    for (fxPlace = 0; fxPlace < FX_PLACES; ++fxPlace) {
        HeldArrange(0, &journal); /* a fresh save; the escrow is not used */
        fxChanged = 0;
        HeldStock(NULL);
        assert(CpAdapter_Unique(&image, 1));
        {
            unsigned char *general = CpSaveImage_Block(&image, 0), digest[32];
            memcpy(general + kFxPlaces[fxPlace].mon, FxHeldSlot(general, 1), kFxPlaces[fxPlace].size);
            if (kFxPlaces[fxPlace].flag) {
                assert(CpAdapter_Unique(&image, 1));
                FxPut16(general + kFxPlaces[fxPlace].flag, 1);
            }
            assert(!CpAdapter_Unique(&image, 1));
            assert(CpSaveImage_Hash(&image, digest) == CP_SAVE_OK);
        }
        printf("PASS %s offer screen: a copy %s is not unique%s\n", FX_GAME_NAME, kFxPlaces[fxPlace].name,
            kFxPlaces[fxPlace].flag ? "; its old image once taken back is ignored" : "");
    }
}

#endif
