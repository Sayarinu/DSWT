#include "platinumwt/server/legality_client.h"

#include <algorithm>
#include <stdexcept>
#include <string_view>

#include <grpcpp/grpcpp.h>
#include <openssl/evp.h>

#include "trade.grpc.pb.h"

namespace platinumwt::server {
namespace {
bool KnownReason(std::string_view reason) {
  constexpr std::string_view reasons[] = {
      "EGG_NOT_ALLOWED", "ILLEGAL_DATA", "ILLEGAL_ENCOUNTER", "ILLEGAL_MOVES",
      "ILLEGAL_RNG", "INCOMPATIBLE_FORM", "INVALID_SPECIES", "INVALID_STRUCTURE",
      "MAIL_NOT_ALLOWED"};
  return std::find(std::begin(reasons), std::end(reasons), reason) != std::end(reasons);
}
} // namespace

struct LegalityClient::Impl {
  std::unique_ptr<v1::PokemonLegality::Stub> stub;
  std::string policy;
  std::chrono::milliseconds timeout;
};

LegalityClient::LegalityClient(const std::string &address,
                               const std::string &expected_policy,
                               std::chrono::milliseconds timeout)
    : impl_(std::make_unique<Impl>()) {
  if (expected_policy.empty() || expected_policy.size() > 64 ||
      !std::all_of(expected_policy.begin(), expected_policy.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
      }) || timeout.count() < 1 || timeout > std::chrono::milliseconds(1500))
    throw std::invalid_argument("invalid legality policy or deadline");
  grpc::ChannelArguments limits;
  limits.SetMaxReceiveMessageSize(1024);
  limits.SetMaxSendMessageSize(256);
  limits.SetInt(GRPC_ARG_ENABLE_RETRIES, 0);
  impl_->stub = v1::PokemonLegality::NewStub(
      grpc::CreateCustomChannel(address, grpc::InsecureChannelCredentials(), limits));
  impl_->policy = expected_policy;
  impl_->timeout = timeout;
}

LegalityClient::~LegalityClient() = default;

LegalityDecision LegalityClient::CheckNewSubmission(
    const std::array<unsigned char, 136> &pk4) {
  std::array<unsigned char, 32> digest{};
  std::size_t digest_size = 0;
  if (EVP_Q_digest(nullptr, "SHA256", nullptr, pk4.data(), pk4.size(),
                   digest.data(), &digest_size) != 1 || digest_size != digest.size())
    return {};
  const std::string hash(reinterpret_cast<const char *>(digest.data()), digest.size());
  v1::ValidateRequest request;
  request.mutable_pokemon()->set_pk4(pk4.data(), pk4.size());
  request.mutable_pokemon()->set_sha256(hash);
  v1::ValidateReply reply;
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + impl_->timeout);
  context.set_wait_for_ready(false);
  auto status = impl_->stub->Validate(&context, request, &reply);
  if (!status.ok() || reply.policy_version() != impl_->policy || reply.sha256() != hash ||
      reply.reason_codes_size() > 8)
    return {};
  if (reply.legal())
    return reply.reason_codes().empty() ? LegalityDecision{LegalityVerdict::Legal, {}}
                                       : LegalityDecision{};
  if (reply.reason_codes().empty())
    return {};
  LegalityDecision decision{LegalityVerdict::Illegal, {}};
  for (const auto &reason : reply.reason_codes()) {
    if (!KnownReason(reason) || (!decision.reasons.empty() && decision.reasons.back() >= reason))
      return {};
    decision.reasons.push_back(reason);
  }
  return decision;
}

} // namespace platinumwt::server
