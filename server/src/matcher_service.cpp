#include "dswt/server/matcher_service.h"

#include "postgres.h"
#include "recovery_gate.h"
#include "trade_encoding.h"
#include "dswt/server/telemetry.h"

#include <optional>
#include <stdexcept>
#include <utility>

namespace dswt::server {
namespace {

using detail::Database;
using detail::HexBytes;
using detail::UnhexBytes;

struct Offer {
  std::string owner, trade, asset;
  std::uint64_t highwater;
  // Decimal destination set and owner's game bit, as the selection read them.
  std::string destinations, game;
};

class SelectionChanged : public std::runtime_error {
public:
  SelectionChanged() : std::runtime_error("selection changed") {}
};

void Result(v1::MatchReply &reply, v1::ResultCode code) {
  reply.Clear();
  reply.set_result(code);
}

// Queued offers (o) of identities that can still record events (i), with the
// games able to hold each Pokémon (d.destinations) and the owner's variant (v).
const std::string kQueued =
    "dswt.offers o JOIN dswt.save_identities i USING(save_id) "
    "JOIN dswt.identity_variants v USING(save_id) JOIN dswt.offer_destinations d USING(trade_id) "
    "WHERE o.state=1 AND i.event_highwater<18446744073709551615 ";
// The destination bit of v's game family: DP 1, Pt 2, HGSS 4. Legacy code 1 is
// Platinum; structured codes carry family 1-3 in their top byte.
const std::string kGame = "(1<<((CASE WHEN v.variant=1 THEN 2 ELSE v.variant>>24 END)::integer-1))";
const std::string kCandidates =
    "(SELECT o.trade_id,o.save_id,d.destinations," + kGame + " AS game FROM " + kQueued + ")";
const std::string kSelected =
    "SELECT replace(i.save_id::text,'-',''),replace(o.trade_id::text,'-',''),d.destinations::text," + kGame + "::text ";
// Two offers are compatible when each owner's game can hold the other's Pokémon.
const std::string kAnchor = kSelected +
    "FROM " + kQueued + "AND NOT o.trade_id=ANY($1::uuid[]) "
    "AND EXISTS (SELECT 1 FROM " + kCandidates + " p WHERE p.save_id<>o.save_id "
    "AND d.destinations&p.game<>0 AND p.destinations&" + kGame + "<>0) "
    "ORDER BY (SELECT a.sequence FROM dswt.audit_journal a WHERE a.table_name='offers' "
    "AND a.before_image IS NULL AND a.row_key->>'trade_id'=o.trade_id::text) "
    "LIMIT 1 FOR UPDATE OF i SKIP LOCKED";
const std::string kPeer = kSelected +
    "FROM " + kQueued + "AND i.save_id<>$1::uuid AND d.destinations&$2::integer<>0 "
    "AND $3::integer&" + kGame + "<>0 ORDER BY random() LIMIT 1 FOR UPDATE OF i SKIP LOCKED";
const std::string kPairAvailable =
    "SELECT EXISTS (SELECT 1 FROM " + kCandidates + " a JOIN " + kCandidates + " b ON a.save_id<b.save_id "
    "AND a.destinations&b.game<>0 AND b.destinations&a.game<>0)";

// Recheck the offer after locking its owner: a cancellation can have committed
// while the selection snapshot was read.
Offer Locked(Database &database, const detail::Result &selected) {
  auto row = database.Parameters(
      "SELECT replace(o.offered_asset_id::text,'-',''),i.event_highwater::text "
      "FROM dswt.offers o JOIN dswt.save_identities i USING(save_id) "
      "WHERE o.save_id=$1::uuid AND o.trade_id=$2::uuid AND o.state=1 AND i.event_highwater<18446744073709551615",
      {PQgetvalue(selected.get(), 0, 0), PQgetvalue(selected.get(), 0, 1)});
  if (PQntuples(row.get()) == 0) throw SelectionChanged();
  return Offer{UnhexBytes(PQgetvalue(selected.get(), 0, 0)), UnhexBytes(PQgetvalue(selected.get(), 0, 1)),
      UnhexBytes(PQgetvalue(row.get(), 0, 0)), detail::Unsigned(PQgetvalue(row.get(), 0, 1)),
      PQgetvalue(selected.get(), 0, 2), PQgetvalue(selected.get(), 0, 3)};
}

// The oldest unlocked offer with a compatible counterpart anchors each pair; a
// random unlocked compatible peer avoids a deterministic partner schedule. An
// offer no other queued game can hold is passed over rather than waited on, and
// if every compatible peer of an anchor is locked the next anchor is tried.
std::optional<std::pair<Offer, Offer>> Select(Database &database) {
  std::string tried;
  for (;;) {
    auto anchor = database.Parameters(kAnchor.c_str(), {"{" + tried + "}"});
    if (PQntuples(anchor.get()) == 0) return std::nullopt;
    auto first = Locked(database, anchor);
    auto peer = database.Parameters(kPeer.c_str(), {HexBytes(first.owner), first.game, first.destinations});
    if (PQntuples(peer.get()) == 1) {
      auto second = Locked(database, peer);
      return std::pair{std::move(first), std::move(second)};
    }
    tried += (tried.empty() ? "" : ",") + HexBytes(first.trade);
  }
}

bool Prior(Database &database, const std::string &operation) {
  auto row = database.Parameters("SELECT 1 FROM dswt.matcher_attempts WHERE operation_id=$1::uuid", {HexBytes(operation)});
  return PQntuples(row.get()) == 1;
}

void Publish(Database &database, detail::RecoveryGate &recovery, const std::string &operation, v1::MatchReply &reply) {
  auto row = database.Parameters(
      "SELECT coalesce(replace(m.first_trade_id::text,'-',''),''),"
      "coalesce(replace(m.second_trade_id::text,'-',''),'') "
      "FROM dswt.matcher_attempts a LEFT JOIN dswt.matches m USING(match_id) WHERE a.operation_id=$1::uuid",
      {HexBytes(operation)});
  if (PQntuples(row.get()) != 1) throw std::runtime_error("worker outcome missing");
  if (!recovery.Covers(database, "operation" + operation, 1)) { Result(reply, v1::RESULT_PROCESSING); return; }
  const auto first = UnhexBytes(PQgetvalue(row.get(), 0, 0));
  Result(reply, first.empty() ? v1::RESULT_NOT_FOUND : v1::RESULT_OK);
  if (!first.empty()) {
    reply.set_first_trade_id(first);
    reply.set_second_trade_id(UnhexBytes(PQgetvalue(row.get(), 0, 1)));
  }
}

void Pair(Database &database, const Offer &first, const Offer &second, const std::string &operation) {
  const auto match = detail::RandomIdentifier();
  database.Parameters("INSERT INTO dswt.matches VALUES ($1::uuid,$2::uuid,"
      "least($3::uuid,$4::uuid),greatest($3::uuid,$4::uuid))",
      {HexBytes(match), HexBytes(operation), HexBytes(first.trade), HexBytes(second.trade)});
  for (const auto &owners : {std::pair{&first, &second}, std::pair{&second, &first}}) {
    const auto &owner = *owners.first, &peer = *owners.second;
    const auto receipt = detail::RandomIdentifier();
    const auto sequence = std::to_string(owner.highwater + 1);
    database.Parameters("INSERT INTO dswt.receipts VALUES ($1::uuid,$2::uuid,$3::uuid,$4::uuid,1,$5::uuid)",
        {HexBytes(receipt), HexBytes(owner.trade), HexBytes(owner.owner), HexBytes(peer.asset), HexBytes(match)});
    auto changed = database.Parameters("UPDATE dswt.offers SET state=2,receipt_id=$2::uuid,last_event=$3::numeric "
        "WHERE trade_id=$1::uuid AND state=1 RETURNING trade_id", {HexBytes(owner.trade), HexBytes(receipt), sequence});
    if (PQntuples(changed.get()) != 1) throw std::runtime_error("queued offer changed");
    changed = database.Parameters("UPDATE dswt.ownership SET location=2,save_id=$2::uuid,trade_id=$3::uuid,last_event=$4::numeric "
        "WHERE asset_id=$1::uuid AND save_id=$5::uuid AND trade_id=$6::uuid AND location=1 RETURNING asset_id",
        {HexBytes(peer.asset), HexBytes(owner.owner), HexBytes(owner.trade), sequence, HexBytes(peer.owner), HexBytes(peer.trade)});
    if (PQntuples(changed.get()) != 1) throw std::runtime_error("queued ownership changed");
    database.Parameters("INSERT INTO dswt.trade_events(save_id,sequence,kind,trade_id,asset_id,payload_sha256,receipt_id,state) "
        "SELECT $1::uuid,$2::numeric,3,$3::uuid,asset_id,sha256,$5::uuid,2 FROM dswt.payloads WHERE asset_id=$4::uuid",
        {HexBytes(owner.owner), sequence, HexBytes(owner.trade), HexBytes(peer.asset), HexBytes(receipt)});
  }
  database.Parameters("INSERT INTO dswt.matcher_attempts VALUES ($1::uuid,$2::uuid)", {HexBytes(operation), HexBytes(match)});
}

bool Retry(const detail::DatabaseError &error) {
  return error.SqlState() == "40001" || error.SqlState() == "40P01" || error.SqlState() == "55P03" ||
      (error.SqlState() == "23505" && (error.Constraint() == "matches_pkey" || error.Constraint() == "receipts_pkey"));
}

} // namespace

MatcherService::MatcherService(std::string connection_info, RecoveryBarrier recovery)
    : connection_info_(std::move(connection_info)), recovery_(std::make_shared<detail::RecoveryGate>(std::move(recovery))) {}

bool MatcherService::PairAvailable() const {
  try {
    Database database(connection_info_);
    // Only a compatible pair: an incompatible queue would otherwise poll forever.
    auto row = database.Execute(kPairAvailable.c_str());
    return std::string(PQgetvalue(row.get(), 0, 0)) == "t";
  } catch (const std::exception &) { return false; }
}

grpc::Status MatcherService::MatchNext(grpc::ServerContext *context, const v1::MatchRequest *request, v1::MatchReply *reply) {
  RpcTelemetry trace(Operation::Match, context, [reply] { return reply->result(); }, request->operation_id());
  reply->Clear();
  if (context && context->IsCancelled()) return {grpc::StatusCode::CANCELLED, "request cancelled"};
  if (!detail::NonzeroBytes(request->operation_id(), 16)) { Result(*reply, v1::RESULT_INVALID_REQUEST); return grpc::Status::OK; }
  try {
    for (int attempt = 0; attempt < 3; ++attempt) {
      try {
        Database database(connection_info_);
        detail::Transaction transaction(database);
        // Serialize short pair-selection transactions across replicas. Clients
        // never take this lock; busy workers return promptly and reuse their ID.
        auto lock = database.Execute("SELECT pg_try_advisory_xact_lock(578892145138)");
        if (std::string(PQgetvalue(lock.get(), 0, 0)) != "t") { Result(*reply, v1::RESULT_PROCESSING); return grpc::Status::OK; }
        if (Prior(database, request->operation_id())) {
          Telemetry::Global().Replay();
          transaction.Commit();
          Publish(database, *recovery_, request->operation_id(), *reply);
          return grpc::Status::OK;
        }
        if (!recovery_->Covers(database, "operation" + request->operation_id(), 0)) {
          Result(*reply, v1::RESULT_PROCESSING); return grpc::Status::OK;
        }
        const auto pair = Select(database);
        if (pair) {
          const auto *first = &pair->first, *second = &pair->second;
          if (!recovery_->Covers(database, "identity" + first->owner, first->highwater) ||
              !recovery_->Covers(database, "identity" + second->owner, second->highwater)) {
            Result(*reply, v1::RESULT_PROCESSING); return grpc::Status::OK;
          }
          Pair(database, *first, *second, request->operation_id());
        } else {
          database.Parameters("INSERT INTO dswt.matcher_attempts VALUES ($1::uuid,NULL)", {HexBytes(request->operation_id())});
        }
        transaction.Commit();
        Publish(database, *recovery_, request->operation_id(), *reply);
        trace.Trade(reply->first_trade_id());
        return grpc::Status::OK;
      } catch (const SelectionChanged &) {
        if (attempt == 2) { Result(*reply, v1::RESULT_PROCESSING); return grpc::Status::OK; }
      } catch (const detail::DatabaseError &error) {
        if (!Retry(error) || attempt == 2) throw;
      }
    }
  } catch (const std::exception &) {
    reply->Clear();
    return {grpc::StatusCode::UNAVAILABLE, "matcher unavailable"};
  }
  return {grpc::StatusCode::UNAVAILABLE, "matcher unavailable"};
}

} // namespace dswt::server
