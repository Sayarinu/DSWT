#include "dswt/server/match_worker.h"
#include "dswt/server/matcher_service.h"
#include "dswt/server/trade_service.h"
#include "dswt/server/identity_store.h"

#include "postgres.h"
#include "trade_encoding.h"

#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <future>
#include <set>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char **environ;

namespace dswt::server {
namespace {

std::string Identifier() {
  static std::atomic<unsigned> serial{1};
  const auto value = serial++;
  std::string id(16, '\0');
  id[0] = '\xD6'; id[1] = static_cast<char>(value >> 8); id[2] = static_cast<char>(value);
  return id;
}

struct Client {
  v1::Credentials auth;
  v1::SubmitRequest submission;
  v1::TradeSnapshot offer;
};

class MatcherTest : public testing::Test {
protected:
  detail::Database database{""};
  TradeService trades{"", [](const auto &) { return LegalityDecision{LegalityVerdict::Legal, {}}; }, [](auto) { return true; }};
  MatcherService matcher{"", [](auto) { return true; }};
  std::vector<v1::Credentials> owners;

  Client Queue(unsigned species = 1) {
    IdentityId nonce{};
    const auto random = Identifier();
    std::copy(random.begin(), random.end(), nonce.begin());
    SaveCredential credential{};
    credential.fill(0x51);
    const auto identity = IdentityStore("").Enroll(nonce, credential);
    if (!identity) throw std::runtime_error("enrollment fixture failed");
    Client client;
    client.auth.set_save_id(identity->data(), identity->size());
    client.auth.set_credential(credential.data(), credential.size());
    owners.push_back(client.auth);
    *client.submission.mutable_context()->mutable_auth() = client.auth;
    client.submission.mutable_context()->set_operation_id(Identifier());
    client.submission.mutable_context()->set_counter(1);
    client.submission.set_escrow_generation(1);
    std::string data(136, '\0');
    data[8] = static_cast<char>(species); data[6] = static_cast<char>(species);
    client.submission.mutable_pokemon()->set_pk4(data);
    client.submission.mutable_pokemon()->set_sha256(detail::HashBytes(data));
    v1::MutationReply reply;
    const auto status = trades.Submit(nullptr, &client.submission, &reply);
    if (!status.ok() || reply.context().result() != v1::RESULT_OK) throw std::runtime_error("submission fixture failed");
    client.offer = reply.outcome();
    return client;
  }

  v1::MatchReply Match(std::string operation = Identifier(), MatcherService *service = nullptr) {
    v1::MatchRequest request;
    request.set_operation_id(operation);
    v1::MatchReply reply;
    EXPECT_TRUE((service ? service : &matcher)->MatchNext(nullptr, &request, &reply).ok());
    return reply;
  }

