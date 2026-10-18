#pragma once

#include "platinumwt/server/legality_client.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "trade.grpc.pb.h"

namespace platinumwt::server {

// The barrier must attest that an off-site, independently recoverable WAL
// prefix covers the supplied PostgreSQL flush position in the current cluster
// and timeline. It must not block; restart the service after recovery.
// An absent barrier never exposes a final result or permits a dependent write.
using RecoveryBarrier = std::function<bool(std::uint64_t)>;
using SubmissionValidator = std::function<LegalityDecision(const std::array<unsigned char, 136> &)>;

struct VisibilityCache;

class TradeService final : public v1::WonderTrade::Service {
public:
  TradeService(std::string connection_info, SubmissionValidator validator,
                RecoveryBarrier recovery = {});

  grpc::Status Submit(grpc::ServerContext *, const v1::SubmitRequest *, v1::MutationReply *) override;
  grpc::Status Status(grpc::ServerContext *, const v1::StatusRequest *, v1::StatusReply *) override;
  grpc::Status Cancel(grpc::ServerContext *, const v1::CancelRequest *, v1::MutationReply *) override;
  grpc::Status Collect(grpc::ServerContext *, const v1::CollectRequest *, v1::CollectReply *) override;
  grpc::Status Acknowledge(grpc::ServerContext *, const v1::AcknowledgeRequest *, v1::MutationReply *) override;
  grpc::Status Reconcile(grpc::ServerContext *, const v1::ReconcileRequest *, v1::ReconcileReply *) override;

private:
  std::string connection_info_;
  SubmissionValidator validator_;
  RecoveryBarrier recovery_;
  std::shared_ptr<VisibilityCache> visibility_;
};

} // namespace platinumwt::server
