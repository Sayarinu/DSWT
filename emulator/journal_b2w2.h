#pragma once

#include "wonder_trade/journal.h"

// The journal core built for Black 2/White 2 (its Pokémon sizes and box count),
// with its functions renamed in CMakeLists.txt so it links beside the
// Generation IV build.
extern "C" {
int WtJournalB2W2_Open(WtJournal *, WtJournalIO);
int WtJournalB2W2_Read(const WtJournal *, WtJournalEntry *);
int WtJournalB2W2_Commit(WtJournal *, const WtJournalEntry *);
void WtJournalB2W2_Close(WtJournal *);
}
