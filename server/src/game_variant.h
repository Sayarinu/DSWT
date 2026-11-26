#pragma once

#include "dswt/server/legality_client.h"

#include <cstdint>

// The enrollment build u32 names a save's game variant; see https://github.com/Sayarinu/DSWT/wiki/Protocol.
// Structured codes pack family << kFamilyShift | version << kVersionShift | language << kLanguageShift | revision
// with the games' own version and language IDs. Code 1 is the legacy alias for
// US English Platinum that released clients send.
namespace dswt::server::detail {

enum class GameFamily : std::uint8_t {
  None = 0, DiamondPearl = 1, Platinum = 2, HeartGoldSoulSilver = 3, BlackWhite = 4, Black2White2 = 5
};

enum GameVersion : unsigned {
  HeartGold = 7, SoulSilver = 8, Diamond = 10, Pearl = 11, Platinum = 12,
  White = 20, Black = 21, White2 = 22, Black2 = 23
};
inline constexpr unsigned kFamilyShift = 24, kVersionShift = 16, kLanguageShift = 8;
inline constexpr unsigned kJapanese = 1, kEnglish = 2, kUnusedLanguage = 6, kKorean = 8;
inline constexpr std::uint32_t kLegacyPlatinum = 1;

constexpr std::uint32_t GameVariant(GameFamily family, unsigned version, unsigned language, unsigned revision) {
  return (static_cast<std::uint32_t>(family) << kFamilyShift) | ((version & UINT8_MAX) << kVersionShift) | ((language & UINT8_MAX) << kLanguageShift) | (revision & UINT8_MAX);
}

// None for an unknown or inconsistent code; the matcher groups trades by this.
constexpr GameFamily VariantFamily(std::uint32_t code) {
  if (code == kLegacyPlatinum) return GameFamily::Platinum;
  const auto family = static_cast<GameFamily>(code >> kFamilyShift);
  const auto version = (code >> kVersionShift) & UINT8_MAX, language = (code >> kLanguageShift) & UINT8_MAX;
  const bool game = (family == GameFamily::DiamondPearl && (version == Diamond || version == Pearl)) || (family == GameFamily::Platinum && version == Platinum) ||
      (family == GameFamily::HeartGoldSoulSilver && (version == HeartGold || version == SoulSilver)) || (family == GameFamily::BlackWhite && (version == White || version == Black)) ||
      (family == GameFamily::Black2White2 && (version == White2 || version == Black2));
  const bool known_language = language >= kJapanese && language <= kKorean && language != kUnusedLanguage;
  return game && known_language ? family : GameFamily::None;
}

constexpr bool ValidVariant(std::uint32_t code) { return VariantFamily(code) != GameFamily::None; }

// Black/White and Black 2/White 2 saves hold PK5; the rest hold PK4.
constexpr PokemonFormat FamilyFormat(GameFamily family) {
  return family == GameFamily::BlackWhite || family == GameFamily::Black2White2 ? PokemonFormat::PK5
                                                                                 : PokemonFormat::PK4;
}

// A stored variant never changes. The legacy alias also matches an explicit
// English Platinum code, so a client upgraded mid-enrollment keeps its identity.
constexpr bool SameVariant(std::uint32_t stored, std::uint32_t requested) {
  const auto english_platinum = [](std::uint32_t code) {
    return code == kLegacyPlatinum || ((code >> kLanguageShift) == (GameVariant(GameFamily::Platinum, Platinum, kEnglish, 0) >> kLanguageShift));
  };
  return stored == requested || (english_platinum(stored) && english_platinum(requested) &&
      (stored == kLegacyPlatinum || requested == kLegacyPlatinum));
}

} // namespace dswt::server::detail
