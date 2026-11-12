#include "dswt/server/legality_client.h"

#include <functional>
#include <atomic>
#include <thread>

#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include "trade.grpc.pb.h"

namespace dswt::server {
namespace {
constexpr char kPolicy[] = "pkhex-26.08.26-74b88906-dswt3";

class FakeValidator final : public v1::PokemonLegality::Service {
public:
  std::function<grpc::Status(grpc::ServerContext *, v1::ValidateReply *)> respond;
  std::atomic<int> calls{0};
  grpc::Status Validate(grpc::ServerContext *context, const v1::ValidateRequest *request,
                        v1::ValidateReply *reply) override {
    ++calls;
    EXPECT_EQ(request->pokemon().pk4().size(), 136U);
    EXPECT_EQ(request->pokemon().sha256().size(), 32U);
    EXPECT_EQ(request->pokemon().sha256(),
              std::string("\xb7\x07\x24\x15\x45\xa3\x46\x26\x5a\xab\x1f\xfb\x32\xff\x64\xb5"
                          "\x5b\xf8\xf8\xdc\x1b\x56\xa4\x6e\xf3\x3c\xe3\xd1\x5d\xb1\x1d\x33", 32));
    reply->set_legal(true);
    reply->set_destinations(kAllDestinations);
    reply->set_policy_version(kPolicy);
    reply->set_sha256(request->pokemon().sha256());
    return respond ? respond(context, reply) : grpc::Status::OK;
  }
};

class LegalityClientTest : public testing::Test {
protected:
  void SetUp() override {
    grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&validator);
    server = builder.BuildAndStart();
    ASSERT_TRUE(server);
    address = "127.0.0.1:" + std::to_string(port);
  }
  void TearDown() override { server->Shutdown(); }
  FakeValidator validator;
  std::unique_ptr<grpc::Server> server;
  std::string address;
  std::array<unsigned char, 136> payload{};
};

TEST_F(LegalityClientTest, AcceptsOnlyTheExpectedPolicyAndPayload) {
  LegalityClient client(address, kPolicy);
  auto result = client.CheckNewSubmission(payload);
  EXPECT_EQ(result.verdict, LegalityVerdict::Legal);
  EXPECT_EQ(result.destinations, kAllDestinations);
}

TEST_F(LegalityClientTest, ReportsGamesThatCanHoldTheOffer) {
  validator.respond = [](auto *, auto *reply) {
    reply->set_destinations(kDestinationPt | kDestinationHGSS);
    return grpc::Status::OK;
  };
  LegalityClient client(address, kPolicy);
  auto result = client.CheckNewSubmission(payload);
  EXPECT_EQ(result.verdict, LegalityVerdict::Legal);
  EXPECT_EQ(result.destinations, kDestinationPt | kDestinationHGSS);
}

TEST_F(LegalityClientTest, RetainsStableRejectionReasons) {
  validator.respond = [](auto *, auto *reply) {
    reply->set_legal(false);
    reply->clear_destinations();
    reply->add_reason_codes("ILLEGAL_ENCOUNTER");
    reply->add_reason_codes("ILLEGAL_MOVES");
    return grpc::Status::OK;
  };
  LegalityClient client(address, kPolicy);
  auto result = client.CheckNewSubmission(payload);
  EXPECT_EQ(result.verdict, LegalityVerdict::Illegal);
  EXPECT_EQ(result.reasons, (std::vector<std::string>{"ILLEGAL_ENCOUNTER", "ILLEGAL_MOVES"}));
  EXPECT_EQ(result.destinations, 0U);
}

TEST_F(LegalityClientTest, RejectsLegalOfferThatFitsNoGame) {
  validator.respond = [](auto *, auto *reply) {
    reply->set_legal(false);
    reply->clear_destinations();
    reply->add_reason_codes("INCOMPATIBLE_GAME");
    return grpc::Status::OK;
  };
  LegalityClient client(address, kPolicy);
  auto result = client.CheckNewSubmission(payload);
  EXPECT_EQ(result.verdict, LegalityVerdict::Illegal);
  EXPECT_EQ(result.reasons, std::vector<std::string>{"INCOMPATIBLE_GAME"});
}

TEST_F(LegalityClientTest, RejectsMalformedUnboundOrAmbiguousReplies) {
  std::vector<std::function<void(v1::ValidateReply *)>> corruptions = {
      [](auto *r) { r->clear_sha256(); },
      [](auto *r) { r->set_sha256(std::string(32, 'x')); },
      [](auto *r) { r->set_policy_version("new-policy"); },
      [](auto *r) { r->set_legal(false); r->clear_destinations(); },
      [](auto *r) { r->add_reason_codes("ILLEGAL_DATA"); },
      // Legal without destinations, as an older validator replies: never "all games".
      [](auto *r) { r->clear_destinations(); },
      [](auto *r) { r->set_destinations(8); },
      [](auto *r) { r->set_destinations(kAllDestinations | 0x80000000U); },
      [](auto *r) { r->set_legal(false); r->add_reason_codes("ILLEGAL_DATA"); },
      [](auto *r) { r->set_legal(false); r->clear_destinations(); r->add_reason_codes("UNKNOWN_REASON"); },
      [](auto *r) { r->set_legal(false); r->clear_destinations(); for (int i = 0; i < 9; ++i) r->add_reason_codes("ILLEGAL_DATA"); },
      [](auto *r) { r->set_legal(false); r->clear_destinations(); r->add_reason_codes("ILLEGAL_RNG"); r->add_reason_codes("ILLEGAL_DATA"); },
      [](auto *r) { r->set_policy_version(std::string(2048, 'x')); }};
  LegalityClient client(address, kPolicy);
  for (const auto &corrupt : corruptions) {
    validator.respond = [&](auto *, auto *reply) { corrupt(reply); return grpc::Status::OK; };
    EXPECT_EQ(client.CheckNewSubmission(payload).verdict, LegalityVerdict::Unavailable);
  }
}

TEST_F(LegalityClientTest, RpcFailuresAreUnavailableRatherThanIllegal) {
  LegalityClient client(address, kPolicy);
  for (auto code : {grpc::StatusCode::INVALID_ARGUMENT, grpc::StatusCode::FAILED_PRECONDITION,
                    grpc::StatusCode::UNAVAILABLE, grpc::StatusCode::RESOURCE_EXHAUSTED}) {
    validator.respond = [code](auto *, auto *) { return grpc::Status(code, "unavailable"); };
    EXPECT_EQ(client.CheckNewSubmission(payload).verdict, LegalityVerdict::Unavailable);
  }
  EXPECT_EQ(validator.calls.load(), 4);
}

TEST_F(LegalityClientTest, BoundsTheDeadlineAndDoesNotRetry) {
  validator.respond = [](auto *context, auto *) {
    while (!context->IsCancelled()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return grpc::Status(grpc::StatusCode::CANCELLED, "cancelled");
  };
  LegalityClient client(address, kPolicy, std::chrono::milliseconds(40));
  auto start = std::chrono::steady_clock::now();
  EXPECT_EQ(client.CheckNewSubmission(payload).verdict, LegalityVerdict::Unavailable);
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(1));
  EXPECT_EQ(validator.calls.load(), 1);
}

TEST_F(LegalityClientTest, StoppedValidatorIsUnavailable) {
  server->Shutdown();
  LegalityClient client(address, kPolicy);
  EXPECT_EQ(client.CheckNewSubmission(payload).verdict, LegalityVerdict::Unavailable);
}

TEST_F(LegalityClientTest, RejectsInvalidPolicyAndDeadlineConfiguration) {
  EXPECT_THROW(LegalityClient(address, ""), std::invalid_argument);
  EXPECT_THROW(LegalityClient(address, std::string(65, 'x')), std::invalid_argument);
  EXPECT_THROW(LegalityClient(address, "invalid policy"), std::invalid_argument);
  EXPECT_THROW(LegalityClient(address, kPolicy, std::chrono::milliseconds(0)), std::invalid_argument);
  EXPECT_THROW(LegalityClient(address, kPolicy, std::chrono::milliseconds(1501)), std::invalid_argument);
}
} // namespace
} // namespace dswt::server
