#pragma once

#include <cstdint>

// The enrollment build u32 names a save's game variant; see protocol/README.md.
// Structured codes pack family << 24 | version << 16 | language << 8 | revision
// with the games' own version and language IDs. Code 1 is the legacy alias for
// US English Platinum that released clients send.
namespace dswt::server::detail {

enum class GameFamily : std::uint8_t { None = 0, DiamondPearl = 1, Platinum = 2, HeartGoldSoulSilver = 3 };

inline constexpr std::uint32_t kLegacyPlatinum = 1;

constexpr std::uint32_t GameVariant(GameFamily family, unsigned version, unsigned language, unsigned revision) {
  return (static_cast<std::uint32_t>(family) << 24) | ((version & 0xFF) << 16) | ((language & 0xFF) << 8) | (revision & 0xFF);
}

// None for an unknown or inconsistent code; the matcher groups trades by this.
constexpr GameFamily VariantFamily(std::uint32_t code) {
  if (code == kLegacyPlatinum) return GameFamily::Platinum;
  const auto family = code >> 24, version = (code >> 16) & 0xFF, language = (code >> 8) & 0xFF;
  const bool game = (family == 1 && (version == 10 || version == 11)) || (family == 2 && version == 12) ||
      (family == 3 && (version == 7 || version == 8));
  const bool known_language = language >= 1 && language <= 8 && language != 6;
  return game && known_language ? static_cast<GameFamily>(family) : GameFamily::None;
}

constexpr bool ValidVariant(std::uint32_t code) { return VariantFamily(code) != GameFamily::None; }

// A stored variant never changes. The legacy alias also matches an explicit
// English Platinum code, so a client upgraded mid-enrollment keeps its identity.
constexpr bool SameVariant(std::uint32_t stored, std::uint32_t requested) {
  const auto english_platinum = [](std::uint32_t code) {
    return code == kLegacyPlatinum || ((code >> 16) == 0x020C && ((code >> 8) & 0xFF) == 2);
  };
  return stored == requested || (english_platinum(stored) && english_platinum(requested) &&
      (stored == kLegacyPlatinum || requested == kLegacyPlatinum));
}

} // namespace dswt::server::detail
