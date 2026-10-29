// Synthetic-only backend for the real HTTPS/ledger integration scenario.
// This target is not copied into runtime images and grants no production bypass.
#include "platinumwt/server/identity_service.h"
#include "platinumwt/server/trade_service.h"
#include "platinumwt/server/telemetry.h"
#include "admission.h"

#include <boost/asio.hpp>
#include <grpcpp/grpcpp.h>

#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <thread>

namespace platinumwt::server {
namespace {
void Mark(const char *message) {
  static std::mutex output;
  std::lock_guard<std::mutex> lock(output);
  std::cout << message << std::endl;
}

class TestTrades final : public v1::WonderTrade::Service {
public:
  explicit TestTrades(std::shared_ptr<detail::ServiceAdmission> admission, std::string release)
      : trades("", [release = std::move(release)](const auto &data) {
          if (data[8] == 2) {
            Mark("TEST_VALIDATION_STARTED");
            std::this_thread::sleep_for(std::chrono::milliseconds(350));
          }
          if (data[8] == 4 && !release.empty()) {
            Mark("TEST_VALIDATION_HELD");
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (!std::filesystem::exists(release) && std::chrono::steady_clock::now() < deadline)
              std::this_thread::sleep_for(std::chrono::milliseconds(5));
          }
          if (!release.empty() && std::filesystem::exists(release + ".outage"))
            return LegalityDecision{LegalityVerdict::Unavailable, {}};
          return LegalityDecision{data[8] == 3 ? LegalityVerdict::Illegal : LegalityVerdict::Legal, {}};
        }, [](auto) { return true; }, std::move(admission)) {}
  TradeService trades;

  grpc::Status Submit(grpc::ServerContext *context, const v1::SubmitRequest *request, v1::MutationReply *reply) override {
    return trades.Submit(context, request, reply);
  }
  grpc::Status Cancel(grpc::ServerContext *context, const v1::CancelRequest *request, v1::MutationReply *reply) override {
    return trades.Cancel(context, request, reply);
  }
  grpc::Status Collect(grpc::ServerContext *context, const v1::CollectRequest *request, v1::CollectReply *reply) override {
    return trades.Collect(context, request, reply);
  }
  grpc::Status Acknowledge(grpc::ServerContext *context, const v1::AcknowledgeRequest *request, v1::MutationReply *reply) override {
    return trades.Acknowledge(context, request, reply);
  }
  grpc::Status Reconcile(grpc::ServerContext *context, const v1::ReconcileRequest *request, v1::ReconcileReply *reply) override {
    return trades.Reconcile(context, request, reply);
  }
  grpc::Status Status(grpc::ServerContext *context, const v1::StatusRequest *request, v1::StatusReply *reply) override {
    if (request->trade_id() == std::string(16, '\xF6')) {
      auto changed = *request;
      changed.clear_trade_id();
      return trades.Status(context, &changed, reply);
    }
    if (request->trade_id() == std::string(16, '\xF0')) {
      Mark("TEST_WAIT_STARTED");
      while (!context->IsCancelled()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
      Mark("TEST_WAIT_CANCELLED");
      return {grpc::StatusCode::CANCELLED, "synthetic cancellation"};
    }
    if (request->trade_id() == std::string(16, '\xF1') || request->trade_id() == std::string(16, '\xF2') ||
        request->trade_id() == std::string(16, '\xF3') || request->trade_id() == std::string(16, '\xF4') ||
        request->trade_id() == std::string(16, '\xF5')) {
      reply->mutable_context()->set_result(v1::RESULT_OK);
      reply->mutable_context()->set_next_counter(1);
      reply->mutable_context()->set_event_highwater(1);
      reply->mutable_trade();
      if (request->trade_id()[0] == '\xF1') reply->mutable_trade()->set_state(static_cast<v1::TradeState>(99));
      if (request->trade_id()[0] == '\xF2') reply->GetReflection()->MutableUnknownFields(reply)->AddVarint(999, 1);
      if (request->trade_id()[0] == '\xF3') reply->mutable_trade()->set_trade_id(std::string(5000, 'x'));
      if (request->trade_id()[0] == '\xF4') reply->mutable_context()->set_result(static_cast<v1::ResultCode>(255));
      if (request->trade_id()[0] == '\xF5') reply->mutable_context()->set_result(v1::RESULT_NOT_FOUND);
      return grpc::Status::OK;
    }
    return trades.Status(context, request, reply);
  }
};

int Run() {
  auto admission = std::make_shared<detail::ServiceAdmission>();
  const auto *release = std::getenv("PLATINUMWT_GATEWAY_FIXTURE_RELEASE");
  IdentityService identities("", [](auto) { return true; }, admission);
  TestTrades trades(admission, release ? release : "");
  grpc::ServerBuilder builder;
  int port = 0;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
  builder.SetMaxReceiveMessageSize(4096);
  builder.SetMaxSendMessageSize(8192); // Deliberately permit the oversized negative fixture.
  grpc::ResourceQuota resources;
  resources.SetMaxThreads(32).Resize(16 * 1024 * 1024);
  builder.SetResourceQuota(resources);
  builder.RegisterService(&identities);
  builder.RegisterService(&trades);
  auto server = builder.BuildAndStart();
  if (!server || !port) return 1;
  std::cout << "TEST_BACKEND " << port << std::endl;
  boost::asio::io_context io;
  boost::asio::signal_set signals(io, SIGINT, SIGTERM);
  signals.async_wait([&](boost::system::error_code, int) {
    server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(1));
    io.stop();
  });
  io.run();
  return 0;
}

} // namespace
} // namespace platinumwt::server

int main(int argc, char **argv) {
  platinumwt::server::Telemetry::Global().Initialize(platinumwt::server::TelemetryRole::Backend);
  if (argc == 3 && std::string(argv[1]) == "--probe-worker") {
    auto stub = platinumwt::v1::TradeMatcher::NewStub(grpc::CreateChannel(argv[2], grpc::InsecureChannelCredentials()));
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
    platinumwt::v1::MatchRequest request;
    request.set_operation_id(std::string(16, '\xEF'));
    platinumwt::v1::MatchReply reply;
    return stub->MatchNext(&context, request, &reply).error_code() == grpc::StatusCode::UNIMPLEMENTED ? 0 : 1;
  }
  return platinumwt::server::Run();
}
