#include "recovery_gate.h"

#include "postgres.h"
#include "trade_encoding.h"

#include <utility>

namespace dswt::server::detail {

RecoveryGate::RecoveryGate(RecoveryBarrier provider) : provider_(std::move(provider)) {}

bool RecoveryGate::Covers(Database &database, const std::string &key, std::uint64_t generation) {
  if (!provider_) return false;
  auto row = database.Execute("SELECT (pg_current_wal_flush_lsn() - '0/0'::pg_lsn)::text");
  auto position = Unsigned(PQgetvalue(row.get(), 0, 0));
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto existing = entries_.find(key);
    if (existing != entries_.end() && existing->second.first >= generation) position = existing->second.second;
    else {
      if (existing == entries_.end() && entries_.size() >= 4096) entries_.erase(entries_.begin());
      entries_[key] = {generation, position};
    }
  }
  return provider_(position);
}

} // namespace dswt::server::detail
