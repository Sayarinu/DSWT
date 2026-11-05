#pragma once

#include "dswt/server/recovery_barrier.h"

#include <mutex>
#include <string>
#include <unordered_map>

namespace dswt::server::detail {

class Database;

// Call only after observing committed state. A newer generation must encompass
// all earlier generations of this key. Eviction delays visibility, never safety.
class RecoveryGate {
public:
  explicit RecoveryGate(RecoveryBarrier provider);
  bool Covers(Database &database, const std::string &key, std::uint64_t generation);

private:
  RecoveryBarrier provider_;
  std::mutex mutex_;
  std::unordered_map<std::string, std::pair<std::uint64_t, std::uint64_t>> entries_;
};

} // namespace dswt::server::detail
