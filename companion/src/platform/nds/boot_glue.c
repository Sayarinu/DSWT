/* The core symbols boot.c, recovery and the entropy pool need from the
 * platform (wonder_trade/flash.h, wonder_trade/game_adapter.h).
 *
 * The platform-only ones are implemented here. The inventory and game
 * checks need the save image and game data (LUNE-871, LUNE-873); until then
 * they are stubs that fail safely: nothing can be held, nothing is proven
 * absent, so WtBoot_Check blocks any journal with work pending instead of
 * granting it. */
#include <stdlib.h>
#include <string.h>

#include <nds.h>
#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#include <mbedtls/private/sha256.h>

#include "wonder_trade/flash.h"
#include "wonder_trade/game_adapter.h"

int WtFlash_Hash(const unsigned char *bytes, size_t length, unsigned char digest[32])
{
    /* Stack-only hashing works before the TLS allocator starts, as in the game. */
    return mbedtls_sha256(bytes, length, digest, 0);
}

/* The companion's WtGame holds no heap of its own: the C heap serves. */
void *WtGame_Alloc(WtGame *game, size_t size)
{
    (void)game;
    return malloc(size);
}

void WtGame_Free(WtGame *game, void *pointer)
{
    (void)game;
    free(pointer);
}

/* Closing the lid must not suspend a journal or save write half way. */
void WtGame_HoldAwake(int hold)
{
    if (hold) disableSleep();
    else enableSleep();
}

void WtGame_SampleInput(WtEntropySample *sample)
{
    static uint32_t frame;
    touchPosition touch;
    uint32_t keys = keysHeld();
    sample->frame = ++frame;
    sample->keys = keys;
    sample->touching = (keys & KEY_TOUCH) != 0;
    if (sample->touching) {
        touchRead(&touch);
        sample->x = touch.px;
        sample->y = touch.py;
    }
}

/* STUB (LUNE-871): party and PC access on the save image. */
int WtGame_Inventory(void *game, enum WtInventoryStep step, WtJournalEntry *entry, unsigned slot)
{
    (void)game; (void)step; (void)entry; (void)slot;
    return -1;
}

/* STUB (LUNE-871): make both save halves match before a deposit. */
int WtGame_Checkpoint(WtGame *game)
{
    (void)game;
    return -1;
}

/* STUB (LUNE-871): nothing is proven, so boot blocks pending work. */
int WtGame_CanLeave(WtGame *game, const WtJournalEntry *entry)
{
    (void)game; (void)entry;
    return 0;
}

/* STUB (LUNE-873): refuse every delivery; it stays pending on the server. */
int WtGame_CanHold(void *game, const unsigned char pokemon[WT_POKEMON_SIZE])
{
    (void)game; (void)pokemon;
    return 0;
}

/* STUB (LUNE-871): no received Pokémon to show. */
int WtGame_Received(const WtJournalEntry *entry, WtReceivedPokemon *received)
{
    (void)entry;
    if (received) memset(received, 0, sizeof(*received));
    return -1;
}
