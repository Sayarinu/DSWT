#pragma once

#include "platinumwt/server/match_worker.h"

#include <grpcpp/grpcpp.h>

#include <chrono>
#include <memory>
#include <string>

#include "trade.grpc.pb.h"

// Drives a matcher endpoint over local gRPC for the disposable device fixture.
class DeviceMatcher {
public:
  explicit DeviceMatcher(const std::string &address)
      : stub_(platinumwt::v1::TradeMatcher::NewStub(
            grpc::CreateChannel(address, grpc::InsecureChannelCredentials()))),
        worker_([this](const auto &request, auto *reply) {
          grpc::ClientContext context;
          context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
          return stub_->MatchNext(&context, request, reply);
        }) {}

  std::chrono::milliseconds Poll() { return worker_.Poll(); }

private:
  std::unique_ptr<platinumwt::v1::TradeMatcher::Stub> stub_;
  platinumwt::server::MatchWorker worker_;
};
