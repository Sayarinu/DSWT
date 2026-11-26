#include "dswt/server/wire_layout.h"
#include "dswt/server/offer_screener.h"

#include "bounded_query.h"
#include "game_variant.h"
#include "postgres.h"
#include "trade_encoding.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace dswt::server {

using detail::Database;
using detail::HexBytes;
using detail::UnhexBytes;

OfferScreener::OfferScreener(std::string connection_info, SubmissionValidator validator, int rule)
    : connection_info_(std::move(connection_info)), validator_(std::move(validator)), rule_(rule) {}

std::size_t OfferScreener::Poll(std::size_t limit, const std::atomic<bool> *stopped) {
  struct Pending { std::string trade, pk4; PokemonFormat format; };
  std::vector<Pending> pending;
  static const std::atomic<bool> running{false};
  // A bounded read, so neither a stalled database nor shutdown waits on it. Both
  // numbers are integers this process chose.
  const auto sql = "SELECT replace(o.trade_id::text,'-',''),encode(p.pk4,'hex'),v.variant::text FROM dswt.offers o "
      "JOIN dswt.payloads p ON p.asset_id=o.offered_asset_id JOIN dswt.identity_variants v USING(save_id) "
      "WHERE o.state=1 AND NOT EXISTS ("
      "SELECT 1 FROM dswt.offer_screenings s WHERE s.trade_id=o.trade_id AND s.rule=" + std::to_string(rule_) + ") "
      "ORDER BY o.trade_id LIMIT " + std::to_string(limit);
  auto rows = detail::BoundedQuery(connection_info_, sql.c_str(), "dswt-screener", stopped ? *stopped : running);
  if (!rows) return 0;
  for (int row = 0; row < PQntuples(rows.get()); ++row) {
    // The payload is read in its owner's format, as at Submit.
    const auto family = detail::VariantFamily(static_cast<std::uint32_t>(detail::Unsigned(PQgetvalue(rows.get(), row, 2))));
    if (family == detail::GameFamily::None) continue;
    pending.push_back({UnhexBytes(PQgetvalue(rows.get(), row, 0)), UnhexBytes(PQgetvalue(rows.get(), row, 1)),
                       detail::FamilyFormat(family)});
  }
  std::size_t recorded = 0;
  for (const auto &offer : pending) {
    if ((stopped && *stopped) || offer.pk4.size() != wire::kPk4Size) continue;
    std::array<unsigned char, wire::kPk4Size> data{};
    std::copy(offer.pk4.begin(), offer.pk4.end(), data.begin());
    const auto verdict = validator_ ? validator_(data, offer.format) : LegalityDecision{};
    if (verdict.verdict == LegalityVerdict::Unavailable) continue;
    const bool refused = verdict.verdict == LegalityVerdict::Illegal &&
        std::find(verdict.reasons.begin(), verdict.reasons.end(), kInappropriateName) != verdict.reasons.end();
    // The offer may have been cancelled meanwhile; a screening of it is still true.
    Database database(connection_info_);
    database.Parameters("INSERT INTO dswt.offer_screenings VALUES ($1::uuid,$2::smallint,$3::boolean) "
        "ON CONFLICT DO NOTHING", {HexBytes(offer.trade), std::to_string(rule_), refused ? "true" : "false"});
    ++recorded;
  }
  return recorded;
}

} // namespace dswt::server
