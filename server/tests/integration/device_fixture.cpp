// Disposable ROM acceptance fixture, never a production recovery provider.
#include "platinumwt/server/identity_service.h"
#include "platinumwt/server/trade_service.h"
#include "platinumwt/server/matcher_service.h"
#include "platinumwt/server/legality_client.h"
#include "admission.h"
#include "postgres.h"
#include "device_matcher.h"
#include "platinumwt/server/config.h"
#include "platinumwt/server/metrics_server.h"

#include <grpcpp/grpcpp.h>

#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
volatile std::sig_atomic_t stopping = 0;
void Stop(int) { stopping = 1; }

bool Configured() {
  const auto equals = [](const char *name, const char *expected) {
    const char *value = std::getenv(name);
    return value && std::string(value) == expected;
  };
  if (!equals("PLATINUMWT_DISPOSABLE_DEVICE_TEST", "1") || !equals("PGDATABASE", "platinumwt_e2e") ||
      !equals("PGHOST", "/test/run")) return false;
  for (const char *name : {"PGHOSTADDR", "PGSERVICE", "PGSERVICEFILE", "PLATINUMWT_DATABASE_URL", "DATABASE_URL"})
    if (std::getenv(name)) return false;
  const char *port = std::getenv("PGPORT");
  return !port || std::string(port) == "5432";
}

constexpr auto kDatabase = "host=/test/run port=5432 dbname=platinumwt_e2e";

// Load rehearsals may run several independent matcher workers; acceptance keeps one.
int MatcherWorkers() {
  const char *value = std::getenv("PLATINUMWT_DEVICE_MATCHERS");
  if (!value || !*value) return 1;
  const std::string text(value);
  if (text.size() != 1 || text[0] < '1' || text[0] > '8') throw std::invalid_argument("matcher workers must be 1-8");
  return text[0] - '0';
}

int Check() {
  using namespace platinumwt::server;
  detail::Database database(kDatabase);
  database.Execute("SELECT save_id FROM platinumwt.save_identities LIMIT 1");
  LegalityClient legality("127.0.0.1:50052", "pkhex-26.08.26-74b88906-pwt2");
  // A real worker must reject this invalid species with the pinned policy/hash.
  if (legality.CheckNewSubmission({}).verdict != LegalityVerdict::Illegal) return 1;
  auto matcher = platinumwt::v1::TradeMatcher::NewStub(
      grpc::CreateChannel("127.0.0.1:50054", grpc::InsecureChannelCredentials()));
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
  platinumwt::v1::MatchRequest request;
  platinumwt::v1::MatchReply reply;
  const auto result = matcher->MatchNext(&context, request, &reply);
  if (!result.ok() || reply.result() != platinumwt::v1::RESULT_INVALID_REQUEST) return 1;
  std::cout << "Local database, strict validator and matcher ready" << std::endl;
  return 0;
}
} // namespace

int main(int argc, char **argv) try {
  if (!Configured()) return 2;
  if (argc == 2 && std::string(argv[1]) == "--check") return Check();
  if (argc != 1) return 2;

  using namespace platinumwt::server;
  Telemetry::Global().Initialize(TelemetryRole::Backend);
  MetricsServer metrics(LoadMetricsPort(), kDatabase);
  LegalityClient legality("127.0.0.1:50052", "pkhex-26.08.26-74b88906-pwt2");
  // Same-host test data proves local flows only, never off-site recovery.
  RecoveryBarrier localTest = [](auto) { return true; };
  auto admission = std::make_shared<detail::ServiceAdmission>();
  IdentityService identities(kDatabase, localTest, admission);
  TradeService trades(kDatabase, [&](const auto &bytes) { return legality.CheckNewSubmission(bytes); }, localTest, admission);
  MatcherService matcher(kDatabase, localTest);

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
  matcherBuilder.SetMaxReceiveMessageSize(4096);
  matcherBuilder.SetMaxSendMessageSize(4096);
  matcherBuilder.RegisterService(&matcher);
  auto matcherServer = matcherBuilder.BuildAndStart();
  if (!backend || !matcherServer) return 1;

  std::signal(SIGINT, Stop);
  std::signal(SIGTERM, Stop);
  std::vector<std::thread> workers;
  for (int index = MatcherWorkers(); index > 0; --index)
    workers.emplace_back([] {
      DeviceMatcher worker("127.0.0.1:50054");
      while (!stopping) {
        if (std::filesystem::exists("/test/matcher-hold")) {
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
          continue;
        }
        std::this_thread::sleep_for(worker.Poll());
      }
    });
  std::cout << "Disposable real-ledger backend ready; no production/off-site claim" << std::endl;
  for (auto &worker : workers) worker.join();
  backend->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(2));
  matcherServer->Shutdown();
} catch (const std::exception &) {
  std::cerr << "Local acceptance service unavailable" << std::endl;
  return 1;
}
