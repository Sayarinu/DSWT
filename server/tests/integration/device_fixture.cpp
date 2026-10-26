// Disposable ROM acceptance fixture, never a production recovery provider.
#include "platinumwt/server/identity_service.h"
#include "platinumwt/server/trade_service.h"
#include "platinumwt/server/matcher_service.h"
#include "platinumwt/server/legality_client.h"

#include <grpcpp/grpcpp.h>
#include <openssl/rand.h>

#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <thread>

namespace {
volatile std::sig_atomic_t stopping = 0;
void Stop(int) { stopping = 1; }
} // namespace

int main() {
  const char *mode = std::getenv("PLATINUMWT_DISPOSABLE_DEVICE_TEST");
  const char *database = std::getenv("PGDATABASE");
  const char *host = std::getenv("PGHOST");
  if (!mode || std::string(mode) != "1" || !database || std::string(database) != "platinumwt_e2e" ||
      !host || std::string(host) != "/test/run") return 2;

  using namespace platinumwt::server;
  LegalityClient legality("127.0.0.1:50052", "pkhex-26.08.26-74b88906-pwt2");
  // Same-host test data proves local flows only, never off-site recovery.
  RecoveryBarrier localTest = [](auto) { return true; };
  IdentityService identities("", localTest);
  TradeService trades("", [&](const auto &bytes) { return legality.CheckNewSubmission(bytes); }, localTest);
  MatcherService matcher("", localTest);

  grpc::ServerBuilder builder;
  builder.AddListeningPort("127.0.0.1:50053", grpc::InsecureServerCredentials());
  builder.SetMaxReceiveMessageSize(4096);
  builder.SetMaxSendMessageSize(4096);
  grpc::ResourceQuota resources;
  resources.SetMaxThreads(32).Resize(16 * 1024 * 1024);
  builder.SetResourceQuota(resources);
  builder.RegisterService(&identities);
  builder.RegisterService(&trades);
  auto backend = builder.BuildAndStart();
  grpc::ServerBuilder matcherBuilder;
  matcherBuilder.AddListeningPort("127.0.0.1:50054", grpc::InsecureServerCredentials());
  matcherBuilder.RegisterService(&matcher);
  auto matcherServer = matcherBuilder.BuildAndStart();
  if (!backend || !matcherServer) return 1;

  auto worker = platinumwt::v1::TradeMatcher::NewStub(
      grpc::CreateChannel("127.0.0.1:50054", grpc::InsecureChannelCredentials()));
  std::signal(SIGINT, Stop);
  std::signal(SIGTERM, Stop);
  std::cout << "Disposable real-ledger backend ready; no production/off-site claim" << std::endl;
  while (!stopping) {
    if (std::filesystem::exists("/test/matcher-hold")) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue;
    }
    unsigned char id[16];
    if (RAND_bytes(id, sizeof(id)) != 1) break;
    platinumwt::v1::MatchRequest request;
    request.set_operation_id(id, sizeof(id));
    platinumwt::v1::MatchReply reply;
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
    auto result = worker->MatchNext(&context, request, &reply);
    if (!result.ok()) std::cerr << "Local matcher unavailable" << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }
  backend->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(2));
  matcherServer->Shutdown();
}
