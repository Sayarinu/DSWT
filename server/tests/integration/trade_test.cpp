#include "dswt/server/trade_service.h"
#include "dswt/server/identity_store.h"
#include "dswt/server/identity_service.h"

#include "postgres.h"
#include "trade_encoding.h"

#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include <atomic>
#include <future>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace dswt::server {
namespace {

std::string Identifier() {
  static std::atomic<unsigned> serial{1};
  std::string id(16, '\0');
  auto value = serial++;
  id[0] = '\xD5';
  id[1] = static_cast<char>(value >> 8);
  id[2] = static_cast<char>(value);
  return id;
}

v1::Credentials Enroll(std::string enrollment = Identifier()) {
  IdentityStore store("");
  IdentityId id{};
  std::copy(enrollment.begin(), enrollment.end(), id.begin());
  SaveCredential credential{};
  credential.fill(0x51);
  const auto result = store.Enroll(id, credential);
  if (!result) throw std::runtime_error("fixture enrollment failed");
  v1::Credentials auth;
  auth.set_save_id(result->data(), result->size());
  auth.set_credential(std::string(32, '\x51'));
  return auth;
}

v1::MutationContext Mutation(const v1::Credentials &auth, std::uint64_t counter) {
  v1::MutationContext context;
  *context.mutable_auth() = auth;
  context.set_operation_id(Identifier());
  context.set_counter(counter);
  return context;
}

v1::SubmitRequest Submission(const v1::Credentials &auth, std::uint64_t counter = 1) {
  v1::SubmitRequest request;
  *request.mutable_context() = Mutation(auth, counter);
  request.set_escrow_generation(1);
  std::string data(136, '\0');
  data[8] = 1; // Deliberately synthetic: shape valid; fake validator tests ownership only.
  data[6] = 1;
  request.mutable_pokemon()->set_pk4(data);
  request.mutable_pokemon()->set_sha256(detail::HashBytes(data));
  return request;
}

class TradeTest : public testing::Test {
protected:
  std::atomic<int> validations{0};
  std::atomic<LegalityVerdict> verdict{LegalityVerdict::Legal};
  std::atomic<bool> covered{true};
  TradeService service{"", [&](const auto &) {
    ++validations;
    return LegalityDecision{verdict.load(), {}};
  }, [&](auto) { return covered.load(); }};
  detail::Database database{""};
  v1::Credentials auth = Enroll();

