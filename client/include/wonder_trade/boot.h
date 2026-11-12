#ifndef WONDER_TRADE_BOOT_H
#define WONDER_TRADE_BOOT_H

#include "wonder_trade/game_adapter.h"
#include "wonder_trade/journal.h"

/* Boot inspection after WtRecovery_Begin, before any gameplay; run it on a
 * worker thread. Retail writes are granted, and a journal mirror repaired,
 * only when nothing is pending or the game proves a pending deposit absent.
 * Zero when gameplay may start; otherwise the caller blocks recovery. */
int WtBoot_Check(WtGame *, WtJournalIO);

#endif
