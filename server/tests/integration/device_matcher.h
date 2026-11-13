#pragma once

#include "dswt/server/match_worker.h"

#include <grpcpp/grpcpp.h>

#include <chrono>
#include <memory>
#include <string>

#include "trade.grpc.pb.h"

// Drives a matcher endpoint over local gRPC for the disposable device fixture.
class DeviceMatcher {
public:
  DeviceMatcher(const std::string &address, dswt::server::MatchWorker::Available available)
      : stub_(dswt::v1::TradeMatcher::NewStub(
            grpc::CreateChannel(address, grpc::InsecureChannelCredentials()))),
        worker_([this](const auto &request, auto *reply) {
          grpc::ClientContext context;
          context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
          return stub_->MatchNext(&context, request, reply);
        }, std::move(available)) {}

  std::chrono::milliseconds Poll() { return worker_.Poll(); }

private:
  std::unique_ptr<dswt::v1::TradeMatcher::Stub> stub_;
  dswt::server::MatchWorker worker_;
};