  v1::MutationReply Submit(const v1::SubmitRequest &request) {
    v1::MutationReply reply;
    EXPECT_TRUE(service.Submit(nullptr, &request, &reply).ok());
    return reply;
  }
  v1::StatusReply Status(std::string trade = {}, v1::Credentials owner = {}) {
    v1::StatusRequest request;
    *request.mutable_auth() = owner.save_id().empty() ? auth : owner;
    request.set_trade_id(trade);
    v1::StatusReply reply;
    EXPECT_TRUE(service.Status(nullptr, &request, &reply).ok());
    return reply;
  }
  v1::MutationReply Cancel(const v1::CancelRequest &request) {
    v1::MutationReply reply;
    EXPECT_TRUE(service.Cancel(nullptr, &request, &reply).ok());
    return reply;
  }
  v1::CancelRequest Cancellation(const v1::TradeSnapshot &snapshot, std::uint64_t counter = 2) {
    v1::CancelRequest request;
    *request.mutable_context() = Mutation(auth, counter);
    request.set_trade_id(snapshot.trade_id());
    return request;
  }
  v1::CollectReply Collect(const std::string &trade, v1::Credentials owner = {}) {
    v1::CollectRequest request;
    *request.mutable_auth() = owner.save_id().empty() ? auth : owner;
    request.set_trade_id(trade);
    v1::CollectReply reply;
    EXPECT_TRUE(service.Collect(nullptr, &request, &reply).ok());
    return reply;
  }
  v1::AcknowledgeRequest Acknowledgement(const v1::TradeSnapshot &snapshot, std::uint64_t counter = 3) {
    v1::AcknowledgeRequest request;
    *request.mutable_context() = Mutation(auth, counter);
    request.set_trade_id(snapshot.trade_id());
    request.set_receipt_id(snapshot.receipt_id());
    request.set_delivered_sha256(snapshot.received_sha256());
    request.set_applied_sha256(detail::HashBytes("synthetic applied image"));
    request.set_applied_generation(3);
    return request;
  }
  v1::MutationReply Ack(const v1::AcknowledgeRequest &request) {
    v1::MutationReply reply;
    EXPECT_TRUE(service.Acknowledge(nullptr, &request, &reply).ok());
    return reply;
  }
  v1::ReconcileReply Reconcile(std::uint64_t after = 0, unsigned limit = 8) {
    v1::ReconcileRequest request;
    *request.mutable_auth() = auth;
    request.set_after_event(after);
    request.set_limit(limit);
    v1::ReconcileReply reply;
    EXPECT_TRUE(service.Reconcile(nullptr, &request, &reply).ok());
    return reply;
  }
  std::string Scalar(const char *sql) {
    auto row = database.Execute(sql);
    return PQgetvalue(row.get(), 0, 0);
  }
};

TEST_F(TradeTest, EnrollmentEndpointPublishesOnlyCoveredIdentityAndLiveProgress) {
  v1::EnrollRequest request;
  request.set_enrollment_id(Identifier());
  request.set_credential(std::string(32, '\x51'));
  v1::EnrollReply reply;
  IdentityService closed("");
  ASSERT_TRUE(closed.Enroll(nullptr, &request, &reply).ok());
  EXPECT_EQ(reply.context().result(), v1::RESULT_PROCESSING);
  EXPECT_TRUE(reply.save_id().empty());
  EXPECT_EQ(reply.context().event_highwater(), 0U);
  IdentityService endpoint("", [](auto) { return true; });
  ASSERT_TRUE(endpoint.Enroll(nullptr, &request, &reply).ok());
  ASSERT_EQ(reply.context().result(), v1::RESULT_OK);
  const auto id = reply.save_id();
  auto owner = auth;
  owner.set_save_id(id);
  ASSERT_EQ(Submit(Submission(owner)).context().result(), v1::RESULT_OK);
  ASSERT_TRUE(endpoint.Enroll(nullptr, &request, &reply).ok());
  EXPECT_EQ(reply.save_id(), id);
  EXPECT_EQ(reply.context().next_counter(), 2U);
  EXPECT_EQ(reply.context().event_highwater(), 2U);
  request.set_credential(std::string(32, '\x52'));
  ASSERT_TRUE(endpoint.Enroll(nullptr, &request, &reply).ok());
  EXPECT_EQ(reply.context().result(), v1::RESULT_CONFLICT);
  EXPECT_TRUE(reply.save_id().empty());
  EXPECT_EQ(reply.context().event_highwater(), 0U);
}

TEST_F(TradeTest, SubmissionStatusRetryAndImmutableOutcome) {
  const auto request = Submission(auth);
  const auto submitted = Submit(request);
  ASSERT_EQ(submitted.context().result(), v1::RESULT_OK);
  EXPECT_EQ(submitted.context().next_counter(), 2U);
  EXPECT_EQ(submitted.operation_event(), 2U);
  EXPECT_EQ(submitted.outcome().state(), v1::TRADE_QUEUED);
  EXPECT_EQ(Status().trade().SerializeAsString(), submitted.outcome().SerializeAsString());
  EXPECT_EQ(Submit(request).SerializeAsString(), submitted.SerializeAsString());
  EXPECT_EQ(validations.load(), 1);
  Cancel(Cancellation(submitted.outcome()));
  verdict = LegalityVerdict::Unavailable;
  const auto replay = Submit(request);
  EXPECT_EQ(replay.outcome().SerializeAsString(), submitted.outcome().SerializeAsString());
  EXPECT_EQ(replay.operation_event(), 2U);
  EXPECT_EQ(replay.context().next_counter(), 3U);
  EXPECT_EQ(validations.load(), 1);
}

TEST_F(TradeTest, OperationReuseConflictsBeforeCounterChecks) {
  auto request = Submission(auth);
  ASSERT_EQ(Submit(request).context().result(), v1::RESULT_OK);
  request.mutable_context()->set_counter(0);
  EXPECT_EQ(Submit(request).context().result(), v1::RESULT_CONFLICT);
  request.mutable_context()->set_counter(1);
  request.set_escrow_generation(2);
  EXPECT_EQ(Submit(request).context().result(), v1::RESULT_CONFLICT);
  const auto snapshot = Status().trade();
  auto cancel = Cancellation(snapshot, 1);
  cancel.mutable_context()->set_operation_id(request.context().operation_id());
  EXPECT_EQ(Cancel(cancel).context().result(), v1::RESULT_CONFLICT);
  EXPECT_EQ(Status().context().next_counter(), 2U);
  EXPECT_EQ(validations.load(), 1);
}

TEST_F(TradeTest, CounterShapeAndCapacityFailuresDoNotConsumeOperations) {
  for (auto counter : {0ULL, 2ULL, 18446744073709551615ULL})
    EXPECT_EQ(Submit(Submission(auth, counter)).context().result(), v1::RESULT_COUNTER_MISMATCH);
  auto malformed = Submission(auth);
  malformed.mutable_pokemon()->set_sha256(std::string(32, 'x'));
  EXPECT_EQ(Submit(malformed).context().result(), v1::RESULT_INVALID_REQUEST);
  malformed = Submission(auth);
  (*malformed.mutable_pokemon()->mutable_pk4())[6] = 2;
  malformed.mutable_pokemon()->set_sha256(detail::HashBytes(malformed.pokemon().pk4()));
  EXPECT_EQ(Submit(malformed).context().result(), v1::RESULT_INVALID_REQUEST);
  ASSERT_EQ(Submit(Submission(auth)).context().result(), v1::RESULT_OK);
  EXPECT_EQ(Submit(Submission(auth, 2)).context().result(), v1::RESULT_CAPACITY);
  EXPECT_EQ(Status().context().next_counter(), 2U);
  EXPECT_EQ(validations.load(), 1);
}

TEST_F(TradeTest, AuthorizationAndForeignTradesNeverDiscloseOwnership) {
  const auto submitted = Submit(Submission(auth));
  auto wrong = auth;
  wrong.set_credential(std::string(32, '\x52'));
  EXPECT_EQ(Status({}, wrong).context().result(), v1::RESULT_UNAUTHENTICATED);
  EXPECT_FALSE(Status({}, wrong).has_trade());
  auto other = Enroll();
  const auto foreign = Status(submitted.outcome().trade_id(), other);
  EXPECT_EQ(foreign.context().result(), v1::RESULT_NOT_FOUND);
  EXPECT_FALSE(foreign.has_trade());
  const auto missing = Status(Identifier());
  EXPECT_EQ(missing.context().result(), v1::RESULT_NOT_FOUND);
  EXPECT_FALSE(missing.has_trade());
  EXPECT_EQ(Collect(submitted.outcome().trade_id(), other).context().result(), v1::RESULT_NOT_FOUND);
  auto cancel = Cancellation(submitted.outcome(), 1);
  *cancel.mutable_context()->mutable_auth() = other;
  EXPECT_EQ(Cancel(cancel).context().result(), v1::RESULT_NOT_FOUND);
  *cancel.mutable_context()->mutable_auth() = wrong;
  EXPECT_EQ(Cancel(cancel).context().result(), v1::RESULT_UNAUTHENTICATED);
  auto ack = Acknowledgement(submitted.outcome(), 1);
  ack.set_receipt_id(Identifier());
  ack.set_delivered_sha256(std::string(32, 'x'));
  *ack.mutable_context()->mutable_auth() = wrong;
  EXPECT_EQ(Ack(ack).context().result(), v1::RESULT_UNAUTHENTICATED);
  v1::ReconcileRequest request;
  *request.mutable_auth() = wrong;
  request.set_limit(8);
  v1::ReconcileReply reply;
  ASSERT_TRUE(service.Reconcile(nullptr, &request, &reply).ok());
  EXPECT_EQ(reply.context().result(), v1::RESULT_UNAUTHENTICATED);
  EXPECT_EQ(reply.events_size(), 0);
  EXPECT_EQ(Status().trade().state(), v1::TRADE_QUEUED);
}

TEST_F(TradeTest, DefinitiveRejectionRetainsMatchingDigestAndRetry) {
  verdict = LegalityVerdict::Illegal;
  const auto request = Submission(auth);
  const auto rejected = Submit(request);
  ASSERT_EQ(rejected.context().result(), v1::RESULT_ILLEGAL_POKEMON);
  EXPECT_EQ(rejected.context().next_counter(), 2U);
  EXPECT_FALSE(rejected.has_outcome());
  verdict = LegalityVerdict::Unavailable;
  EXPECT_EQ(Submit(request).SerializeAsString(), rejected.SerializeAsString());
  EXPECT_EQ(validations.load(), 1);
  const auto events = Reconcile();
  ASSERT_EQ(events.events_size(), 2);
  EXPECT_EQ(events.events(1).kind(), v1::EVENT_REJECTED);
  EXPECT_EQ(events.events(1).operation_id(), request.context().operation_id());
  EXPECT_EQ(events.events(1).payload_sha256(), request.pokemon().sha256());
  EXPECT_TRUE(events.events(1).asset_id().empty());
  EXPECT_EQ(Status().trade().state(), v1::TRADE_NONE);
}

TEST_F(TradeTest, ValidatorOutageDoesNotBlockAcceptedReturnCollectAndAck) {
  auto request = Submission(auth);
  verdict = LegalityVerdict::Unavailable;
  EXPECT_EQ(Submit(request).context().result(), v1::RESULT_UNAVAILABLE);
  EXPECT_EQ(Status().context().next_counter(), 1U);
  verdict = LegalityVerdict::Legal;
  const auto submitted = Submit(request);
  ASSERT_EQ(submitted.context().result(), v1::RESULT_OK);
  verdict = LegalityVerdict::Unavailable;
  const auto cancellation = Cancellation(submitted.outcome());
  const auto returned = Cancel(cancellation);
  ASSERT_EQ(returned.context().result(), v1::RESULT_OK);
  EXPECT_EQ(returned.outcome().delivery_kind(), v1::DELIVERY_RETURN);
  EXPECT_EQ(Cancel(cancellation).SerializeAsString(), returned.SerializeAsString());
  EXPECT_EQ(Collect(returned.outcome().trade_id()).pokemon().pk4(), request.pokemon().pk4());
  const auto acknowledgement = Acknowledgement(returned.outcome());
  const auto applied = Ack(acknowledgement);
  ASSERT_EQ(applied.context().result(), v1::RESULT_OK);
  EXPECT_EQ(applied.outcome().state(), v1::TRADE_ACKNOWLEDGED);
  EXPECT_EQ(applied.outcome().delivery_kind(), v1::DELIVERY_RETURN);
  EXPECT_EQ(Ack(acknowledgement).SerializeAsString(), applied.SerializeAsString());
  EXPECT_EQ(Status().trade().state(), v1::TRADE_NONE);
  EXPECT_EQ(Status(returned.outcome().trade_id()).trade().state(), v1::TRADE_ACKNOWLEDGED);
  EXPECT_EQ(Collect(returned.outcome().trade_id()).pokemon().pk4(), request.pokemon().pk4());
  EXPECT_EQ(Collect(returned.outcome().trade_id()).context().next_counter(), 4U);
  EXPECT_EQ(validations.load(), 2);
}

TEST_F(TradeTest, ForgedCredentialsNeverAcquireOrWaitForAnOwnersWriteLock) {
  auto forged = Submission(auth);
  forged.mutable_context()->mutable_auth()->set_credential(std::string(32, '\x52'));
  detail::Transaction held(database);
  database.Parameters("SELECT save_id FROM dswt.save_identities WHERE save_id=$1::uuid FOR UPDATE",
      {detail::HexBytes(auth.save_id())});
  const auto before = std::chrono::steady_clock::now();
  const auto reply = Submit(forged);
  EXPECT_LT(std::chrono::steady_clock::now() - before, std::chrono::seconds(1));
  EXPECT_EQ(reply.context().result(), v1::RESULT_UNAUTHENTICATED);
  EXPECT_EQ(reply.context().next_counter(), 0U);
  EXPECT_EQ(validations.load(), 0);
}

TEST_F(TradeTest, ForgedCredentialChurnCannotEvictARealOwnersFirstRecoveryRequest) {
  const auto request = Submission(auth);
  const auto submitted = Submit(request);
  ASSERT_EQ(submitted.context().result(), v1::RESULT_OK);
  // Prepare a delivery using an independent service; the target service has
  // never admitted a recovery key for this accepted deposit.
  TradeService preparation("", {}, [](auto) { return true; });
  auto cancel = Cancellation(submitted.outcome());
  v1::MutationReply returned;
  ASSERT_TRUE(preparation.Cancel(nullptr, &cancel, &returned).ok());
  ASSERT_EQ(returned.context().result(), v1::RESULT_OK);
  std::atomic<unsigned> serial{0}, failures{0};
  std::vector<std::future<void>> attackers;
  for (unsigned worker = 0; worker < 8; ++worker) attackers.push_back(std::async(std::launch::async, [&] {
    for (unsigned index; (index = serial++) < 4097;) {
      v1::StatusRequest forged;
      *forged.mutable_auth() = auth;
      std::string secret(32, '\x52');
      for (unsigned byte = 0; byte < 4; ++byte) secret[byte] = static_cast<char>(index >> (8 * byte));
      forged.mutable_auth()->set_credential(secret);
      v1::StatusReply reply;
      const auto status = service.Status(nullptr, &forged, &reply);
      if (!status.ok() || reply.context().result() != v1::RESULT_UNAUTHENTICATED ||
          reply.context().next_counter() != 0 || reply.context().event_highwater() != 0) ++failures;
    }
  }));
  for (auto &attacker : attackers) attacker.get();
  ASSERT_EQ(failures, 0U);
  const auto incoming = Collect(returned.outcome().trade_id());
  ASSERT_EQ(incoming.context().result(), v1::RESULT_OK);
  EXPECT_EQ(incoming.pokemon().pk4(), request.pokemon().pk4());
  const auto applied = Ack(Acknowledgement(incoming.trade()));
  ASSERT_EQ(applied.context().result(), v1::RESULT_OK);
  EXPECT_EQ(applied.outcome().state(), v1::TRADE_ACKNOWLEDGED);
  EXPECT_EQ(validations.load(), 1);
}

TEST_F(TradeTest, ReceiptProofAndTerminalCancellationFailuresAreRetained) {
  const auto submitted = Submit(Submission(auth));
  const auto returned = Cancel(Cancellation(submitted.outcome()));
  ASSERT_EQ(returned.context().result(), v1::RESULT_OK);
  auto invalid = Acknowledgement(returned.outcome());
  invalid.set_delivered_sha256(std::string(32, 'x'));
  EXPECT_EQ(Ack(invalid).context().result(), v1::RESULT_CONFLICT);
  EXPECT_EQ(Status().trade().state(), v1::TRADE_RETURN_READY);
  auto correct = Acknowledgement(returned.outcome(), 4);
  ASSERT_EQ(Ack(correct).context().result(), v1::RESULT_OK);
  EXPECT_EQ(Cancel(Cancellation(returned.outcome(), 5)).context().result(), v1::RESULT_CONFLICT);
  EXPECT_EQ(Status(returned.outcome().trade_id()).trade().state(), v1::TRADE_ACKNOWLEDGED);
  EXPECT_EQ(Status().context().next_counter(), 6U);
}

TEST_F(TradeTest, MatchedTradeCannotCancelAndDeliversOnlyItsAssignedPeerAsset) {
  const auto offered = Submit(Submission(auth));
  const auto peer = Enroll();
  auto peer_request = Submission(peer);
  auto data = peer_request.pokemon().pk4();
  data[6] = data[8] = 2;
  peer_request.mutable_pokemon()->set_pk4(data);
  peer_request.mutable_pokemon()->set_sha256(detail::HashBytes(data));
  const auto peer_offer = Submit(peer_request);
  ASSERT_EQ(offered.context().result(), v1::RESULT_OK);
  ASSERT_EQ(peer_offer.context().result(), v1::RESULT_OK);
  // Build a committed match through the existing schema contract; worker
  // selection/concurrent matching is implemented in its following ticket.
  const auto receipt = Identifier(), peer_receipt = Identifier(), match = Identifier();
  detail::Transaction transaction(database);
  const std::vector<std::string> parameters = {
      detail::HexBytes(auth.save_id()), detail::HexBytes(offered.outcome().trade_id()),
      detail::HexBytes(offered.outcome().offered_asset_id()), detail::HexBytes(peer.save_id()),
      detail::HexBytes(peer_offer.outcome().trade_id()), detail::HexBytes(peer_offer.outcome().offered_asset_id()),
      detail::HexBytes(receipt), detail::HexBytes(peer_receipt), detail::HexBytes(match), detail::HexBytes(Identifier())};
  database.Parameters("SELECT save_id FROM dswt.save_identities WHERE save_id IN ($1::uuid,$2::uuid) "
      "ORDER BY save_id FOR UPDATE", {parameters[0], parameters[3]});
  auto pair_sql = [&](const char *sql) {
    const std::string typed = "WITH args AS (SELECT $1::uuid,$2::uuid,$3::uuid,$4::uuid,$5::uuid,"
        "$6::uuid,$7::uuid,$8::uuid,$9::uuid,$10::uuid) ";
    return database.Parameters((typed + sql).c_str(), parameters);
  };
  pair_sql("INSERT INTO dswt.matches VALUES ($9::uuid,$10::uuid,"
      "least($2::uuid,$5::uuid),greatest($2::uuid,$5::uuid))");
  pair_sql("INSERT INTO dswt.matcher_attempts VALUES ($10::uuid,$9::uuid)");
  pair_sql("INSERT INTO dswt.receipts VALUES "
      "($7::uuid,$2::uuid,$1::uuid,$6::uuid,1,$9::uuid),"
      "($8::uuid,$5::uuid,$4::uuid,$3::uuid,1,$9::uuid)");
  pair_sql("UPDATE dswt.offers SET state=2,last_event=3,receipt_id=CASE "
      "WHEN trade_id=$2::uuid THEN $7::uuid ELSE $8::uuid END WHERE trade_id IN ($2::uuid,$5::uuid)");
  pair_sql("UPDATE dswt.ownership SET location=2,last_event=3,"
      "save_id=CASE WHEN asset_id=$3::uuid THEN $4::uuid ELSE $1::uuid END,"
      "trade_id=CASE WHEN asset_id=$3::uuid THEN $5::uuid ELSE $2::uuid END WHERE asset_id IN ($3::uuid,$6::uuid)");
  pair_sql("INSERT INTO dswt.trade_events(save_id,sequence,kind,trade_id,asset_id,payload_sha256,receipt_id,state) "
      "SELECT r.save_id,3,3,r.trade_id,r.asset_id,p.sha256,r.receipt_id,2 FROM dswt.receipts r "
      "JOIN dswt.payloads p USING(asset_id) WHERE r.match_id=$9::uuid");
  transaction.Commit();
  EXPECT_EQ(Cancel(Cancellation(offered.outcome())).context().result(), v1::RESULT_CONFLICT);
  const auto incoming = Collect(offered.outcome().trade_id());
  ASSERT_EQ(incoming.context().result(), v1::RESULT_OK);
  EXPECT_EQ(incoming.pokemon().pk4(), peer_request.pokemon().pk4());
  EXPECT_EQ(incoming.trade().delivery_kind(), v1::DELIVERY_MATCH);
  auto wrong = Acknowledgement(incoming.trade(), 3);
  wrong.set_delivered_sha256(offered.outcome().offered_sha256());
  EXPECT_EQ(Ack(wrong).context().result(), v1::RESULT_CONFLICT);
  const auto applied = Ack(Acknowledgement(incoming.trade(), 4));
  ASSERT_EQ(applied.context().result(), v1::RESULT_OK);
  EXPECT_EQ(applied.outcome().delivery_kind(), v1::DELIVERY_MATCH);
  EXPECT_EQ(applied.outcome().state(), v1::TRADE_ACKNOWLEDGED);
  EXPECT_EQ(Collect(offered.outcome().trade_id()).pokemon().pk4(), peer_request.pokemon().pk4());
  EXPECT_EQ(Status(peer_offer.outcome().trade_id(), peer).trade().state(), v1::TRADE_MATCHED);
}

TEST_F(TradeTest, ValidatorWaitHoldsNoDatabaseOrIdentityLock) {
  std::promise<void> entered, release;
  auto released = release.get_future().share();
  TradeService waiting("", [&](const auto &) {
    entered.set_value();
    released.wait();
    return LegalityDecision{LegalityVerdict::Legal, {}};
  }, [](auto) { return true; });
  const auto request = Submission(auth);
  auto pending = std::async(std::launch::async, [&] {
    v1::MutationReply reply;
    auto status = waiting.Submit(nullptr, &request, &reply);
    EXPECT_TRUE(status.ok());
    return reply;
  });
  const auto ready = entered.get_future().wait_for(std::chrono::seconds(2));
  const auto before = std::chrono::steady_clock::now();
  const auto current = Status();
  const auto elapsed = std::chrono::steady_clock::now() - before;
  release.set_value();
  const auto submitted = pending.get();
  ASSERT_EQ(ready, std::future_status::ready);
  EXPECT_EQ(current.trade().state(), v1::TRADE_NONE);
  EXPECT_LT(elapsed, std::chrono::seconds(1));
  EXPECT_EQ(submitted.context().result(), v1::RESULT_OK);
}

TEST_F(TradeTest, ReconciliationIsContiguousBoundedAndRetained) {
  verdict = LegalityVerdict::Illegal;
  for (unsigned counter = 1; counter <= 10; ++counter)
    ASSERT_EQ(Submit(Submission(auth, counter)).context().result(), v1::RESULT_ILLEGAL_POKEMON);
  const auto first = Reconcile();
  ASSERT_EQ(first.events_size(), 8);
  EXPECT_EQ(first.page_end(), 8U);
  EXPECT_TRUE(first.has_more());
  const auto last = Reconcile(first.page_end());
  ASSERT_EQ(last.events_size(), 3);
  EXPECT_EQ(last.events(0).sequence(), 9U);
  EXPECT_EQ(last.page_end(), 11U);
  EXPECT_FALSE(last.has_more());
  EXPECT_EQ(Reconcile(11).page_end(), 11U);
  EXPECT_EQ(Reconcile(12).context().result(), v1::RESULT_CURSOR_AHEAD);
  EXPECT_EQ(Reconcile(0, 0).context().result(), v1::RESULT_INVALID_REQUEST);
  EXPECT_EQ(Reconcile(0, 9).context().result(), v1::RESULT_INVALID_REQUEST);
}

TEST_F(TradeTest, ReconciliationRejectsTruncatedOrContradictoryHistory) {
  verdict = LegalityVerdict::Illegal;
  ASSERT_EQ(Submit(Submission(auth, 1)).context().result(), v1::RESULT_ILLEGAL_POKEMON);
  ASSERT_EQ(Submit(Submission(auth, 2)).context().result(), v1::RESULT_ILLEGAL_POKEMON);
  auto corrupt = [&](const char *sql, const std::string &value) {
    detail::Transaction transaction(database);
    database.Execute("SET LOCAL session_replication_role=replica");
    database.Parameters(sql, {detail::HexBytes(auth.save_id()), value});
    transaction.Commit();
  };
  auto rejected = [&](std::uint64_t after) {
    v1::ReconcileRequest request;
    *request.mutable_auth() = auth;
    request.set_after_event(after);
    request.set_limit(8);
    v1::ReconcileReply reply;
    reply.add_events();
    const auto status = service.Reconcile(nullptr, &request, &reply);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::UNAVAILABLE);
    EXPECT_EQ(reply.ByteSizeLong(), 0U);
  };
  const char *watermark = "UPDATE dswt.save_identities SET event_highwater=$2::numeric WHERE save_id=$1::uuid";
  // Privileged corruption bypasses normal guards; restore the original rows so
  // the independent audit replay still verifies the rest of the fixture.
  corrupt(watermark, "5");
  rejected(0); // Missing tail after a partial page.
  rejected(3); // Missing tail with no returned rows.
  corrupt(watermark, "1");
  rejected(0); // Retained events extend beyond this snapshot's highwater.
  corrupt(watermark, "3");
  corrupt("UPDATE dswt.trade_events SET sequence=$2::numeric WHERE save_id=$1::uuid AND sequence=2", "4");
  rejected(0); // Same row count, but an interior gap and an event beyond highwater.
  corrupt("UPDATE dswt.trade_events SET sequence=2 WHERE save_id=$1::uuid AND sequence=$2::numeric", "4");
  EXPECT_EQ(Reconcile().events_size(), 3);
}

