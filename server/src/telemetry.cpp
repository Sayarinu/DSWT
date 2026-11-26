#include "hex_digits.h"
#include "dswt/server/wire_layout.h"
#include "dswt/server/telemetry.h"

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <iomanip>
#include <locale>
#include <sstream>
#include <thread>

namespace dswt::server {
namespace {
constexpr int kMetricPrecision = 12;
constexpr const char *kOperationNames[] = {"invalid", "enroll", "submit", "status", "cancel", "collect", "acknowledge", "reconcile", "match", "legality"};
constexpr const char *kResultNames[] = {"transport_error", "ok", "processing", "invalid_request", "unauthenticated", "conflict", "not_found",
                                  "capacity", "illegal_pokemon", "counter_mismatch", "cursor_ahead", "unavailable"};
constexpr std::uint64_t kBounds[] = {5000, 10000, 25000, 50000, 100000, 250000, 500000, 1000000, 2000000, 5000000};
static_assert(sizeof(kOperationNames) / sizeof(*kOperationNames) == static_cast<unsigned>(Operation::Count));

unsigned Index(Operation operation) {
  const auto value = static_cast<unsigned>(operation);
  return value < static_cast<unsigned>(Operation::Count) ? value : 0;
}

std::string HexId(std::string_view bytes) {
  if (bytes.size() != wire::kIdSize || std::all_of(bytes.begin(), bytes.end(), [](char byte) { return byte == 0; })) return {};
  constexpr char digits[] = "0123456789abcdef";
  std::string value(2 * wire::kIdSize, '0');
  for (unsigned i = 0; i < wire::kIdSize; ++i) {
    const auto byte = static_cast<unsigned char>(bytes[i]);
    value[i * 2] = digits[byte >> kNibbleBits]; value[i * 2 + 1] = digits[byte & kNibbleMask];
  }
  return value;
}
} // namespace

// Lines wait here for the writer thread. A full queue means the output is stuck, and only
// then is a line dropped; request threads never wait on stderr.
struct LogQueue {
  static constexpr std::size_t kCapacity = 8192;
  std::mutex mutex;
  std::condition_variable wake, idle;
  std::deque<std::string> lines;
  bool stopping = false, writing = false;
  std::function<bool(std::string_view)> sink = [](std::string_view line) {
    return std::fwrite(line.data(), 1, line.size(), stderr) == line.size();
  };
  std::atomic<std::uint64_t> dropped{0};

  bool Push(std::string line) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (lines.size() >= kCapacity) return false;
      lines.push_back(std::move(line));
    }
    wake.notify_one();
    return true;
  }

  // Owns the queue through a shared pointer and runs detached, so a stuck pipe cannot hang shutdown.
  static void Drain(std::shared_ptr<LogQueue> queue) {
    std::unique_lock<std::mutex> lock(queue->mutex);
    for (;;) {
      queue->wake.wait(lock, [&] { return queue->stopping || !queue->lines.empty(); });
      if (queue->lines.empty()) return;
      std::deque<std::string> batch;
      batch.swap(queue->lines);
      auto sink = queue->sink;
      queue->writing = true;
      lock.unlock();
      for (const auto &line : batch)
        if (!sink(line)) queue->dropped.fetch_add(1, std::memory_order_relaxed);
      lock.lock();
      queue->writing = false;
      queue->idle.notify_all();
    }
  }
};

Telemetry &Telemetry::Global() { static Telemetry instance; return instance; }

Telemetry::Telemetry() : log_queue_(std::make_shared<LogQueue>()) {
  for (auto &operation : requests_) for (auto &counter : operation) counter.store(0);
  for (auto &operation : latency_) for (auto &counter : operation) counter.store(0);
  for (auto &counter : elapsed_us_) counter.store(0);
}

Telemetry::~Telemetry() {
  {
    std::lock_guard<std::mutex> lock(log_queue_->mutex);
    log_queue_->stopping = true;
  }
  log_queue_->wake.notify_all();
}

void Telemetry::SetLogSinkForTesting(std::function<bool(std::string_view)> sink) {
  std::lock_guard<std::mutex> lock(log_queue_->mutex);
  log_queue_->sink = std::move(sink);
}

