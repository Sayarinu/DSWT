#pragma once

#include "platinumwt/server/legality_client.h"
#include "platinumwt/server/recovery_barrier.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "trade.grpc.pb.h"

namespace platinumwt::server {

using SubmissionValidator = std::function<LegalityDecision(const std::array<unsigned char, 136> &)>;

namespace detail { class RecoveryGate; class ServiceAdmission; }

class TradeService final : public v1::WonderTrade::Service {
public:
  TradeService(std::string connection_info, SubmissionValidator validator,
                RecoveryBarrier recovery = {}, std::shared_ptr<detail::ServiceAdmission> admission = {});

  grpc::Status Submit(grpc::ServerContext *, const v1::SubmitRequest *, v1::MutationReply *) override;
  grpc::Status Status(grpc::ServerContext *, const v1::StatusRequest *, v1::StatusReply *) override;
  grpc::Status Cancel(grpc::ServerContext *, const v1::CancelRequest *, v1::MutationReply *) override;
  grpc::Status Collect(grpc::ServerContext *, const v1::CollectRequest *, v1::CollectReply *) override;
  grpc::Status Acknowledge(grpc::ServerContext *, const v1::AcknowledgeRequest *, v1::MutationReply *) override;
  grpc::Status Reconcile(grpc::ServerContext *, const v1::ReconcileRequest *, v1::ReconcileReply *) override;

private:
  std::string connection_info_;
  SubmissionValidator validator_;
  std::shared_ptr<detail::RecoveryGate> recovery_;
  std::shared_ptr<detail::ServiceAdmission> admission_;
};

} // namespace platinumwt::server