TEST_F(TradeTest, CopiedSaveConcurrentSubmissionsHaveOneAuthoritativeWinner) {
  const auto first = Submission(auth), second = Submission(auth);
  auto one = std::async(std::launch::async, [&] { return Submit(first); });
  auto two = std::async(std::launch::async, [&] { return Submit(second); });
  const auto a = one.get(), b = two.get();
  EXPECT_TRUE((a.context().result() == v1::RESULT_OK && b.context().result() == v1::RESULT_COUNTER_MISMATCH) ||
              (b.context().result() == v1::RESULT_OK && a.context().result() == v1::RESULT_COUNTER_MISMATCH));
  EXPECT_EQ(Status().context().next_counter(), 2U);
  const auto retry = a.context().result() == v1::RESULT_OK ? first : second;
  EXPECT_EQ(Submit(retry).outcome().SerializeAsString(), Status().trade().SerializeAsString());
}

TEST_F(TradeTest, ConcurrentIdenticalSubmissionAndAcknowledgementReplayExactlyOnce) {
  const auto request = Submission(auth);
  auto one = std::async(std::launch::async, [&] { return Submit(request); });
  auto two = std::async(std::launch::async, [&] { return Submit(request); });
  auto a = one.get(), b = two.get();
  ASSERT_EQ(a.context().result(), v1::RESULT_OK);
  EXPECT_EQ(a.SerializeAsString(), b.SerializeAsString());
  const auto returned = Cancel(Cancellation(a.outcome()));
  const auto ack = Acknowledgement(returned.outcome());
  one = std::async(std::launch::async, [&] { return Ack(ack); });
  two = std::async(std::launch::async, [&] { return Ack(ack); });
  a = one.get(); b = two.get();
  EXPECT_EQ(a.context().result(), v1::RESULT_OK);
  EXPECT_EQ(a.SerializeAsString(), b.SerializeAsString());
  EXPECT_EQ(Status().context().next_counter(), 4U);
}

