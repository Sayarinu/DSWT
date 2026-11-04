#pragma once

#include <chrono>
#include <functional>

#include <grpcpp/support/status.h>

#include "trade.pb.h"

namespace platinumwt::server {

// One polling worker for MatchNext. An uncertain outcome reuses its operation ID,
// so a retry can only return that attempt's result; only a final outcome advances.
class MatchWorker {
public:
  using Call = std::function<grpc::Status(const v1::MatchRequest &, v1::MatchReply *)>;
  explicit MatchWorker(Call call) : call_(std::move(call)) {}
  // Runs one attempt and returns how long to wait before the next.
  std::chrono::milliseconds Poll();
  // An uncertain attempt must be resolved with its own ID before anything else.
  bool Pending() const { return !request_.operation_id().empty(); }

private:
  Call call_;
  v1::MatchRequest request_;
};

} // namespace platinumwt::server
