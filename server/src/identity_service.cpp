#include "platinumwt/server/identity_service.h"

#include "platinumwt/server/identity_store.h"
#include "admission.h"
#include "postgres.h"
#include "recovery_gate.h"
#include "trade_encoding.h"
#include "platinumwt/server/telemetry.h"

#include <openssl/crypto.h>

#include <algorithm>
#include <utility>

namespace platinumwt::server {
namespace {
struct CredentialBuffer {
  SaveCredential bytes{};
  ~CredentialBuffer() { OPENSSL_cleanse(bytes.data(), bytes.size()); }
};
} // namespace

IdentityService::IdentityService(std::string connection_info, RecoveryBarrier recovery,
                                 std::shared_ptr<detail::ServiceAdmission> admission)
    : connection_info_(std::move(connection_info)), recovery_(std::make_shared<detail::RecoveryGate>(std::move(recovery))),
      admission_(admission ? std::move(admission) : std::make_shared<detail::ServiceAdmission>()) {}

grpc::Status IdentityService::Enroll(grpc::ServerContext *context, const v1::EnrollRequest *request, v1::EnrollReply *reply) {
  RpcTelemetry trace(Operation::Enroll, context, [reply] { return reply->context().result(); }, request->enrollment_id());
  reply->Clear();
  if (context && context->IsCancelled()) return {grpc::StatusCode::CANCELLED, "request cancelled"};
  if (!detail::NonzeroBytes(request->enrollment_id(), 16) || !detail::NonzeroBytes(request->credential(), 32) || request->hints().size() > 64) {
    reply->mutable_context()->set_result(v1::RESULT_INVALID_REQUEST);
    return grpc::Status::OK;
  }
  try {
    auto permit = admission_->Acquire(detail::RequestKind::Enrollment);
    if (!permit) { Telemetry::Global().Refused(); return {grpc::StatusCode::RESOURCE_EXHAUSTED, "request budget unavailable"}; }
    IdentityId nonce{};
    CredentialBuffer credential;
    std::copy(request->enrollment_id().begin(), request->enrollment_id().end(), nonce.begin());
    std::copy(request->credential().begin(), request->credential().end(), credential.bytes.begin());
    const auto identity = [&] {
      IdentityStore store(connection_info_);
      return store.Enroll(nonce, credential.bytes);
    }();
    if (!identity) {
      reply->mutable_context()->set_result(v1::RESULT_CONFLICT);
      return grpc::Status::OK;
    }
    const std::string id(reinterpret_cast<const char *>(identity->data()), identity->size());
    if (!admission_->Authorize(*permit, id, request->credential())) {
      Telemetry::Global().Refused();
      return {grpc::StatusCode::RESOURCE_EXHAUSTED, "request budget unavailable"};
    }
    detail::Database database(connection_info_);
    detail::Transaction transaction(database, "BEGIN ISOLATION LEVEL REPEATABLE READ READ ONLY");
    auto row = database.Parameters("SELECT next_counter::text,event_highwater::text FROM platinumwt.save_identities WHERE save_id=$1::uuid",
        {detail::HexBytes(id)});
    if (PQntuples(row.get()) != 1) throw std::runtime_error("identity disappeared");
    const auto highwater = detail::Unsigned(PQgetvalue(row.get(), 0, 1));
    if (!recovery_->Covers(database, id, highwater)) {
      reply->mutable_context()->set_result(v1::RESULT_PROCESSING);
    } else {
      reply->set_save_id(id);
      reply->mutable_context()->set_result(v1::RESULT_OK);
      reply->mutable_context()->set_next_counter(detail::Unsigned(PQgetvalue(row.get(), 0, 0)));
      reply->mutable_context()->set_event_highwater(highwater);
    }
    transaction.Commit();
    return grpc::Status::OK;
  } catch (const std::exception &) {
    reply->Clear();
    return {grpc::StatusCode::UNAVAILABLE, "identity service unavailable"};
  }
}

} // namespace platinumwt::server
