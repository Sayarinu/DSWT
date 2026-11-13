#ifndef WONDER_TRADE_BOOT_H
#define WONDER_TRADE_BOOT_H

#include "wonder_trade/game_adapter.h"
#include "wonder_trade/journal.h"

/* Boot inspection after WtRecovery_Begin, before any gameplay; run it on a
 * worker thread. Retail writes are granted, and a journal mirror repaired,
 * only when nothing is pending or the game proves a pending deposit absent.
 * Zero when gameplay may start; otherwise the caller blocks recovery. */
int WtBoot_Check(WtGame *, WtJournalIO);

/* The boot screen stays hidden while the check runs, so an ordinary boot goes
 * straight to the title. It opens when the check blocks gameplay (recovery or
 * an error) or is still running after this many frames, about 3 s at 60 Hz:
 * an enrolled save's check verifies both journal sectors several times and
 * rewrites both mirrors, which often outlasts half a second, while a blank
 * screen much past 3 s looks frozen. Hiding the check changes no gate:
 * gameplay starts only after it succeeds, and journal writes stay safe
 * against power loss. */
#define WT_BOOT_SILENT_FRAMES 180

#endif
