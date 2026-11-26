#pragma once

#include "dswt/server/trade_service.h"

#include <atomic>
#include <cstddef>
#include <string>

namespace dswt::server {

// Screens queued offers that have no screening under the current name rule
// (kNameRule), such as offers stored before the rule existed. The matcher pairs
// only offers that passed; the owner's next request hands back a refused offer.
// Only an INAPPROPRIATE_NAME verdict refuses: an accepted offer is never refused
// afterwards for legality, and an unavailable validator leaves the offer for later.
class OfferScreener {
public:
  // `rule` is the screening this records; tests screen again under a later rule.
  OfferScreener(std::string connection_info, SubmissionValidator validator, int rule = kNameRule);

  // Screen up to `limit` offers and return how many were recorded. It never holds
  // a database transaction while waiting on the validator, and setting `stopped`
  // ends a wait on a stalled database.
  static constexpr std::size_t kBatchSize = 16;
  std::size_t Poll(std::size_t limit = kBatchSize, const std::atomic<bool> *stopped = nullptr);

private:
  std::string connection_info_;
  SubmissionValidator validator_;
  int rule_;
};

} // namespace dswt::server
