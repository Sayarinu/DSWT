#include "dswt/server/wire_layout.h"
#include "dswt/server/legality_client.h"
#include "dswt/server/telemetry.h"

#include <algorithm>
#include <stdexcept>
#include <string_view>

#include <grpcpp/grpcpp.h>
#include <openssl/evp.h>

#include "trade.grpc.pb.h"

namespace dswt::server {
namespace {
constexpr std::size_t kMaximumPolicyLength = 64;
constexpr int kMaximumReplyBytes = 1024;
constexpr int kMaximumRequestBytes = 256;
constexpr int kMaximumReasons = 8;
bool KnownReason(std::string_view reason) {
  constexpr std::string_view reasons[] = {
      "EGG_NOT_ALLOWED", "ILLEGAL_DATA", "ILLEGAL_ENCOUNTER", "ILLEGAL_MOVES",
      "ILLEGAL_RNG", "INAPPROPRIATE_NAME", "INCOMPATIBLE_FORM", "INCOMPATIBLE_GAME", "INVALID_SPECIES", "INVALID_STRUCTURE",
      "MAIL_NOT_ALLOWED"};
  return std::find(std::begin(reasons), std::end(reasons), reason) != std::end(reasons);
}
} // namespace

int RefusalFor(const std::vector<std::string> &reasons) {
  const auto has = [&](auto match) { return std::any_of(reasons.begin(), reasons.end(), match); };
  const auto starts = [](std::string_view prefix) {
    return [prefix](const std::string &reason) { return std::string_view(reason).substr(0, prefix.size()) == prefix; };
  };
  const auto is = [](std::string_view name) { return [name](const std::string &reason) { return reason == name; }; };
  if (has(is("EGG_NOT_ALLOWED"))) return v1::REFUSAL_EGG;
  if (has(is("MAIL_NOT_ALLOWED"))) return v1::REFUSAL_MAIL;
  if (has(starts("INCOMPATIBLE_"))) return v1::REFUSAL_INCOMPATIBLE;
  if (has(starts("ILLEGAL_")) || has(starts("INVALID_"))) return v1::REFUSAL_ILLEGAL;
  if (has(is(kInappropriateName))) return v1::REFUSAL_NAME;
  return v1::REFUSAL_UNSPECIFIED;
}

struct LegalityClient::Impl {
  std::unique_ptr<v1::PokemonLegality::Stub> stub;
  std::string policy;
  std::chrono::milliseconds timeout;
};

LegalityClient::LegalityClient(const std::string &address,
                               const std::string &expected_policy,
                               std::chrono::milliseconds timeout)
    : impl_(std::make_unique<Impl>()) {
  if (expected_policy.empty() || expected_policy.size() > kMaximumPolicyLength ||
      !std::all_of(expected_policy.begin(), expected_policy.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
      }) || timeout.count() < 1 || timeout > kMaximumLegalityTimeout)
    throw std::invalid_argument("invalid legality policy or deadline");
  grpc::ChannelArguments limits;
  limits.SetMaxReceiveMessageSize(kMaximumReplyBytes);
  limits.SetMaxSendMessageSize(kMaximumRequestBytes);
  limits.SetInt(GRPC_ARG_ENABLE_RETRIES, 0);
  impl_->stub = v1::PokemonLegality::NewStub(
      grpc::CreateCustomChannel(address, grpc::InsecureChannelCredentials(), limits));
  impl_->policy = expected_policy;
  impl_->timeout = timeout;
}

LegalityClient::~LegalityClient() = default;

LegalityDecision LegalityClient::CheckNewSubmission(
    const std::array<unsigned char, wire::kPk4Size> &pk4, PokemonFormat format) {
  unsigned verdict = v1::RESULT_UNAVAILABLE;
  RpcTelemetry trace(Operation::Legality, nullptr, [&] { return verdict; });
  std::array<unsigned char, wire::kDigestSize> digest{};
  std::size_t digest_size = 0;
  if (EVP_Q_digest(nullptr, "SHA256", nullptr, pk4.data(), pk4.size(),
                   digest.data(), &digest_size) != 1 || digest_size != digest.size())
    return {};
  const std::string hash(reinterpret_cast<const char *>(digest.data()), digest.size());
  v1::ValidateRequest request;
  request.mutable_pokemon()->set_pk4(pk4.data(), pk4.size());
  request.mutable_pokemon()->set_sha256(hash);
  request.set_format(format == PokemonFormat::PK5 ? v1::FORMAT_PK5 : v1::FORMAT_PK4);
  v1::ValidateReply reply;
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + impl_->timeout);
  context.set_wait_for_ready(false);
  auto status = impl_->stub->Validate(&context, request, &reply);
  if (!status.ok() || reply.policy_version() != impl_->policy || reply.sha256() != hash ||
      reply.format() != request.format() ||
      reply.reason_codes_size() > kMaximumReasons || (reply.destinations() & ~FormatDestinations(format)) != 0)
    return {};
  if (reply.legal()) {
    // An empty destination set (as from a validator predating the field) is
    // malformed, never "every game".
    if (!reply.reason_codes().empty() || reply.destinations() == 0) return {};
    verdict = v1::RESULT_OK;
    return {LegalityVerdict::Legal, {}, reply.destinations()};
  }
  if (reply.reason_codes().empty() || reply.destinations() != 0)
    return {};
  LegalityDecision decision{LegalityVerdict::Illegal, {}};
  for (const auto &reason : reply.reason_codes()) {
    if (!KnownReason(reason) || (!decision.reasons.empty() && decision.reasons.back() >= reason))
      return {};
    decision.reasons.push_back(reason);
  }
  verdict = v1::RESULT_ILLEGAL_POKEMON;
  return decision;
}

} // namespace dswt::server
