#include "dswt/server/match_worker.h"

#include <openssl/rand.h>

#include <stdexcept>

namespace dswt::server {

std::chrono::milliseconds MatchWorker::Poll() {
  if (!Pending()) {
    // An uncertain attempt is always resolved; a new one needs a compatible pair.
    if (!available_()) return kIdleDelay;
    unsigned char id[16];
    if (RAND_bytes(id, sizeof(id)) != 1) throw std::runtime_error("worker randomness unavailable");
    request_.set_operation_id(id, sizeof(id));
  }
  v1::MatchReply reply;
  const auto result = call_(request_, &reply);
  if (result.ok() && (reply.result() == v1::RESULT_OK || reply.result() == v1::RESULT_NOT_FOUND)) {
    request_.Clear();
    return std::chrono::milliseconds(reply.result() == v1::RESULT_OK ? 0 : 250);
  }
  return std::chrono::milliseconds(result.ok() ? 100 : 500);
}

} // namespace dswt::server
