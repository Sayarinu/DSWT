#pragma once

#include "wonder_trade/journal.h"

// The journal core built for Black/White (its Pokémon sizes and box count),
// with its functions renamed in CMakeLists.txt so it links beside the other
// families' builds.
extern "C" {
int WtJournalBW_Open(WtJournal *, WtJournalIO);
int WtJournalBW_Read(const WtJournal *, WtJournalEntry *);
int WtJournalBW_Commit(WtJournal *, const WtJournalEntry *);
void WtJournalBW_Close(WtJournal *);
}