TEST_F(TradeTest, MissingRecoveryBarrierCannotPublishOrMutate) {
  TradeService no_recovery("", [&](const auto &) { ++validations; return LegalityDecision{LegalityVerdict::Legal, {}}; });
  const auto request = Submission(auth);
  v1::MutationReply reply;
  ASSERT_TRUE(no_recovery.Submit(nullptr, &request, &reply).ok());
  EXPECT_EQ(reply.context().result(), v1::RESULT_PROCESSING);
  EXPECT_EQ(reply.context().event_highwater(), 0U);
  EXPECT_EQ(validations.load(), 0);
  EXPECT_EQ(Status().context().next_counter(), 1U);
}

TEST_F(TradeTest, LocallyCommittedUncoveredOutcomeBlocksReadsAndDependentWrites) {
  std::atomic<unsigned> checks{0};
  TradeService delayed("", [&](const auto &) { return LegalityDecision{LegalityVerdict::Legal, {}}; },
      [&](auto) { return ++checks <= 2; });
  const auto request = Submission(auth);
  v1::MutationReply reply;
  ASSERT_TRUE(delayed.Submit(nullptr, &request, &reply).ok());
  ASSERT_EQ(reply.context().result(), v1::RESULT_PROCESSING);
  EXPECT_FALSE(reply.has_outcome());
  covered = false;
  EXPECT_EQ(Submit(request).context().result(), v1::RESULT_PROCESSING);
  EXPECT_EQ(Status().context().result(), v1::RESULT_PROCESSING);
  EXPECT_FALSE(Status().has_trade());
  EXPECT_EQ(Reconcile().events_size(), 0);
  covered = true;
  const auto published = Submit(request);
  ASSERT_EQ(published.context().result(), v1::RESULT_OK);
  EXPECT_EQ(validations.load(), 0);
  covered = false;
  EXPECT_EQ(Cancel(Cancellation(published.outcome())).context().result(), v1::RESULT_PROCESSING);
  covered = true;
  EXPECT_EQ(Status().context().next_counter(), 2U);
  EXPECT_EQ(Cancel(Cancellation(published.outcome())).context().result(), v1::RESULT_OK);
}

