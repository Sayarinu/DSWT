#pragma once

#include "dswt/server/recovery_barrier.h"

#include <memory>
#include <string>

#include "trade.grpc.pb.h"

namespace dswt::server {

namespace detail { class RecoveryGate; }

// Register only on the separately authorized worker endpoint, never the public
// game backend. Each call retains one pair or a definitive no-pair outcome.
class MatcherService final : public v1::TradeMatcher::Service {
public:
  explicit MatcherService(std::string connection_info, RecoveryBarrier recovery = {});
  grpc::Status MatchNext(grpc::ServerContext *, const v1::MatchRequest *, v1::MatchReply *) override;
  // Whether two owners have queued offers. This writes nothing, unlike MatchNext,
  // which retains every attempt. Failures read as false.
  bool PairAvailable() const;

private:
  std::string connection_info_;
  std::shared_ptr<detail::RecoveryGate> recovery_;
};

} // namespace dswt::server
