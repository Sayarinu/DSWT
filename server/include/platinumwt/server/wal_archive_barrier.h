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

// Advance coverage from one pg_stat_archiver observation. The last archived
// segment covers every position up to its end only because server/wal-archive.sh
// refuses a segment whose predecessor is not already archived. PostgreSQL alone
// does not promise that order. Anything else (no archive yet, another timeline,
// a position behind what was archived) covers nothing.
WalCoverage NextWalCoverage(WalCoverage prior, const std::optional<std::string> &last_archived,
                            std::string_view current_segment, std::uint64_t segment_size);

enum class WalForce { None, Switch, RecordAndSwitch };

struct WalForceInput {
  std::uint64_t wanted = 0, covered = 0, flushed = 0, segment_size = 0;
  bool archive_failing = false;
};

// Whether a waiting request should force WAL out. A switch only helps a position
// in the open segment, and adds nothing to a backlog or a failing archive, where
// each one would pin another whole segment on disk. With no known coverage there
// may be nothing left to switch, so a small record gives the archiver a new segment.
WalForce NextWalForce(const WalForceInput &);

// Attests positions from the archive state of the connected cluster. A
// background thread keeps request threads from ever waiting on the database.
// Forcing WAL out needs EXECUTE on pg_switch_wal(); without it the barrier just
// stays closed longer. archive_command must be server/wal-archive.sh, shipping to
// independently recoverable off-site storage.
class WalArchiveBarrier {
public:
  explicit WalArchiveBarrier(std::string connection_info,
                             std::chrono::milliseconds interval = std::chrono::milliseconds(250),
                             std::chrono::milliseconds switch_interval = std::chrono::seconds(1),
                             std::chrono::milliseconds unknown_interval = std::chrono::seconds(15));
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
