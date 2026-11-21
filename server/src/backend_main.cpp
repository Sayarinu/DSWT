#include "dswt/server/config.h"
#include "dswt/server/identity_service.h"
#include "dswt/server/legality_client.h"
#include "dswt/server/offer_screener.h"
#include "dswt/server/trade_service.h"
#include "dswt/server/wal_archive_barrier.h"
#include "admission.h"
#include "dswt/server/metrics_server.h"

#include <boost/asio.hpp>
#include <grpcpp/grpcpp.h>

#include <atomic>
#include <condition_variable>
#include <csignal>
#include <exception>
#include <iostream>
#include <mutex>
#include <thread>

int main() {
  try {
    using namespace dswt::server;
    const auto config = LoadBackendConfig();
    Telemetry::Global().Initialize(TelemetryRole::Backend);
    MetricsServer metrics(config.metrics_port, config.connection_info);
    LegalityClient legality(config.legality_address, config.legality_policy);
    // No attestation bypass: enrollment/operations stay PROCESSING until the
    // cluster's WAL archive covers them.
    WalArchiveBarrier recovery(config.connection_info);
    auto admission = std::make_shared<detail::ServiceAdmission>(config.submission_workers);
    IdentityService identities(config.connection_info, recovery.Barrier(), admission);
    const SubmissionValidator validate = [&](const auto &data, auto format) { return legality.CheckNewSubmission(data, format); };
    TradeService trades(config.connection_info, validate, recovery.Barrier(), admission);
    OfferScreener screener(config.connection_info, validate);
    grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort(config.address, grpc::InsecureServerCredentials(), &port);
    builder.SetMaxReceiveMessageSize(4096);
    builder.SetMaxSendMessageSize(4096);
    grpc::ResourceQuota resources;
    resources.SetMaxThreads(32).Resize(16 * 1024 * 1024);
    builder.SetResourceQuota(resources);
    builder.SetSyncServerOption(grpc::ServerBuilder::NUM_CQS, 2);
    builder.SetSyncServerOption(grpc::ServerBuilder::MIN_POLLERS, 2);
    builder.SetSyncServerOption(grpc::ServerBuilder::MAX_POLLERS, 8);
    builder.RegisterService(&identities);
    builder.RegisterService(&trades);
    auto server = builder.BuildAndStart();
    if (!server || port == 0) throw std::runtime_error("cannot start private trade backend");
    std::cout << "Trade backend listening on loopback port " << port << std::endl;
    boost::asio::io_context io;
    boost::asio::signal_set signals(io, SIGINT, SIGTERM);
    std::mutex mutex;
    std::condition_variable changed;
    std::atomic<bool> stopping{false};
    const auto stop = [&] {
      { std::lock_guard<std::mutex> lock(mutex); stopping = true; }
      changed.notify_all();
      server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(3));
      io.stop();
    };
    signals.async_wait([&](boost::system::error_code, int) { stop(); });
    // Screen queued offers that predate the current name rule; idle when none remain.
    std::thread screening([&] {
      std::unique_lock<std::mutex> lock(mutex);
      while (!stopping) {
        lock.unlock();
        std::size_t screened = 0;
        try { screened = screener.Poll(16, &stopping); } catch (const std::exception &) {}
        lock.lock();
        changed.wait_for(lock, std::chrono::seconds(screened ? 0 : 10), [&] { return stopping.load(); });
      }
    });
    io.run();
    screening.join();
    return 0;
  } catch (const std::exception &) {
    std::cerr << "trade backend unavailable; check private configuration\n";
    return 1;
  }
}