TEST_F(TradeTest, LaggingWalWatermarkCanFinishWithoutChasingRetryLocks) {
  auto position = [&] {
    return detail::Unsigned(Scalar("SELECT (pg_current_wal_flush_lsn() - '0/0'::pg_lsn)::text"));
  };
  std::atomic<std::uint64_t> recoverable{0};
  std::atomic<bool> first_check{true};
  TradeService lagged("", [](const auto &) { return LegalityDecision{LegalityVerdict::Legal, {}}; },
      [&](auto required) {
        if (first_check.exchange(false)) recoverable = required;
        return required <= recoverable.load();
      });
  const auto request = Submission(auth);
  v1::MutationReply reply;
  ASSERT_TRUE(lagged.Submit(nullptr, &request, &reply).ok());
  EXPECT_EQ(reply.context().result(), v1::RESULT_PROCESSING);
  // The old state was covered, so the new transition must commit locally even
  // though the admission transaction itself generated row-lock WAL.
  ASSERT_EQ(Status().context().next_counter(), 2U);
  EXPECT_FALSE(reply.has_outcome());
  recoverable = position();
  ASSERT_TRUE(lagged.Submit(nullptr, &request, &reply).ok());
  EXPECT_EQ(reply.context().result(), v1::RESULT_OK);
  EXPECT_EQ(reply.outcome().state(), v1::TRADE_QUEUED);
  const auto before = reply.outcome().SerializeAsString();
  for (int retry = 0; retry < 3; ++retry) {
    ASSERT_TRUE(lagged.Submit(nullptr, &request, &reply).ok());
    EXPECT_EQ(reply.context().result(), v1::RESULT_OK);
    EXPECT_EQ(reply.outcome().SerializeAsString(), before);
  }
}

