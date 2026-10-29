#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>

#include <grpcpp/server_context.h>

namespace platinumwt::server {

enum class TelemetryRole { Gateway, Backend };
enum class Operation : unsigned { Invalid, Enroll, Submit, Status, Cancel, Collect, Acknowledge, Reconcile, Match, Legality, Count };

struct LedgerMetrics {
  bool up = false;
  std::uint64_t queued = 0, pending = 0, matched = 0, completed = 0, acknowledged = 0, unknown_queue_ages = 0, unknown_delivery_ages = 0;
  double oldest_queue_seconds = 0, oldest_delivery_seconds = 0;
};

// Only fixed operation/result labels and explicit bounded IDs enter telemetry.
// Ownership evidence remains in PostgreSQL, never in this process's counters/logs.
class Telemetry {
public:
  Telemetry();
  static Telemetry &Global();
  void Initialize(TelemetryRole role);
  void Record(Operation, unsigned result, std::chrono::microseconds elapsed,
              std::string_view request_id = {}, std::string_view operation_id = {}, std::string_view trade_id = {}) noexcept;
  void Replay() noexcept { replays_.fetch_add(1, std::memory_order_relaxed); }
  void Refused() noexcept { refused_.fetch_add(1, std::memory_order_relaxed); }
  void SetLedger(LedgerMetrics);
  std::string Render() const;
  static std::string LogLine(Operation, unsigned result, std::uint64_t elapsed_us,
                             std::string_view request_id, std::string_view operation_id, std::string_view trade_id,
                             TelemetryRole role = TelemetryRole::Backend);

private:
  static constexpr unsigned kOperations = static_cast<unsigned>(Operation::Count), kResults = 12, kBuckets = 10;
  std::array<std::array<std::atomic<std::uint64_t>, kResults>, kOperations> requests_{};
  std::array<std::array<std::atomic<std::uint64_t>, kBuckets + 1>, kOperations> latency_{};
  std::array<std::atomic<std::uint64_t>, kOperations> elapsed_us_{};
  std::atomic<std::uint64_t> replays_{0}, dropped_logs_{0}, refused_{0};
  std::atomic<bool> logs_{false};
  TelemetryRole role_ = TelemetryRole::Backend;
  mutable std::mutex ledger_mutex_, log_mutex_;
  LedgerMetrics ledger_;
  double last_ledger_success_ = 0;
};

// The reply object outlives this scope. No protobuf body/credential is formatted.
class RpcTelemetry {
public:
  RpcTelemetry(Operation, grpc::ServerContext *, std::function<unsigned()> result,
               std::string_view operation_id = {}, std::string_view trade_id = {}, std::string_view request_id = {}) noexcept;
  ~RpcTelemetry();
  RpcTelemetry(const RpcTelemetry &) = delete;
  RpcTelemetry &operator=(const RpcTelemetry &) = delete;
  void Trade(std::string_view id) noexcept { try { if (id.size() == 16) trade_id_.assign(id); } catch (...) {} }
private:
  Operation operation_;
  std::function<unsigned()> result_;
  std::chrono::steady_clock::time_point begin_ = std::chrono::steady_clock::now();
  std::string request_id_, operation_id_, trade_id_;
};

} // namespace platinumwt::server
