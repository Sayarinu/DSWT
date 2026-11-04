#include "platinumwt/server/wal_archive_barrier.h"

#include "bounded_query.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>

namespace platinumwt::server {
namespace {

struct Segment {
  std::uint32_t timeline;
  std::uint64_t log, number;
};

std::uint32_t Hex(std::string_view text) {
  std::uint32_t value = 0;
  for (const char c : text) value = value << 4 | static_cast<std::uint32_t>(c <= '9' ? c - '0' : c - 'A' + 10);
  return value;
}

// Backup and timeline history files also appear as the last archived name.
std::optional<Segment> ParseSegment(std::string_view name) {
  if (name.size() != 24 || !std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return std::isdigit(c) || (c >= 'A' && c <= 'F');
      })) return std::nullopt;
  return Segment{Hex(name.substr(0, 8)), Hex(name.substr(8, 8)), Hex(name.substr(16, 8))};
}

// PostgreSQL's segment names count segments within 4 GiB logical files.
std::uint64_t SegmentEnd(const Segment &segment, std::uint64_t size) {
  return (segment.log * (0x100000000ULL / size) + segment.number + 1) * size;
}

constexpr auto kQuery = "SELECT a.last_archived_wal, pg_walfile_name(pg_current_wal_flush_lsn()), "
                        "pg_size_bytes(current_setting('wal_segment_size')), "
                        "(pg_current_wal_flush_lsn() - '0/0'::pg_lsn)::text, "
                        "a.last_failed_time IS NOT NULL AND "
                        "(a.last_archived_time IS NULL OR a.last_failed_time > a.last_archived_time) "
                        "FROM pg_stat_archiver a";

} // namespace

WalCoverage NextWalCoverage(WalCoverage prior, const std::optional<std::string> &last_archived,
                            std::string_view current_segment, std::uint64_t segment_size) {
  const auto current = ParseSegment(current_segment);
  if (!current || !last_archived || segment_size == 0 || (segment_size & (segment_size - 1)) != 0) return {};
  const auto limit = SegmentEnd(*current, segment_size);
  if (prior.timeline != current->timeline || prior.end > limit) prior = {};
  const auto archived = ParseSegment(*last_archived);
  if (!archived) return prior;
  if (archived->timeline != current->timeline) return {};
  const auto end = SegmentEnd(*archived, segment_size);
  if (end > limit) return {};
  return {current->timeline, std::max(prior.end, end)};
}

WalForce NextWalForce(const WalForceInput &in) {
  if (in.wanted == 0 || in.wanted <= in.covered || in.wanted > in.flushed || in.archive_failing ||
      in.segment_size == 0) return WalForce::None;
  if (in.covered == 0) return WalForce::RecordAndSwitch;
  const auto open_segment = in.flushed - in.flushed % in.segment_size;
  return in.wanted > open_segment && in.covered >= open_segment ? WalForce::Switch : WalForce::None;
}

struct WalArchiveBarrier::Impl {
  struct Shared {
    std::atomic<std::uint64_t> covered{0}, wanted{0};
  };
  std::shared_ptr<Shared> shared = std::make_shared<Shared>();
  std::atomic<bool> stopped{false};
  std::mutex mutex;
  std::condition_variable changed;
  std::thread poller;

  struct Observation {
    WalCoverage coverage;
    WalForceInput force;
  };

  Observation Observe(const std::string &database, WalCoverage coverage) const {
    const auto row = detail::BoundedQuery(database, kQuery, "platinumwt-recovery", stopped);
    if (!row || PQntuples(row.get()) != 1) return {};
    for (const int column : {1, 2, 3, 4}) if (PQgetisnull(row.get(), 0, column)) return {};
    std::optional<std::string> last;
    if (!PQgetisnull(row.get(), 0, 0)) last = PQgetvalue(row.get(), 0, 0);
    try {
      Observation observed;
      observed.force.segment_size = std::stoull(PQgetvalue(row.get(), 0, 2));
      observed.force.flushed = std::stoull(PQgetvalue(row.get(), 0, 3));
      observed.force.archive_failing = std::string_view(PQgetvalue(row.get(), 0, 4)) == "t";
      observed.coverage = NextWalCoverage(coverage, last, PQgetvalue(row.get(), 0, 1), observed.force.segment_size);
      // A history file can hide a segment archived since the last poll, so coverage may be
      // stale. Treating it as unknown still gets a waiter out of an otherwise idle segment.
      observed.force.covered = last && ParseSegment(*last) ? observed.coverage.end : 0;
      return observed;
    } catch (const std::exception &) { return {}; }
  }

  bool Execute(const std::string &database, const char *sql) const {
    return static_cast<bool>(detail::BoundedQuery(database, sql, "platinumwt-recovery", stopped));
  }

  void Run(const std::string &database, std::chrono::milliseconds interval, std::chrono::milliseconds switch_interval,
           std::chrono::milliseconds unknown_interval) {
    WalCoverage coverage;
    auto last_force = std::chrono::steady_clock::time_point{};
    bool can_force = true;
    std::unique_lock<std::mutex> lock(mutex);
    while (!stopped) {
      lock.unlock();
      auto observed = Observe(database, coverage);
      if ((observed.coverage.end == 0) != (coverage.end == 0))
        std::cerr << (observed.coverage.end ? "recovery barrier: WAL archive coverage available\n"
                                            : "recovery barrier: no WAL archive coverage\n");
      coverage = observed.coverage;
      shared->covered.store(coverage.end);
      // Only a waiting request forces WAL out, so an idle service archives nothing.
      observed.force.wanted = shared->wanted.load();
      const auto force = NextWalForce(observed.force);
      const auto now = std::chrono::steady_clock::now();
      if (force != WalForce::None &&
          now - last_force >= (force == WalForce::Switch ? switch_interval : unknown_interval)) {
        last_force = now;
        const bool forced =
            (force == WalForce::Switch || Execute(database, "SELECT pg_logical_emit_message(false,'platinumwt','')")) &&
            Execute(database, "SELECT pg_switch_wal()");
        if (can_force && !forced && !stopped) std::cerr << "recovery barrier: cannot force WAL out\n";
        can_force = forced;
      }
      lock.lock();
      changed.wait_for(lock, interval, [this] { return stopped.load(); });
    }
    shared->covered.store(0);
  }
};

WalArchiveBarrier::WalArchiveBarrier(std::string connection_info, std::chrono::milliseconds interval,
                                     std::chrono::milliseconds switch_interval,
                                     std::chrono::milliseconds unknown_interval)
    : impl_(std::make_unique<Impl>()) {
  impl_->poller = std::thread([impl = impl_.get(), database = std::move(connection_info), interval, switch_interval, unknown_interval] {
    impl->Run(database, interval, switch_interval, unknown_interval);
  });
}

WalArchiveBarrier::~WalArchiveBarrier() {
  { std::lock_guard<std::mutex> lock(impl_->mutex); impl_->stopped = true; }
  impl_->changed.notify_all();
  impl_->poller.join();
}

RecoveryBarrier WalArchiveBarrier::Barrier() const {
  return [shared = impl_->shared](std::uint64_t position) {
    const auto end = shared->covered.load();
    if (end != 0 && position <= end) return true;
    auto wanted = shared->wanted.load();
    while (wanted < position && !shared->wanted.compare_exchange_weak(wanted, position)) {}
    return false;
  };
}

} // namespace platinumwt::server
