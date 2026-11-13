#include "game_variant.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace dswt::server::detail {
namespace {

constexpr unsigned kLanguages[] = {1, 2, 3, 4, 5, 7, 8}; // JPN ENG FRE ITA GER SPA KOR

TEST(GameVariantTest, LegacyCodeIsUsEnglishPlatinum) {
  EXPECT_TRUE(ValidVariant(kLegacyPlatinum));
  EXPECT_EQ(VariantFamily(kLegacyPlatinum), GameFamily::Platinum);
}

TEST(GameVariantTest, EveryGameLanguageAndRevisionHasAFamily) {
  struct Game {
    GameFamily family;
    unsigned version;
  };
  constexpr Game games[] = {{GameFamily::DiamondPearl, 10}, {GameFamily::DiamondPearl, 11},
                            {GameFamily::Platinum, 12}, {GameFamily::HeartGoldSoulSilver, 7},
                            {GameFamily::HeartGoldSoulSilver, 8}};
  for (const auto game : games)
    for (const auto language : kLanguages)
      for (const unsigned revision : {0U, 1U, 255U}) {
        const auto code = GameVariant(game.family, game.version, language, revision);
        EXPECT_EQ(VariantFamily(code), game.family) << code;
        EXPECT_EQ(code >> 24, static_cast<std::uint32_t>(game.family));
      }
  EXPECT_EQ(GameVariant(GameFamily::Platinum, 12, 2, 1), 0x020C0201U);
}

TEST(GameVariantTest, UnknownOrInconsistentCodesAreRejected) {
  for (const std::uint32_t code :
       {0U, 2U, 0x000C0200U, 0xFFFFFFFFU, 0x040C0200U, 0x020A0200U, 0x010C0200U, 0x030A0200U,
        0x02000200U, 0x020C0000U, 0x020C0600U, 0x020C0900U, 0x020CFF00U, 0x01090200U})
    EXPECT_FALSE(ValidVariant(code)) << code;
}

TEST(GameVariantTest, StoredVariantOnlyAcceptsItselfOrTheLegacyAlias) {
  const auto english = GameVariant(GameFamily::Platinum, 12, 2, 1);
  const auto french = GameVariant(GameFamily::Platinum, 12, 3, 0);
  const auto pearl = GameVariant(GameFamily::DiamondPearl, 11, 2, 0);
  EXPECT_TRUE(SameVariant(pearl, pearl));
  EXPECT_TRUE(SameVariant(kLegacyPlatinum, english));
  EXPECT_TRUE(SameVariant(english, kLegacyPlatinum));
  EXPECT_FALSE(SameVariant(english, GameVariant(GameFamily::Platinum, 12, 2, 0)));
  EXPECT_FALSE(SameVariant(kLegacyPlatinum, french));
  EXPECT_FALSE(SameVariant(french, kLegacyPlatinum));
  EXPECT_FALSE(SameVariant(kLegacyPlatinum, pearl));
  EXPECT_FALSE(SameVariant(pearl, GameVariant(GameFamily::DiamondPearl, 10, 2, 0)));
}

} // namespace
} // namespace dswt::server::detail
