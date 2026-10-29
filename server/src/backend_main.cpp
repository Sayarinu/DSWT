#include "platinumwt/server/config.h"
#include "platinumwt/server/identity_service.h"
#include "platinumwt/server/legality_client.h"
#include "platinumwt/server/trade_service.h"
#include "admission.h"
#include "platinumwt/server/metrics_server.h"

#include <boost/asio.hpp>
#include <grpcpp/grpcpp.h>

#include <csignal>
#include <exception>
#include <iostream>

int main() {
  try {
    using namespace platinumwt::server;
    const auto config = LoadBackendConfig();
    Telemetry::Global().Initialize(TelemetryRole::Backend);
    MetricsServer metrics(config.metrics_port, config.connection_info);
    LegalityClient legality(config.legality_address, config.legality_policy);
    // No production attestation bypass. Until the independently recoverable
    // prefix provider is integrated, enrollment/operations remain PROCESSING.
    auto admission = std::make_shared<detail::ServiceAdmission>();
    IdentityService identities(config.connection_info, {}, admission);
    TradeService trades(config.connection_info, [&](const auto &data) { return legality.CheckNewSubmission(data); }, {}, admission);
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
    signals.async_wait([&](boost::system::error_code, int) {
      server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(3));
      io.stop();
    });
    io.run();
    return 0;
  } catch (const std::exception &) {
    std::cerr << "trade backend unavailable; check private configuration\n";
    return 1;
  }
}
