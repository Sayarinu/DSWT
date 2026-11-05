#include "platinumwt/server/config.h"
#include "platinumwt/server/match_worker.h"
#include "platinumwt/server/matcher_service.h"
#include "platinumwt/server/metrics_server.h"
#include "platinumwt/server/telemetry.h"
#include "platinumwt/server/wal_archive_barrier.h"

#include <boost/asio.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

constexpr std::chrono::milliseconds kIdleDelay{500};

// Pairs queued offers. This process opens no matcher RPC port: workers call the
// service directly, so nothing on the network can reach matching.
int main() {
  try {
    using namespace platinumwt::server;
    const auto config = LoadMatcherConfig();
    Telemetry::Global().Initialize(TelemetryRole::Matcher);
    MetricsServer metrics(config.metrics_port);
    WalArchiveBarrier recovery(config.connection_info);
    MatcherService matcher(config.connection_info, recovery.Barrier());

    boost::asio::io_context io;
    boost::asio::signal_set signals(io, SIGINT, SIGTERM);
    std::mutex mutex;
    std::condition_variable changed;
    bool stopping = false;
    std::atomic<bool> failed{false};
    const auto stop = [&] {
      { std::lock_guard<std::mutex> lock(mutex); stopping = true; }
      changed.notify_all();
      io.stop();
    };
    signals.async_wait([&](boost::system::error_code, int) { stop(); });

    std::vector<std::thread> workers;
    for (unsigned index = 0; index < config.workers; ++index)
      workers.emplace_back([&] {
        try {
          MatchWorker worker([&](const auto &request, auto *reply) { return matcher.MatchNext(nullptr, &request, reply); });
          std::unique_lock<std::mutex> lock(mutex);
          while (!stopping) {
            lock.unlock();
            // Empty polls would retain an attempt row and WAL forever on an idle service.
            const auto delay = worker.Pending() || matcher.PairAvailable() ? worker.Poll() : kIdleDelay;
            lock.lock();
            changed.wait_for(lock, delay, [&] { return stopping; });
          }
        } catch (const std::exception &) {
          // A failed worker stops the process rather than silently reducing matching capacity.
          failed = true;
          stop();
        }
      });
    std::cout << "Trade matcher running with " << config.workers << " worker(s)" << std::endl;
    io.run();
    for (auto &worker : workers) worker.join();
    if (failed) {
      std::cerr << "trade matcher worker failed\n";
      return 1;
    }
    return 0;
  } catch (const std::exception &) {
    std::cerr << "trade matcher unavailable; check private configuration\n";
    return 1;
  }
}