TEST_F(TradeTest, CommitFailureRollsBackPayloadOwnershipOperationAndAudit) {
  const auto before = Scalar("SELECT count(*) FROM dswt.audit_journal");
  database.Execute("CREATE FUNCTION dswt.fail_lifecycle_test() RETURNS trigger LANGUAGE plpgsql AS $$ "
      "BEGIN RAISE EXCEPTION 'PRIVATE_SENTINEL'; END $$; "
      "CREATE CONSTRAINT TRIGGER fail_lifecycle_test AFTER INSERT ON dswt.operations "
      "DEFERRABLE INITIALLY DEFERRED FOR EACH ROW EXECUTE FUNCTION dswt.fail_lifecycle_test()");
  const auto request = Submission(auth);
  v1::MutationReply reply;
  const auto status = service.Submit(nullptr, &request, &reply);
  database.Execute("DROP TRIGGER fail_lifecycle_test ON dswt.operations; DROP FUNCTION dswt.fail_lifecycle_test()");
  EXPECT_EQ(status.error_code(), grpc::StatusCode::UNAVAILABLE);
  EXPECT_EQ(status.error_message().find("PRIVATE_SENTINEL"), std::string::npos);
  EXPECT_FALSE(reply.has_outcome());
  EXPECT_EQ(Status().context().next_counter(), 1U);
  EXPECT_EQ(Status().trade().state(), v1::TRADE_NONE);
  EXPECT_EQ(Scalar("SELECT count(*) FROM dswt.audit_journal"), before);
}

