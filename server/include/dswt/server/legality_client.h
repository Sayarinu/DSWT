#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dswt::server {

enum class LegalityVerdict { Legal, Illegal, Unavailable };

// Bits of LegalityDecision::destinations: Gen IV games able to hold the Pokémon.
inline constexpr std::uint32_t kDestinationDP = 1;
inline constexpr std::uint32_t kDestinationPt = 2;
inline constexpr std::uint32_t kDestinationHGSS = 4;
inline constexpr std::uint32_t kAllDestinations = kDestinationDP | kDestinationPt | kDestinationHGSS;

struct LegalityDecision {
  LegalityVerdict verdict = LegalityVerdict::Unavailable;
  std::vector<std::string> reasons;
  // Non-zero only for Legal. Zero never means "all games".
  std::uint32_t destinations = 0;
};

// Use only before accepting a NEW submission. Collection, cancellation,
// reconciliation and replay of an accepted operation need no validator.
class LegalityClient {
public:
  LegalityClient(const std::string &address, const std::string &expected_policy,
                 std::chrono::milliseconds timeout = std::chrono::milliseconds(1500));
  ~LegalityClient();
  LegalityClient(const LegalityClient &) = delete;
  LegalityClient &operator=(const LegalityClient &) = delete;

  LegalityDecision CheckNewSubmission(const std::array<unsigned char, 136> &pk4);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace dswt::server
