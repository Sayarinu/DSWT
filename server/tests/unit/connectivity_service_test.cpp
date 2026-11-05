#include "dswt/server/connectivity_service.h"

#include <gtest/gtest.h>

namespace dswt::server {
namespace {

TEST(ConnectivityServiceTest, ReturnsProtocolAcknowledgement) {
  ConnectivityService service;
  grpc::ServerContext context;
  google::protobuf::Empty request;
  v1::PingResponse response;

  const auto status = service.Ping(&context, &request, &response);

  EXPECT_TRUE(status.ok());
  EXPECT_EQ(response.acknowledgement(), "DSWT_ACK_V1\n");
}

} // namespace
} // namespace dswt::server
