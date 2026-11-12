#ifndef WONDER_TRADE_GAME_ADAPTER_H
#define WONDER_TRADE_GAME_ADAPTER_H

#include <stddef.h>
#include <stdint.h>

#include "wonder_trade/engine.h"
#include "wonder_trade/entropy.h"

/* The shared core (engine, wire, journal, flash, TLS, HTTP, recovery and the
 * network worker) uses only the C library, Mbed TLS and the DS SDK. Everything
 * it needs from a particular game goes through this interface; a port to
 * another Generation IV game implements it and its own menu, nothing else.
 *
 * The game also calls the core: WonderTradeEntropy_Init once at boot,
 * WonderTradeEntropy_Frame once per main-loop frame, and the WtRecovery gates
 * around every retail save write or erase and before its first gameplay
 * overlay (wonder_trade/recovery.h). Before gameplay can start, its boot
 * screen runs WtBoot_Check (wonder_trade/boot.h); its Wonder Trade screen
 * drives wonder_trade/network.h.
 *
 * Inventory, checkpoint, leave and received calls run on a worker thread
 * while the Wonder Trade screen holds the game; the core never runs two of
 * them at once. Other functions are called from the main thread unless noted. */

/* The game's save data and the heap for temporary allocations, defined by the
 * adapter. The core only passes it back. */
typedef struct WtGame WtGame;

#define WT_GAME_NAME_LENGTH 10
#define WT_GAME_TRAINER_NAME_LENGTH 7

/* What the player received, for display only. Names use the game's 16-bit
 * character encoding and end with its terminator. */
typedef struct WtReceivedPokemon {
    uint16_t species;
    uint8_t gender, form, shiny;
    uint32_t personality;
    int hasNickname;
    uint16_t nickname[WT_GAME_NAME_LENGTH + 1], originalTrainer[WT_GAME_TRAINER_NAME_LENGTH + 1];
} WtReceivedPokemon;

typedef struct WtGameConfig {
    /* Flash addresses of the two 4 KiB journal banks. Each starts a 64 KiB
     * sector of the 4 Mbit save flash that the game never uses. */
    uint32_t journalBank[2];
    /* Wi-Fi access point DMA channel and power mode the game's SDK use expects. */
    uint8_t wifiDma, wifiPowerMode;
} WtGameConfig;

extern const WtGameConfig gWtGameConfig;

/* Memory from the game's heap; NULL when exhausted. */
void *WtGame_Alloc(WtGame *, size_t);
void WtGame_Free(WtGame *, void *);

/* Load the game's Wi-Fi code and start its wireless driver. The core polls
 * Ready before connecting and before Shutdown, then waits until Active is
 * false before it releases the Wi-Fi code. */
void WtGame_WirelessStart(void);
int WtGame_WirelessReady(void);
void WtGame_WirelessShutdown(void);
int WtGame_WirelessActive(void);
void WtGame_WirelessRelease(void);

/* Keep the console awake (nonzero) while a journal bank is written. Any thread. */
void WtGame_HoldAwake(int);

/* Fill frame, keys, x, y and touching from this frame's input. Leaves low
 * untouched. Called from WonderTradeEntropy_Frame. */
void WtGame_SampleInput(WtEntropySample *);

/* The engine's inventory callback (WtEngineIO.inventory): party and PC access,
 * including trade evolution and other receive effects, with both retail save
 * copies written and verified. The argument is the WtGame. */
int WtGame_Inventory(void *, enum WtInventoryStep, WtJournalEntry *, unsigned slot);
/* Make both retail copies match the live save before a deposit. Zero on success. */
int WtGame_Checkpoint(WtGame *);
/* Nonzero when gameplay may resume with this journal entry: nothing is
 * pending, or the outgoing Pokémon is provably absent from both copies. */
int WtGame_CanLeave(WtGame *, const WtJournalEntry *);
/* The engine's receive check (WtEngineIO.canHold), on a matched delivery's
 * canonical PK4 before anything is recorded or imported: nonzero when this
 * game can hold it. Check species, form, held item and ball against the game's
 * own limits, typically through WtPokemon_Fits (wonder_trade/pokemon_codec.h).
 * Zero leaves the trade pending on the server and the save unchanged, so a
 * later build that accepts it can still collect it. The argument is the WtGame. */
int WtGame_CanHold(void *, const unsigned char pk4[136]);
/* Read the delivered image as saved, including any trade evolution. Zero on success. */
int WtGame_Received(const WtJournalEntry *, WtReceivedPokemon *);

#endif
