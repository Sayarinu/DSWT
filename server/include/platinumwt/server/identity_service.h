#pragma once

#include "platinumwt/server/recovery_barrier.h"

#include <memory>
#include <string>

#include "trade.grpc.pb.h"

namespace platinumwt::server {

namespace detail { class RecoveryGate; class ServiceAdmission; }

class IdentityService final : public v1::SaveIdentity::Service {
public:
  explicit IdentityService(std::string connection_info, RecoveryBarrier recovery = {},
                           std::shared_ptr<detail::ServiceAdmission> admission = {});
  grpc::Status Enroll(grpc::ServerContext *, const v1::EnrollRequest *, v1::EnrollReply *) override;

private:
  std::string connection_info_;
  std::shared_ptr<detail::RecoveryGate> recovery_;
  std::shared_ptr<detail::ServiceAdmission> admission_;
};

} // namespace platinumwt::server
