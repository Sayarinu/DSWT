#pragma once

#include "platinumwt/server/recovery_barrier.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace platinumwt::server {

// WAL known to be archived on one timeline. An end of zero covers nothing.
struct WalCoverage {
  std::uint32_t timeline = 0;
  std::uint64_t end = 0;
};

// Advance coverage from one pg_stat_archiver observation. PostgreSQL archives
// segments in order and records one only after archive_command succeeds, so the
// last archived segment covers every position up to its end. Anything that does
// not prove that (no archive yet, another timeline, a position behind what was
// archived) covers nothing.
WalCoverage NextWalCoverage(WalCoverage prior, const std::optional<std::string> &last_archived,
                            std::string_view current_segment, std::uint64_t segment_size);

// Attests positions from the archive state of the connected cluster. A
// background thread keeps request threads from ever waiting on the database, and
// forces the current segment out while a request waits on it. That needs
// EXECUTE on pg_switch_wal(); without it the barrier just stays closed longer.
// archive_command defines what "archived" means, so production must point it at
// independently recoverable off-site storage.
class WalArchiveBarrier {
public:
  explicit WalArchiveBarrier(std::string connection_info,
                             std::chrono::milliseconds interval = std::chrono::milliseconds(250),
                             std::chrono::milliseconds switch_interval = std::chrono::seconds(1));
  ~WalArchiveBarrier();
  WalArchiveBarrier(const WalArchiveBarrier &) = delete;
  WalArchiveBarrier &operator=(const WalArchiveBarrier &) = delete;
  // The returned barrier stays valid, and closed, after this object stops.
  RecoveryBarrier Barrier() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace platinumwt::server
