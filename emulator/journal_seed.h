#pragma once

#include <string>

namespace dswt {
// Read-only preflight before creating a recovery namespace for a save copy.
void InspectJournal(const std::string &path, const std::string &target);
// Requires an existing checked host store; retains every non-entropy field.
void RefreshJournalSeed(const std::string &path, const std::string &target);
}
