// Disposable ROM acceptance fixture, never a production recovery provider.
#include "dswt/server/identity_service.h"
#include "dswt/server/trade_service.h"
#include "dswt/server/matcher_service.h"
#include "dswt/server/legality_client.h"
#include "admission.h"
#include "postgres.h"
#include "device_matcher.h"
#include "dswt/server/config.h"
#include "dswt/server/metrics_server.h"

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
// Read by every matcher thread; lock-free so the signal handler may set it.
std::atomic<bool> stopping{false};
static_assert(std::atomic<bool>::is_always_lock_free);
void Stop(int) { stopping = true; }

bool Configured() {
  const auto equals = [](const char *name, const char *expected) {
    const char *value = std::getenv(name);
    return value && std::string(value) == expected;
  };
  if (!equals("DSWT_DISPOSABLE_DEVICE_TEST", "1") || !equals("PGDATABASE", "dswt_e2e") ||
      !equals("PGHOST", "/test/run")) return false;
  for (const char *name : {"PGHOSTADDR", "PGSERVICE", "PGSERVICEFILE", "DSWT_DATABASE_URL", "DATABASE_URL"})
    if (std::getenv(name)) return false;
  const char *port = std::getenv("PGPORT");
  return !port || std::string(port) == "5432";
}

constexpr auto kDatabase = "host=/test/run port=5432 dbname=dswt_e2e";

// Load rehearsals may run several independent matcher workers; acceptance keeps one.
int MatcherWorkers() {
  const char *value = std::getenv("DSWT_DEVICE_MATCHERS");
  if (!value || !*value) return 1;
  const std::string text(value);
  if (text.size() != 1 || text[0] < '1' || text[0] > '8') throw std::invalid_argument("matcher workers must be 1-8");
  return text[0] - '0';
}

int Check() {
  using namespace dswt::server;
  detail::Database database(kDatabase);
  database.Execute("SELECT save_id FROM dswt.save_identities LIMIT 1");
  LegalityClient legality("127.0.0.1:50052", "pkhex-26.08.26-74b88906-dswt3");
  // A real worker must reject this invalid species with the pinned policy/hash.
  if (legality.CheckNewSubmission({}).verdict != LegalityVerdict::Illegal) return 1;
  auto matcher = dswt::v1::TradeMatcher::NewStub(
      grpc::CreateChannel("127.0.0.1:50054", grpc::InsecureChannelCredentials()));
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
  dswt::v1::MatchRequest request;
  dswt::v1::MatchReply reply;
  const auto result = matcher->MatchNext(&context, request, &reply);
  if (!result.ok() || reply.result() != dswt::v1::RESULT_INVALID_REQUEST) return 1;
  std::cout << "Local database, strict validator and matcher ready" << std::endl;
  return 0;
}
} // namespace

int main(int argc, char **argv) try {
  if (!Configured()) return 2;
  if (argc == 2 && std::string(argv[1]) == "--check") return Check();
  if (argc != 1) return 2;

  using namespace dswt::server;
  Telemetry::Global().Initialize(TelemetryRole::Backend);
  MetricsServer metrics(LoadMetricsPort(), kDatabase);
  LegalityClient legality("127.0.0.1:50052", "pkhex-26.08.26-74b88906-dswt3");
  // Same-host test data proves local flows only, never off-site recovery.
  RecoveryBarrier localTest = [](auto) { return true; };
  auto admission = std::make_shared<detail::ServiceAdmission>(LoadSubmissionWorkers());
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
  std::atomic<bool> failed{false};
  for (int index = MatcherWorkers(); index > 0; --index)
    workers.emplace_back([&failed, &matcher] {
      try {
        DeviceMatcher worker("127.0.0.1:50054", [&matcher] { return matcher.PairAvailable(); });
        while (!stopping) {
          if (std::filesystem::exists("/test/matcher-hold")) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
          }
          std::this_thread::sleep_for(worker.Poll());
        }
      } catch (const std::exception &) {
        // A failed worker stops the whole fixture rather than silently reducing matching capacity.
        failed = true;
        stopping = true;
      }
    });
  std::cout << "Disposable real-ledger backend ready; no production/off-site claim" << std::endl;
  for (auto &worker : workers) worker.join();
  backend->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(2));
  matcherServer->Shutdown();
  if (failed) {
    std::cerr << "Local matcher worker failed" << std::endl;
    return 1;
  }
} catch (const std::exception &) {
  std::cerr << "Local acceptance service unavailable" << std::endl;
  return 1;
}