TEST_F(TradeTest, ActualGrpcServiceUsesTheSamePersistentContract) {
  grpc::ServerBuilder builder;
  int port = 0;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
  builder.RegisterService(&service);
  auto server = builder.BuildAndStart();
  ASSERT_TRUE(server);
  auto stub = v1::WonderTrade::NewStub(grpc::CreateChannel("127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials()));
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(3));
  const auto request = Submission(auth);
  v1::MutationReply reply;
  const auto status = stub->Submit(&context, request, &reply);
  server->Shutdown();
  ASSERT_TRUE(status.ok());
  EXPECT_EQ(reply.context().result(), v1::RESULT_OK);
  EXPECT_EQ(Submit(request).SerializeAsString(), reply.SerializeAsString());
}

int RestartScenario() {
  auto auth = Enroll(std::string(16, '\x91'));
  auto request = Submission(auth);
  request.mutable_context()->set_operation_id(std::string(16, '\x92'));
  TradeService service("", [](const auto &) { return LegalityDecision{LegalityVerdict::Legal, {}}; }, [](auto) { return true; });
  v1::MutationReply reply;
  auto status = service.Submit(nullptr, &request, &reply);
  return status.ok() && reply.context().result() == v1::RESULT_OK && reply.context().next_counter() == 2 &&
      reply.outcome().state() == v1::TRADE_QUEUED ? 0 : 1;
}

TEST_F(TradeTest, CallerExitAndNewProcessRecoverTheCommittedOutcome) {
  pid_t child = 0;
  char executable[] = "/proc/self/exe", mode[] = "--commit-and-exit";
  char *arguments[] = {executable, mode, nullptr};
  ASSERT_EQ(posix_spawn(&child, executable, nullptr, nullptr, arguments, environ), 0);
  int status = 0;
  ASSERT_EQ(waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(WEXITSTATUS(status), 0);
  EXPECT_EQ(RestartScenario(), 0);
}

} // namespace
} // namespace dswt::server

int main(int argc, char **argv) {
  if (argc == 2 && std::string(argv[1]) == "--commit-and-exit")
    std::_Exit(dswt::server::RestartScenario());
  if (argc == 2 && std::string(argv[1]) == "--verify-restart")
    return dswt::server::RestartScenario();
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