void Telemetry::FlushLogsForTesting() {
  std::unique_lock<std::mutex> lock(log_queue_->mutex);
  log_queue_->idle.wait(lock, [&] { return log_queue_->lines.empty() && !log_queue_->writing; });
}

static const char *RoleName(TelemetryRole role) {
  return role == TelemetryRole::Gateway ? "gateway" : role == TelemetryRole::Matcher ? "matcher" : "backend";
}

void Telemetry::Initialize(TelemetryRole role) {
  role_ = role;
  if (!logs_.exchange(true, std::memory_order_acq_rel)) std::thread(LogQueue::Drain, log_queue_).detach();
}

std::string Telemetry::LogLine(Operation operation, unsigned result, std::uint64_t elapsed,
                              std::string_view request, std::string_view mutation, std::string_view trade, TelemetryRole role) {
  std::ostringstream output;
  output.imbue(std::locale::classic());
  output << "{\"event\":\"trade_request\",\"service\":\"" << RoleName(role) << "\",\"timestamp_ms\":"
         << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()
         << ",\"operation\":\"" << kOperationNames[Index(operation)]
         << "\",\"result\":\"" << kResultNames[result < std::size(kResultNames) ? result : 0] << "\",\"duration_us\":" << elapsed;
  for (const auto &field : {std::pair{"request_id", request}, std::pair{"operation_id", mutation}, std::pair{"trade_id", trade}}) {
    const auto identifier = HexId(field.second);
    if (!identifier.empty()) output << ",\"" << field.first << "\":\"" << identifier << '"';
  }
  output << "}\n";
  return output.str();
}

void Telemetry::Record(Operation operation, unsigned result, std::chrono::microseconds elapsed,
                       std::string_view request, std::string_view mutation, std::string_view trade) noexcept {
  const auto op = Index(operation);
  result = result < kResults ? result : 0;
  const auto duration = static_cast<std::uint64_t>(std::max<std::int64_t>(0, elapsed.count()));
  requests_[op][result].fetch_add(1, std::memory_order_relaxed);
  elapsed_us_[op].fetch_add(duration, std::memory_order_relaxed);
  unsigned bucket = 0;
  while (bucket < kBuckets && duration > kBounds[bucket]) ++bucket;
  latency_[op][bucket].fetch_add(1, std::memory_order_relaxed);
  if (!logs_.load(std::memory_order_acquire)) return;
  try {
    if (!log_queue_->Push(LogLine(operation, result, duration, request, mutation, trade, role_)))
      dropped_logs_.fetch_add(1, std::memory_order_relaxed);
  } catch (...) { dropped_logs_.fetch_add(1, std::memory_order_relaxed); }
}

