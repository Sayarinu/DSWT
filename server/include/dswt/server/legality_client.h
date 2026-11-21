#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dswt::server {

enum class LegalityVerdict { Legal, Illegal, Unavailable };

// Bits of LegalityDecision::destinations: the games able to hold the Pokémon.
// A game family's bit is 1 << (family - 1); see game_variant.h.
inline constexpr std::uint32_t kDestinationDP = 1;
inline constexpr std::uint32_t kDestinationPt = 2;
inline constexpr std::uint32_t kDestinationHGSS = 4;
inline constexpr std::uint32_t kDestinationBW = 8;
inline constexpr std::uint32_t kDestinationB2W2 = 16;
// Pokémon never cross generations: an offer's set lies within its owner's pool.
inline constexpr std::uint32_t kGenIVDestinations = kDestinationDP | kDestinationPt | kDestinationHGSS;
inline constexpr std::uint32_t kGenVDestinations = kDestinationBW | kDestinationB2W2;

// The payload format the validator reads (v1::PokemonFormat): the owner's generation.
enum class PokemonFormat { PK4 = 0, PK5 = 1 };

// The destinations a Pokémon of this format may name.
constexpr std::uint32_t FormatDestinations(PokemonFormat format) {
  return format == PokemonFormat::PK5 ? kGenVDestinations : kGenIVDestinations;
}

// The name screening rule offers are checked against (dswt.offer_screenings.rule).
// Rule 1 is the Nintendo Switch word list the validator applies to the nickname and
// original trainer's name. Raise it to screen every queued offer again.
inline constexpr int kNameRule = 1;
inline constexpr const char *kInappropriateName = "INAPPROPRIATE_NAME";

struct LegalityDecision {
  LegalityVerdict verdict = LegalityVerdict::Unavailable;
  std::vector<std::string> reasons;
  // Non-zero only for Legal. Zero never means "all games".
  std::uint32_t destinations = 0;
};

// What the player is told about an illegal verdict (v1::RefusalReason): the first of
// Egg, Mail, no other game, illegal, name that any reason names; zero if none is known.
// Illegal comes before the name because renaming would not make the Pokémon legal.
int RefusalFor(const std::vector<std::string> &reasons);

// Use only before accepting a NEW submission. Collection, cancellation,
// reconciliation and replay of an accepted operation need no validator.
class LegalityClient {
public:
  LegalityClient(const std::string &address, const std::string &expected_policy,
                 std::chrono::milliseconds timeout = std::chrono::milliseconds(1500));
  ~LegalityClient();
  LegalityClient(const LegalityClient &) = delete;
  LegalityClient &operator=(const LegalityClient &) = delete;

  // A reply naming a game outside the format's generation is malformed (Unavailable).
  LegalityDecision CheckNewSubmission(const std::array<unsigned char, 136> &data,
                                      PokemonFormat format = PokemonFormat::PK4);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace dswt::server
