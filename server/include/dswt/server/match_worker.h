#pragma once

#include <chrono>
#include <functional>

#include <grpcpp/support/status.h>

#include "trade.pb.h"

namespace dswt::server {

// One polling worker for MatchNext. An uncertain outcome reuses its operation ID,
// so a retry can only return that attempt's result; only a final outcome advances.
class MatchWorker {
public:
  using Call = std::function<grpc::Status(const v1::MatchRequest &, v1::MatchReply *)>;
  // Whether a compatible pair is queued, read without writing (MatcherService::PairAvailable).
  using Available = std::function<bool()>;
  static constexpr std::chrono::milliseconds kIdleDelay{500};
  MatchWorker(Call call, Available available) : call_(std::move(call)), available_(std::move(available)) {}
  // Runs one attempt and returns how long to wait before the next. Without a
  // compatible pair it calls nothing: every MatchNext retains an attempt row.
  std::chrono::milliseconds Poll();
  // An uncertain attempt must be resolved with its own ID before anything else.
  bool Pending() const { return !request_.operation_id().empty(); }

private:
  Call call_;
  Available available_;
  v1::MatchRequest request_;
};

} // namespace dswt::server