  v1::MatchReply Final(const std::string &operation, MatcherService &service) {
    v1::MatchReply reply;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    do {
      reply = Match(operation, &service);
      if (reply.result() != v1::RESULT_PROCESSING) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    return reply;
  }

  v1::StatusReply Status(const v1::Credentials &auth) {
    v1::StatusRequest request;
    *request.mutable_auth() = auth;
    v1::StatusReply reply;
    EXPECT_TRUE(trades.Status(nullptr, &request, &reply).ok());
    return reply;
  }

  v1::MutationReply Cancel(const Client &client) {
    v1::CancelRequest request;
    *request.mutable_context()->mutable_auth() = client.auth;
    request.mutable_context()->set_operation_id(Identifier());
    request.mutable_context()->set_counter(2);
    request.set_trade_id(client.offer.trade_id());
    v1::MutationReply reply;
    EXPECT_TRUE(trades.Cancel(nullptr, &request, &reply).ok());
    return reply;
  }

  v1::CollectReply Collect(const Client &client) {
    v1::CollectRequest request;
    *request.mutable_auth() = client.auth;
    request.set_trade_id(client.offer.trade_id());
    v1::CollectReply reply;
    EXPECT_TRUE(trades.Collect(nullptr, &request, &reply).ok());
    return reply;
  }

  std::string Scalar(const char *sql) {
    auto row = database.Execute(sql);
    return PQgetvalue(row.get(), 0, 0);
  }

  void TearDown() override {
    // Retain all evidence. Finish these synthetic offers through the real
    // lifecycle so later fixtures start with an empty eligible queue.
    for (const auto &auth : owners) {
      auto current = Status(auth);
      if (current.trade().state() == v1::TRADE_NONE) continue;
      v1::MutationReply reply;
      if (current.trade().state() == v1::TRADE_QUEUED) {
        v1::CancelRequest request;
        *request.mutable_context()->mutable_auth() = auth;
        request.mutable_context()->set_operation_id(Identifier());
        request.mutable_context()->set_counter(current.context().next_counter());
        request.set_trade_id(current.trade().trade_id());
        EXPECT_TRUE(trades.Cancel(nullptr, &request, &reply).ok());
        current = Status(auth);
      }
      v1::AcknowledgeRequest request;
      *request.mutable_context()->mutable_auth() = auth;
      request.mutable_context()->set_operation_id(Identifier());
      request.mutable_context()->set_counter(current.context().next_counter());
      request.set_trade_id(current.trade().trade_id());
      request.set_receipt_id(current.trade().receipt_id());
      request.set_delivered_sha256(current.trade().received_sha256());
      request.set_applied_sha256(detail::HashBytes("synthetic applied image"));
      request.set_applied_generation(4);
      EXPECT_TRUE(trades.Acknowledge(nullptr, &request, &reply).ok());
      EXPECT_EQ(reply.context().result(), v1::RESULT_OK);
    }
  }
};

TEST_F(MatcherTest, PairAvailabilityNeedsTwoQueuedOwnersAndWritesNothing) {
  const auto attempts = Scalar("SELECT count(*) FROM dswt.matcher_attempts");
  const auto position = Scalar("SELECT pg_current_wal_insert_lsn()");
  EXPECT_FALSE(matcher.PairAvailable());
  Queue();
  EXPECT_FALSE(matcher.PairAvailable());
  Queue();
  const auto queued = Scalar("SELECT pg_current_wal_insert_lsn()");
  EXPECT_NE(position, queued);
  EXPECT_TRUE(matcher.PairAvailable());
  EXPECT_EQ(Scalar("SELECT count(*) FROM dswt.matcher_attempts"), attempts);
  EXPECT_EQ(Scalar("SELECT pg_current_wal_insert_lsn()"), queued);
}

TEST_F(MatcherTest, PairAvailabilityFailsClosedWithoutADatabase) {
  EXPECT_FALSE(MatcherService("host=/nonexistent port=1 dbname=none").PairAvailable());
}

TEST_F(MatcherTest, InProcessWorkerPairsQueuedOffersWithoutANetworkEndpoint) {
  const auto first = Queue(), second = Queue();
  MatchWorker worker([this](const auto &request, auto *reply) { return matcher.MatchNext(nullptr, &request, reply); });
  EXPECT_EQ(worker.Poll().count(), 0);
  EXPECT_EQ(Collect(first).context().result(), v1::RESULT_OK);
  EXPECT_EQ(Collect(second).context().result(), v1::RESULT_OK);
  EXPECT_FALSE(worker.Pending());
  EXPECT_GT(worker.Poll().count(), 0) << "nothing further to pair";
}

TEST_F(MatcherTest, InvalidRequestsAndMissingRecoveryDoNotCreateAttempts) {
  for (auto operation : {std::string{}, std::string(16, '\0'), std::string(17, 'x')})
    EXPECT_EQ(Match(operation).result(), v1::RESULT_INVALID_REQUEST);
  const auto before = Scalar("SELECT count(*) FROM dswt.audit_journal");
  MatcherService closed("");
  const auto reply = Match(Identifier(), &closed);
  EXPECT_EQ(reply.result(), v1::RESULT_PROCESSING);
  EXPECT_TRUE(reply.first_trade_id().empty());
  EXPECT_EQ(Scalar("SELECT count(*) FROM dswt.audit_journal"), before);
}

TEST_F(MatcherTest, EmptyAndSingleQueuesRetainNoPairWithoutExpiry) {
  const auto empty = Identifier();
  EXPECT_EQ(Match(empty).result(), v1::RESULT_NOT_FOUND);
  const auto first = Queue();
  EXPECT_EQ(Match(empty).result(), v1::RESULT_NOT_FOUND);
  const auto single = Identifier();
  EXPECT_EQ(Match(single).result(), v1::RESULT_NOT_FOUND);
  EXPECT_EQ(Status(first.auth).trade().state(), v1::TRADE_QUEUED);
  const auto second = Queue(2);
  EXPECT_EQ(Match(single).result(), v1::RESULT_NOT_FOUND);
  const auto pair = Match();
  ASSERT_EQ(pair.result(), v1::RESULT_OK);
  EXPECT_EQ((std::set<std::string>{pair.first_trade_id(), pair.second_trade_id()}),
            (std::set<std::string>{first.offer.trade_id(), second.offer.trade_id()}));
}

TEST_F(MatcherTest, PairPreservesBothCanonicalPayloadsAndOriginalRetryOutcome) {
  const auto first = Queue(), second = Queue(2);
  const auto operation = Identifier();
  const auto pair = Match(operation);
  ASSERT_EQ(pair.result(), v1::RESULT_OK);
  const auto one = Collect(first), two = Collect(second);
  ASSERT_EQ(one.context().result(), v1::RESULT_OK);
  ASSERT_EQ(two.context().result(), v1::RESULT_OK);
  EXPECT_EQ(one.pokemon().SerializeAsString(), second.submission.pokemon().SerializeAsString());
  EXPECT_EQ(two.pokemon().SerializeAsString(), first.submission.pokemon().SerializeAsString());
  EXPECT_EQ(one.trade().delivery_kind(), v1::DELIVERY_MATCH);
  EXPECT_NE(one.trade().received_asset_id(), two.trade().received_asset_id());
  EXPECT_EQ(Match(operation).SerializeAsString(), pair.SerializeAsString());
  EXPECT_EQ(Cancel(first).context().result(), v1::RESULT_CONFLICT);
  EXPECT_EQ(Match(operation).SerializeAsString(), pair.SerializeAsString());
}

TEST_F(MatcherTest, OldestEligibleOfferAnchorsEachPair) {
  std::vector<Client> clients;
  for (int i = 0; i < 6; ++i) clients.push_back(Queue(i + 1));
  std::set<std::string> matched;
  for (int i = 0; i < 3; ++i) {
    const auto oldest = std::find_if(clients.begin(), clients.end(), [&](const auto &client) {
      return !matched.count(client.offer.trade_id());
    });
    ASSERT_NE(oldest, clients.end());
    const auto pair = Match();
    ASSERT_EQ(pair.result(), v1::RESULT_OK);
    EXPECT_TRUE(pair.first_trade_id() == oldest->offer.trade_id() || pair.second_trade_id() == oldest->offer.trade_id());
    EXPECT_TRUE(matched.insert(pair.first_trade_id()).second);
    EXPECT_TRUE(matched.insert(pair.second_trade_id()).second);
  }
}

TEST_F(MatcherTest, BusyOldestIdentityIsSkippedAndServedAfterRelease) {
  const auto first = Queue();
  Queue(); Queue(); Queue();
  {
    detail::Transaction transaction(database);
    database.Parameters("SELECT save_id FROM dswt.save_identities WHERE save_id=$1::uuid FOR UPDATE",
        {detail::HexBytes(first.auth.save_id())});
    const auto pair = Match();
    ASSERT_EQ(pair.result(), v1::RESULT_OK);
    EXPECT_NE(pair.first_trade_id(), first.offer.trade_id());
    EXPECT_NE(pair.second_trade_id(), first.offer.trade_id());
    transaction.Commit();
  }
  const auto pair = Match();
  EXPECT_EQ(pair.result(), v1::RESULT_OK);
  EXPECT_TRUE(pair.first_trade_id() == first.offer.trade_id() || pair.second_trade_id() == first.offer.trade_id());
}

TEST_F(MatcherTest, IndependentReplicasMatchEveryOfferOnce) {
  std::set<std::string> offered;
  for (int i = 0; i < 24; ++i) offered.insert(Queue(i + 1).offer.trade_id());
  std::vector<std::future<v1::MatchReply>> workers;
  for (int i = 0; i < 12; ++i) workers.push_back(std::async(std::launch::async, [&] {
    MatcherService replica("", [](auto) { return true; });
    const auto operation = Identifier();
    const auto reply = Final(operation, replica);
    EXPECT_EQ(Final(operation, replica).SerializeAsString(), reply.SerializeAsString());
    return reply;
  }));
  std::set<std::string> matched;
  for (auto &worker : workers) {
    const auto pair = worker.get();
    ASSERT_EQ(pair.result(), v1::RESULT_OK);
    EXPECT_TRUE(matched.insert(pair.first_trade_id()).second);
    EXPECT_TRUE(matched.insert(pair.second_trade_id()).second);
  }
  EXPECT_EQ(matched, offered);
}

TEST_F(MatcherTest, RepeatedConcurrentOperationRetainsExactlyOnePair) {
  Queue(); Queue(); Queue(); Queue();
  const auto operation = Identifier();
  auto worker = [&] {
    MatcherService replica("", [](auto) { return true; });
    return Final(operation, replica);
  };
  auto first = std::async(std::launch::async, worker), second = std::async(std::launch::async, worker);
  const auto a = first.get(), b = second.get();
  EXPECT_EQ(a.result(), v1::RESULT_OK);
  EXPECT_EQ(a.SerializeAsString(), b.SerializeAsString());
  const auto next = Match();
  EXPECT_EQ(next.result(), v1::RESULT_OK);
  EXPECT_NE(next.first_trade_id(), a.first_trade_id());
  EXPECT_NE(next.second_trade_id(), a.second_trade_id());
}

TEST_F(MatcherTest, CancellationAndMatchingHaveOneCommittedWinner) {
  const auto first = Queue(), second = Queue(2);
  auto worker = std::async(std::launch::async, [&] { return Match(); });
  auto cancellation = std::async(std::launch::async, [&] { return Cancel(first); });
  const auto pair = worker.get();
  const auto cancelled = cancellation.get();
  const auto current = Status(first.auth);
  if (cancelled.context().result() == v1::RESULT_OK) {
    EXPECT_EQ(pair.result(), v1::RESULT_NOT_FOUND);
    EXPECT_EQ(current.trade().state(), v1::TRADE_RETURN_READY);
    EXPECT_EQ(current.trade().received_asset_id(), first.offer.offered_asset_id());
    EXPECT_EQ(Status(second.auth).trade().state(), v1::TRADE_QUEUED);
  } else {
    EXPECT_EQ(cancelled.context().result(), v1::RESULT_CONFLICT);
    EXPECT_EQ(pair.result(), v1::RESULT_OK);
    EXPECT_EQ(current.trade().state(), v1::TRADE_MATCHED);
    EXPECT_EQ(current.trade().received_asset_id(), second.offer.offered_asset_id());
    EXPECT_EQ(Status(second.auth).trade().state(), v1::TRADE_MATCHED);
  }
}

TEST_F(MatcherTest, UncoveredDepositsCannotMatchAndCommittedPairWaitsForRecovery) {
  const auto first = Queue(), second = Queue();
  std::atomic<unsigned> checks{0};
  MatcherService pending("", [&](auto) { return ++checks <= 1; });
  const auto operation = Identifier();
  EXPECT_EQ(Match(operation, &pending).result(), v1::RESULT_PROCESSING);
  EXPECT_EQ(Status(first.auth).trade().state(), v1::TRADE_QUEUED);
  checks = 0;
  MatcherService delayed("", [&](auto) { return ++checks <= 3; });
  const auto reply = Match(operation, &delayed);
  EXPECT_EQ(reply.result(), v1::RESULT_PROCESSING);
  EXPECT_TRUE(reply.first_trade_id().empty());
  EXPECT_TRUE(reply.second_trade_id().empty());
  EXPECT_EQ(Status(first.auth).trade().state(), v1::TRADE_MATCHED);
  EXPECT_EQ(Status(second.auth).trade().state(), v1::TRADE_MATCHED);
  const auto recovered = Match(operation);
  EXPECT_EQ(recovered.result(), v1::RESULT_OK);
  EXPECT_EQ(Match(operation).SerializeAsString(), recovered.SerializeAsString());
}

TEST_F(MatcherTest, MidTransactionFailureRollsBackBothOwnersAndRedactsDiagnostics) {
  const auto first = Queue(), second = Queue();
  const auto before = Scalar("SELECT count(*) FROM dswt.audit_journal");
  database.Execute("CREATE FUNCTION dswt.fail_match_test() RETURNS trigger LANGUAGE plpgsql AS $$ "
      "BEGIN RAISE EXCEPTION 'PRIVATE_SENTINEL'; END $$; CREATE TRIGGER fail_match_test AFTER INSERT ON dswt.receipts "
      "FOR EACH ROW EXECUTE FUNCTION dswt.fail_match_test()");
  v1::MatchRequest request;
  request.set_operation_id(Identifier());
  v1::MatchReply reply;
  const auto status = matcher.MatchNext(nullptr, &request, &reply);
  database.Execute("DROP TRIGGER fail_match_test ON dswt.receipts; DROP FUNCTION dswt.fail_match_test()");
  EXPECT_EQ(status.error_code(), grpc::StatusCode::UNAVAILABLE);
  EXPECT_EQ(status.error_message().find("PRIVATE_SENTINEL"), std::string::npos);
  EXPECT_EQ(reply.ByteSizeLong(), 0U);
  EXPECT_EQ(Status(first.auth).trade().state(), v1::TRADE_QUEUED);
  EXPECT_EQ(Status(second.auth).trade().state(), v1::TRADE_QUEUED);
  EXPECT_EQ(Scalar("SELECT count(*) FROM dswt.audit_journal"), before);
  EXPECT_EQ(Match(request.operation_id()).result(), v1::RESULT_OK);
}

TEST_F(MatcherTest, SerializationFailureRetriesWithoutDuplicatingThePair) {
  Queue(); Queue();
  database.Execute("CREATE SEQUENCE dswt.match_retry_test; "
      "CREATE FUNCTION dswt.retry_match_test() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN "
      "IF nextval('dswt.match_retry_test')=1 THEN RAISE EXCEPTION 'retry' USING ERRCODE='40001'; END IF; "
      "RETURN NULL; END $$; CREATE TRIGGER retry_match_test AFTER INSERT ON dswt.receipts "
      "FOR EACH ROW EXECUTE FUNCTION dswt.retry_match_test()");
  const auto operation = Identifier();
  const auto reply = Match(operation);
  database.Execute("DROP TRIGGER retry_match_test ON dswt.receipts; DROP FUNCTION dswt.retry_match_test(); "
      "DROP SEQUENCE dswt.match_retry_test");
  EXPECT_EQ(reply.result(), v1::RESULT_OK);
  EXPECT_EQ(Match(operation).SerializeAsString(), reply.SerializeAsString());
}

int RunWorker(const std::string &operation) {
  MatcherService matcher("", [](auto) { return true; });
  v1::MatchRequest request;
  request.set_operation_id(operation);
  v1::MatchReply reply;
  const auto status = matcher.MatchNext(nullptr, &request, &reply);
  return status.ok() && reply.result() == v1::RESULT_OK && reply.first_trade_id().size() == 16 &&
      reply.second_trade_id().size() == 16 ? 0 : 1;
}

TEST_F(MatcherTest, KilledWorkerBetweenReceiptsCannotCommitAHalfMatch) {
  const auto first = Queue(), second = Queue(2);
  const auto operation = Identifier();
  database.Execute("SELECT pg_advisory_lock(578892145199); "
      "CREATE FUNCTION dswt.pause_match_test() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN "
      "PERFORM pg_advisory_xact_lock(578892145199); RETURN NULL; END $$; "
      "CREATE TRIGGER pause_match_test AFTER INSERT ON dswt.receipts "
      "FOR EACH ROW EXECUTE FUNCTION dswt.pause_match_test()");
  std::string executable = "/proc/self/exe", mode = "--match", argument = detail::HexBytes(operation);
  char *arguments[] = {executable.data(), mode.data(), argument.data(), nullptr};
  pid_t child = 0;
  const auto spawned = posix_spawn(&child, executable.c_str(), nullptr, nullptr, arguments, environ);
  bool paused = false;
  if (spawned == 0) {
    for (int poll = 0; poll < 300; ++poll) {
      paused = Scalar("SELECT EXISTS(SELECT 1 FROM pg_stat_activity WHERE datname=current_database() "
          "AND wait_event='advisory' AND query LIKE 'INSERT INTO dswt.receipts VALUES%')") == "t";
      if (paused) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(kill(child, SIGKILL), 0);
    int status = 0;
    EXPECT_EQ(waitpid(child, &status, 0), child);
    EXPECT_TRUE(WIFSIGNALED(status));
  }
  database.Execute("SELECT pg_advisory_unlock(578892145199); DROP TRIGGER pause_match_test ON dswt.receipts; "
      "DROP FUNCTION dswt.pause_match_test()");
  ASSERT_EQ(spawned, 0);
  EXPECT_TRUE(paused);
  EXPECT_EQ(Status(first.auth).trade().state(), v1::TRADE_QUEUED);
  EXPECT_EQ(Status(second.auth).trade().state(), v1::TRADE_QUEUED);
  EXPECT_EQ(Match(operation).result(), v1::RESULT_OK);
}

TEST_F(MatcherTest, GrpcAndNewProcessReplayTheCommittedPair) {
  Queue(); Queue();
  const std::string operation(16, '\xD7');
  std::string executable = "/proc/self/exe", mode = "--match", argument = detail::HexBytes(operation);
  char *arguments[] = {executable.data(), mode.data(), argument.data(), nullptr};
  pid_t child = 0;
  ASSERT_EQ(posix_spawn(&child, executable.c_str(), nullptr, nullptr, arguments, environ), 0);
  int status = 0;
  ASSERT_EQ(waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
  const auto prior = Match(operation);
  grpc::ServerBuilder builder;
  int port = 0;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
  builder.RegisterService(&matcher);
  auto server = builder.BuildAndStart();
  ASSERT_TRUE(server);
  auto stub = v1::TradeMatcher::NewStub(grpc::CreateChannel("127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials()));
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(3));
  v1::MatchRequest request;
  request.set_operation_id(operation);
  v1::MatchReply reply;
  const auto result = stub->MatchNext(&context, request, &reply);
  server->Shutdown();
  EXPECT_TRUE(result.ok());
  EXPECT_EQ(reply.SerializeAsString(), prior.SerializeAsString());
}

} // namespace
} // namespace dswt::server

int main(int argc, char **argv) {
  if (argc == 3 && std::string(argv[1]) == "--match")
    std::_Exit(dswt::server::RunWorker(dswt::server::detail::UnhexBytes(argv[2])));
  if (argc == 2 && std::string(argv[1]) == "--verify-restart")
    return dswt::server::RunWorker(std::string(16, '\xD7'));
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
