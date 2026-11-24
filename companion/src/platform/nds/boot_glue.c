/* The core symbols boot.c, recovery and the entropy pool need from the
 * platform (wonder_trade/flash.h, wonder_trade/game_adapter.h).
 *
 * The platform-only ones are implemented here. The inventory, checkpoint,
 * leave and hold checks are the save adapter's (adapter/platinum.c). Showing a
 * received Pokémon needs game data (LUNE-873); until then it is a stub that
 * fails safely. */
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

/* STUB (LUNE-873): no received Pokémon to show. */
int WtGame_Received(const WtJournalEntry *entry, WtReceivedPokemon *received)
{
    (void)entry;
    if (received) memset(received, 0, sizeof(*received));
    return -1;
}
