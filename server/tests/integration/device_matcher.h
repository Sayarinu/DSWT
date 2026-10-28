#pragma once

#include "trade.grpc.pb.h"

#include <grpcpp/grpcpp.h>
#include <openssl/rand.h>

#include <chrono>
#include <stdexcept>

// Local acceptance worker. Keep an uncertain call's ID until its final outcome.
class DeviceMatcher {
public:
  explicit DeviceMatcher(const std::string &address)
      : stub_(platinumwt::v1::TradeMatcher::NewStub(
            grpc::CreateChannel(address, grpc::InsecureChannelCredentials()))) {}

  std::chrono::milliseconds Poll() {
    using namespace platinumwt::v1;
    if (request_.operation_id().empty()) {
      unsigned char id[16];
      if (RAND_bytes(id, sizeof(id)) != 1) throw std::runtime_error("worker randomness unavailable");
      request_.set_operation_id(id, sizeof(id));
    }
    MatchReply reply;
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
    const auto result = stub_->MatchNext(&context, request_, &reply);
    if (result.ok() && (reply.result() == RESULT_OK || reply.result() == RESULT_NOT_FOUND)) {
      request_.Clear();
      return std::chrono::milliseconds(reply.result() == RESULT_OK ? 0 : 250);
    }
    return std::chrono::milliseconds(result.ok() ? 100 : 500);
  }

private:
  std::unique_ptr<platinumwt::v1::TradeMatcher::Stub> stub_;
  platinumwt::v1::MatchRequest request_;
};
