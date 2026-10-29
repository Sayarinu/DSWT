#include "platinumwt/server/matcher_service.h"

#include "postgres.h"
#include "recovery_gate.h"
#include "trade_encoding.h"
#include "platinumwt/server/telemetry.h"

#include <optional>
#include <stdexcept>
#include <utility>

namespace platinumwt::server {
namespace {

using detail::Database;
using detail::HexBytes;
using detail::UnhexBytes;

struct Offer {
  std::string owner, trade, asset;
  std::uint64_t highwater;
};

class SelectionChanged : public std::runtime_error {
public:
  SelectionChanged() : std::runtime_error("selection changed") {}
};

void Result(v1::MatchReply &reply, v1::ResultCode code) {
  reply.Clear();
  reply.set_result(code);
}

// The oldest unlocked identity anchors each pair; a random unlocked peer avoids
// a deterministic partner schedule. Recheck the offer after locking its owner:
// a cancellation can have committed while the selection snapshot was read.
std::optional<Offer> Select(Database &database, const std::string &excluded = {}) {
  const std::string common =
      "SELECT replace(i.save_id::text,'-',''),replace(o.trade_id::text,'-','') "
      "FROM platinumwt.offers o JOIN platinumwt.save_identities i USING(save_id) ";
  const std::string admission =
      "JOIN platinumwt.audit_journal a ON a.table_name='offers' AND a.before_image IS NULL "
      "AND a.row_key->>'trade_id'=o.trade_id::text ";
  const std::string filter =
      "WHERE o.state=1 AND i.event_highwater<18446744073709551615 "
      "AND ($1='' OR i.save_id<>nullif($1,'')::uuid) ";
  const auto sql = common + (excluded.empty() ? admission : "") + filter +
      (excluded.empty() ? "ORDER BY a.sequence " : "ORDER BY random() ") +
      "LIMIT 1 FOR UPDATE OF i SKIP LOCKED";
  auto selected = database.Parameters(sql.c_str(), {HexBytes(excluded)});
  if (PQntuples(selected.get()) == 0) return std::nullopt;
  auto row = database.Parameters(
      "SELECT replace(o.offered_asset_id::text,'-',''),i.event_highwater::text "
      "FROM platinumwt.offers o JOIN platinumwt.save_identities i USING(save_id) "
      "WHERE o.save_id=$1::uuid AND o.trade_id=$2::uuid AND o.state=1 AND i.event_highwater<18446744073709551615",
      {PQgetvalue(selected.get(), 0, 0), PQgetvalue(selected.get(), 0, 1)});
  if (PQntuples(row.get()) == 0) throw SelectionChanged();
  return Offer{UnhexBytes(PQgetvalue(selected.get(), 0, 0)), UnhexBytes(PQgetvalue(selected.get(), 0, 1)),
      UnhexBytes(PQgetvalue(row.get(), 0, 0)), detail::Unsigned(PQgetvalue(row.get(), 0, 1))};
}

bool Prior(Database &database, const std::string &operation) {
  auto row = database.Parameters("SELECT 1 FROM platinumwt.matcher_attempts WHERE operation_id=$1::uuid", {HexBytes(operation)});
  return PQntuples(row.get()) == 1;
}

void Publish(Database &database, detail::RecoveryGate &recovery, const std::string &operation, v1::MatchReply &reply) {
  auto row = database.Parameters(
      "SELECT coalesce(replace(m.first_trade_id::text,'-',''),''),"
      "coalesce(replace(m.second_trade_id::text,'-',''),'') "
      "FROM platinumwt.matcher_attempts a LEFT JOIN platinumwt.matches m USING(match_id) WHERE a.operation_id=$1::uuid",
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
  database.Parameters("INSERT INTO platinumwt.matches VALUES ($1::uuid,$2::uuid,"
      "least($3::uuid,$4::uuid),greatest($3::uuid,$4::uuid))",
      {HexBytes(match), HexBytes(operation), HexBytes(first.trade), HexBytes(second.trade)});
  for (const auto &owners : {std::pair{&first, &second}, std::pair{&second, &first}}) {
    const auto &owner = *owners.first, &peer = *owners.second;
    const auto receipt = detail::RandomIdentifier();
    const auto sequence = std::to_string(owner.highwater + 1);
    database.Parameters("INSERT INTO platinumwt.receipts VALUES ($1::uuid,$2::uuid,$3::uuid,$4::uuid,1,$5::uuid)",
        {HexBytes(receipt), HexBytes(owner.trade), HexBytes(owner.owner), HexBytes(peer.asset), HexBytes(match)});
    auto changed = database.Parameters("UPDATE platinumwt.offers SET state=2,receipt_id=$2::uuid,last_event=$3::numeric "
        "WHERE trade_id=$1::uuid AND state=1 RETURNING trade_id", {HexBytes(owner.trade), HexBytes(receipt), sequence});
    if (PQntuples(changed.get()) != 1) throw std::runtime_error("queued offer changed");
    changed = database.Parameters("UPDATE platinumwt.ownership SET location=2,save_id=$2::uuid,trade_id=$3::uuid,last_event=$4::numeric "
        "WHERE asset_id=$1::uuid AND save_id=$5::uuid AND trade_id=$6::uuid AND location=1 RETURNING asset_id",
        {HexBytes(peer.asset), HexBytes(owner.owner), HexBytes(owner.trade), sequence, HexBytes(peer.owner), HexBytes(peer.trade)});
    if (PQntuples(changed.get()) != 1) throw std::runtime_error("queued ownership changed");
    database.Parameters("INSERT INTO platinumwt.trade_events(save_id,sequence,kind,trade_id,asset_id,payload_sha256,receipt_id,state) "
        "SELECT $1::uuid,$2::numeric,3,$3::uuid,asset_id,sha256,$5::uuid,2 FROM platinumwt.payloads WHERE asset_id=$4::uuid",
        {HexBytes(owner.owner), sequence, HexBytes(owner.trade), HexBytes(peer.asset), HexBytes(receipt)});
  }
  database.Parameters("INSERT INTO platinumwt.matcher_attempts VALUES ($1::uuid,$2::uuid)", {HexBytes(operation), HexBytes(match)});
}

bool Retry(const detail::DatabaseError &error) {
  return error.SqlState() == "40001" || error.SqlState() == "40P01" || error.SqlState() == "55P03" ||
      (error.SqlState() == "23505" && (error.Constraint() == "matches_pkey" || error.Constraint() == "receipts_pkey"));
}

} // namespace

MatcherService::MatcherService(std::string connection_info, RecoveryBarrier recovery)
    : connection_info_(std::move(connection_info)), recovery_(std::make_shared<detail::RecoveryGate>(std::move(recovery))) {}

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
        const auto first = Select(database);
        const auto second = first ? Select(database, first->owner) : std::nullopt;
        if (first && second) {
          if (!recovery_->Covers(database, "identity" + first->owner, first->highwater) ||
              !recovery_->Covers(database, "identity" + second->owner, second->highwater)) {
            Result(*reply, v1::RESULT_PROCESSING); return grpc::Status::OK;
          }
          Pair(database, *first, *second, request->operation_id());
        } else {
          database.Parameters("INSERT INTO platinumwt.matcher_attempts VALUES ($1::uuid,NULL)", {HexBytes(request->operation_id())});
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

} // namespace platinumwt::server
