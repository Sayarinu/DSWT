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
                        "pg_size_bytes(current_setting('wal_segment_size')) FROM pg_stat_archiver a";

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

struct WalArchiveBarrier::Impl {
  struct Shared {
    std::atomic<std::uint64_t> covered{0}, wanted{0};
  };
  std::shared_ptr<Shared> shared = std::make_shared<Shared>();
  std::atomic<bool> stopped{false};
  std::mutex mutex;
  std::condition_variable changed;
  std::thread poller;

  WalCoverage Observe(const std::string &database, WalCoverage coverage) const {
    const auto row = detail::BoundedQuery(database, kQuery, "platinumwt-recovery", stopped);
    if (!row || PQntuples(row.get()) != 1 || PQgetisnull(row.get(), 0, 1) || PQgetisnull(row.get(), 0, 2)) return {};
    std::optional<std::string> last;
    if (!PQgetisnull(row.get(), 0, 0)) last = PQgetvalue(row.get(), 0, 0);
    try {
      return NextWalCoverage(coverage, last, PQgetvalue(row.get(), 0, 1), std::stoull(PQgetvalue(row.get(), 0, 2)));
    } catch (const std::exception &) { return {}; }
  }

  void Run(const std::string &database, std::chrono::milliseconds interval, std::chrono::milliseconds switch_interval) {
    WalCoverage coverage;
    auto last_switch = std::chrono::steady_clock::time_point{};
    bool can_switch = true;
    std::unique_lock<std::mutex> lock(mutex);
    while (!stopped) {
      lock.unlock();
      const auto next = Observe(database, coverage);
      if ((next.end == 0) != (coverage.end == 0))
        std::cerr << (next.end ? "recovery barrier: WAL archive coverage available\n"
                               : "recovery barrier: no WAL archive coverage\n");
      coverage = next;
      shared->covered.store(coverage.end);
      // Only a waiting request forces a segment out, so an idle service archives nothing.
      const auto now = std::chrono::steady_clock::now();
      if (shared->wanted.load() > coverage.end && now - last_switch >= switch_interval) {
        last_switch = now;
        const bool switched = static_cast<bool>(
            detail::BoundedQuery(database, "SELECT pg_switch_wal()", "platinumwt-recovery", stopped));
        if (can_switch && !switched && !stopped) std::cerr << "recovery barrier: cannot switch WAL segments\n";
        can_switch = switched;
      }
      lock.lock();
      changed.wait_for(lock, interval, [this] { return stopped.load(); });
    }
    shared->covered.store(0);
  }
};

WalArchiveBarrier::WalArchiveBarrier(std::string connection_info, std::chrono::milliseconds interval,
                                     std::chrono::milliseconds switch_interval)
    : impl_(std::make_unique<Impl>()) {
  impl_->poller = std::thread([impl = impl_.get(), database = std::move(connection_info), interval, switch_interval] {
    impl->Run(database, interval, switch_interval);
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
