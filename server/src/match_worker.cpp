#include "dswt/server/wire_layout.h"
#include "dswt/server/match_worker.h"

#include <openssl/rand.h>

#include <stdexcept>

namespace dswt::server {

namespace {
constexpr auto kNoMatchDelay = std::chrono::milliseconds(250);
constexpr auto kRetryDelay = std::chrono::milliseconds(100);
} // namespace


std::chrono::milliseconds MatchWorker::Poll() {
  if (!Pending()) {
    // An uncertain attempt is always resolved; a new one needs a compatible pair.
    if (!available_()) return kIdleDelay;
    unsigned char id[wire::kIdSize];
    if (RAND_bytes(id, sizeof(id)) != 1) throw std::runtime_error("worker randomness unavailable");
    request_.set_operation_id(id, sizeof(id));
  }
  v1::MatchReply reply;
  const auto result = call_(request_, &reply);
  if (result.ok() && (reply.result() == v1::RESULT_OK || reply.result() == v1::RESULT_NOT_FOUND)) {
    request_.Clear();
    return reply.result() == v1::RESULT_OK ? std::chrono::milliseconds::zero() : kNoMatchDelay;
  }
  return result.ok() ? kRetryDelay : kIdleDelay;
}

} // namespace dswt::server