void Telemetry::SetLedger(LedgerMetrics metrics) {
  std::lock_guard<std::mutex> lock(ledger_mutex_);
  ledger_ = metrics;
  if (metrics.up) last_ledger_success_ = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string Telemetry::Render() const {
  std::ostringstream output;
  output.imbue(std::locale::classic());
  output << std::setprecision(kMetricPrecision);
  const char *role = RoleName(role_);
  output << "# HELP dswt_requests_total Completed API/legality attempts; not committed trades.\n# TYPE dswt_requests_total counter\n";
  for (unsigned op = 0; op < kOperations; ++op) {
    const std::string labels = std::string("service=\"") + role + "\",operation=\"" + kOperationNames[op] + '"';
    for (unsigned result = 0; result < kResults; ++result)
      output << "dswt_requests_total{" << labels << ",result=\"" << kResultNames[result] << "\"} " << requests_[op][result].load() << '\n';
  }
  output << "# HELP dswt_request_duration_seconds API/legality latency excluding matchmaking wait.\n# TYPE dswt_request_duration_seconds histogram\n";
  for (unsigned op = 0; op < kOperations; ++op) {
    const std::string labels = std::string("service=\"") + role + "\",operation=\"" + kOperationNames[op] + '"';
    std::uint64_t count = 0;
    for (unsigned bucket = 0; bucket < kBuckets; ++bucket) {
      count += latency_[op][bucket].load();
      output << "dswt_request_duration_seconds_bucket{" << labels << ",le=\"" << kBounds[bucket] / static_cast<double>(std::micro::den) << "\"} " << count << '\n';
    }
    count += latency_[op][kBuckets].load();
    output << "dswt_request_duration_seconds_bucket{" << labels << ",le=\"+Inf\"} " << count << '\n';
    output << "dswt_request_duration_seconds_count{" << labels << "} " << count << '\n';
    output << "dswt_request_duration_seconds_sum{" << labels << "} " << elapsed_us_[op].load() / static_cast<double>(std::micro::den) << '\n';
  }
  output << "# TYPE dswt_operation_replays_total counter\ndswt_operation_replays_total{service=\"" << role << "\"} " << replays_.load() << '\n';
  output << "# TYPE dswt_dropped_logs_total counter\ndswt_dropped_logs_total{service=\"" << role << "\"} " << dropped_logs_.load() + log_queue_->dropped.load() << '\n';
  output << "# TYPE dswt_admission_rejections_total counter\ndswt_admission_rejections_total{service=\"" << role << "\"} " << refused_.load() << '\n';
  if (role_ == TelemetryRole::Backend) {
    std::lock_guard<std::mutex> lock(ledger_mutex_);
    for (const auto &value : {std::pair{"database_up", static_cast<double>(ledger_.up)},
        std::pair{"ledger_sample_success_timestamp_seconds", last_ledger_success_},
        std::pair{"queued_offers", static_cast<double>(ledger_.queued)}, std::pair{"pending_deliveries", static_cast<double>(ledger_.pending)},
        std::pair{"matched_pairs", static_cast<double>(ledger_.matched)}, std::pair{"unknown_queue_ages", static_cast<double>(ledger_.unknown_queue_ages)},
        std::pair{"completed_pairs", static_cast<double>(ledger_.completed)}, std::pair{"acknowledged_offers", static_cast<double>(ledger_.acknowledged)},
        std::pair{"unknown_delivery_ages", static_cast<double>(ledger_.unknown_delivery_ages)}}) {
      output << "# TYPE dswt_" << value.first << " gauge\ndswt_" << value.first << ' ';
      if (!ledger_.up && std::string_view(value.first) != "database_up" && std::string_view(value.first) != "ledger_sample_success_timestamp_seconds") output << "NaN\n";
      else output << value.second << '\n';
    }
    for (const auto &value : {std::pair{"oldest_queue_age_seconds", std::pair{ledger_.oldest_queue_seconds, ledger_.unknown_queue_ages}},
                              std::pair{"oldest_delivery_age_seconds", std::pair{ledger_.oldest_delivery_seconds, ledger_.unknown_delivery_ages}}}) {
      output << "# TYPE dswt_" << value.first << " gauge\ndswt_" << value.first << ' ';
      if (!ledger_.up || value.second.second) output << "NaN\n"; else output << value.second.first << '\n';
    }
  }
  return output.str();
}

RpcTelemetry::RpcTelemetry(Operation operation, grpc::ServerContext *context, std::function<unsigned()> result,
                           std::string_view mutation, std::string_view trade, std::string_view request) noexcept
    : operation_(operation), result_(std::move(result)) {
  try {
    if (mutation.size() == wire::kIdSize) operation_id_.assign(mutation);
    if (trade.size() == wire::kIdSize) trade_id_.assign(trade);
    if (request.size() == wire::kIdSize) request_id_.assign(request);
    if (context) {
      const auto found = context->client_metadata().find("x-dswt-request-id-bin");
      if (found != context->client_metadata().end() && found->second.size() == wire::kIdSize) request_id_.assign(found->second.data(), wire::kIdSize);
    }
  } catch (...) {}
}

RpcTelemetry::~RpcTelemetry() {
  try {
    Telemetry::Global().Record(operation_, result_(), std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - begin_),
                               request_id_, operation_id_, trade_id_);
  } catch (...) {}
}

} // namespace dswt::server
