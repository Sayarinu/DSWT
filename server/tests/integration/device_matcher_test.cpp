#include "device_matcher.h"

#include <gtest/gtest.h>
#include <vector>

namespace {
using namespace dswt;

TEST(DeviceMatcherTest, UncertainOutcomesReuseIdAndOnlyFinalOutcomesAdvance) {
  class Endpoint : public v1::TradeMatcher::Service {
  public:
    std::vector<std::string> ids;
    grpc::Status MatchNext(grpc::ServerContext *, const v1::MatchRequest *request, v1::MatchReply *reply) override {
      ids.push_back(request->operation_id());
      if (ids.size() == 1) return {grpc::StatusCode::UNAVAILABLE, "lost reply"};
      if (ids.size() == 2) reply->set_result(v1::RESULT_PROCESSING);
      else if (ids.size() == 3) reply->set_result(v1::RESULT_OK);
      else reply->set_result(v1::RESULT_NOT_FOUND);
      return grpc::Status::OK;
    }
  } endpoint;
  grpc::ServerBuilder builder;
  int port = 0;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
  builder.RegisterService(&endpoint);
  auto server = builder.BuildAndStart();
  ASSERT_TRUE(server);
  DeviceMatcher worker("127.0.0.1:" + std::to_string(port));
  EXPECT_GT(worker.Poll().count(), 0);
  EXPECT_GT(worker.Poll().count(), 0);
  EXPECT_EQ(worker.Poll().count(), 0);
  EXPECT_GT(worker.Poll().count(), 0);
  EXPECT_GT(worker.Poll().count(), 0);
  server->Shutdown();
  ASSERT_EQ(endpoint.ids.size(), 5U);
  EXPECT_EQ(endpoint.ids[0].size(), 16U);
  EXPECT_NE(endpoint.ids[0], std::string(16, '\0'));
  EXPECT_EQ(endpoint.ids[0], endpoint.ids[1]);
  EXPECT_EQ(endpoint.ids[1], endpoint.ids[2]);
  EXPECT_NE(endpoint.ids[2], endpoint.ids[3]);
  EXPECT_NE(endpoint.ids[3], endpoint.ids[4]);
}
} // namespace
