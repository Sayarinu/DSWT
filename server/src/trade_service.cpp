#include "platinumwt/server/trade_service.h"

#include "identity_auth.h"
#include "postgres.h"
#include "trade_encoding.h"

#include <algorithm>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace platinumwt::server {

// Model state advances only with a new identity event. Remember one conservative
// WAL requirement per identity so retries do not chase unrelated/new row-lock
// WAL forever while the off-site prefix advances. Eviction only delays a reply.
struct VisibilityCache {
  std::mutex mutex;
  std::unordered_map<std::string, std::pair<std::uint64_t, std::uint64_t>> entries;

  std::uint64_t Remember(const std::string &id, std::uint64_t event, std::uint64_t position) {
    std::lock_guard<std::mutex> lock(mutex);
    auto existing = entries.find(id);
    if (existing != entries.end() && existing->second.first >= event) return existing->second.second;
    if (existing == entries.end() && entries.size() >= 4096) entries.erase(entries.begin());
    entries[id] = {event, position};
    return position;
  }
};

namespace {
using detail::Database;
using detail::HexBytes;
using detail::UnhexBytes;
using detail::Unsigned;
constexpr auto kMaximum = std::numeric_limits<std::uint64_t>::max();

template <typename Reply, typename Work>
grpc::Status Guard(grpc::ServerContext *context, Reply &reply, Work work) {
  reply.Clear();
  if (context && context->IsCancelled())
    return {grpc::StatusCode::CANCELLED, "request cancelled"};
  try {
    work();
    return grpc::Status::OK;
  } catch (const std::exception &) {
    reply.Clear();
    return {grpc::StatusCode::UNAVAILABLE, "trade service unavailable"};
  }
}

bool AuthShape(const v1::Credentials &auth) {
  return detail::NonzeroBytes(auth.save_id(), 16) && detail::NonzeroBytes(auth.credential(), 32);
}

template <typename Reply>
void Result(Reply &reply, v1::ResultCode code) {
  reply.Clear();
  reply.mutable_context()->set_result(code);
}

// A connection belongs to one request/transaction. The service itself shares
// no libpq connection across concurrent gRPC calls.
class Session {
public:
  Database database;
  detail::Transaction transaction;
  const v1::Credentials &auth;
  const RecoveryBarrier &recovery;
  VisibilityCache &visibility;
  bool authenticated = false;
  std::uint64_t next = 0, highwater = 0;

  Session(const std::string &info, const v1::Credentials &credentials,
          const RecoveryBarrier &barrier, VisibilityCache &cache, bool write)
      : database(info), transaction(database, write ? "BEGIN" : "BEGIN ISOLATION LEVEL REPEATABLE READ READ ONLY"),
        auth(credentials), recovery(barrier), visibility(cache) {
    auto row = database.Parameters(write ?
        "SELECT encode(credential_verifier,'hex'),next_counter::text,event_highwater::text "
        "FROM platinumwt.save_identities WHERE save_id=$1::uuid FOR UPDATE" :
        "SELECT encode(credential_verifier,'hex'),next_counter::text,event_highwater::text "
        "FROM platinumwt.save_identities WHERE save_id=$1::uuid", {HexBytes(auth.save_id())});
    const bool found = PQntuples(row.get()) == 1;
    const bool matches = detail::CredentialMatches(auth.save_id(), auth.credential(),
        found ? PQgetvalue(row.get(), 0, 0) : "");
    authenticated = found && matches;
    if (authenticated) {
      next = Unsigned(PQgetvalue(row.get(), 0, 1));
      highwater = Unsigned(PQgetvalue(row.get(), 0, 2));
    }
  }

  bool Covered(std::uint64_t event) {
    if (!recovery) return false;
    auto row = database.Execute("SELECT (pg_current_wal_flush_lsn() - '0/0'::pg_lsn)::text");
    return recovery(visibility.Remember(auth.save_id(), event, Unsigned(PQgetvalue(row.get(), 0, 0))));
  }

  template <typename Reply>
  void Publish(Reply &reply, v1::ResultCode code) {
    // After a write commit these reads also cover any newer committed progress.
    // For reads they remain in the same repeatable-read snapshot.
    auto row = database.Parameters("SELECT next_counter::text,event_highwater::text "
        "FROM platinumwt.save_identities WHERE save_id=$1::uuid", {HexBytes(auth.save_id())});
    if (PQntuples(row.get()) != 1) throw std::runtime_error("identity disappeared");
    if (!Covered(Unsigned(PQgetvalue(row.get(), 0, 1)))) {
      Result(reply, v1::RESULT_PROCESSING);
      return;
    }
    if (code != v1::RESULT_OK) reply.Clear();
    auto *context = reply.mutable_context();
    context->set_result(code);
    context->set_next_counter(Unsigned(PQgetvalue(row.get(), 0, 0)));
    context->set_event_highwater(Unsigned(PQgetvalue(row.get(), 0, 1)));
  }

  bool Begin(const v1::MutationContext &context, const std::string &hash,
             v1::MutationReply &reply) {
    if (!authenticated) {
      Result(reply, v1::RESULT_UNAUTHENTICATED);
      return false;
    }
    auto prior = database.Parameters("SELECT encode(request_sha256,'hex'),result,"
        "encode(outcome,'hex') FROM platinumwt.operations WHERE save_id=$1::uuid "
        "AND operation_id=$2::uuid", {HexBytes(auth.save_id()), HexBytes(context.operation_id())});
    if (PQntuples(prior.get()) == 1) {
      auto code = static_cast<v1::ResultCode>(std::stoi(PQgetvalue(prior.get(), 0, 1)));
      if (PQgetvalue(prior.get(), 0, 0) != HexBytes(hash)) code = v1::RESULT_CONFLICT;
      else if (code == v1::RESULT_OK)
        detail::DecodeOutcome(UnhexBytes(PQgetvalue(prior.get(), 0, 2)), reply);
      Publish(reply, code);
      return false;
    }
    if (context.counter() == 0 || context.counter() == kMaximum || context.counter() != next) {
      Publish(reply, v1::RESULT_COUNTER_MISMATCH);
      return false;
    }
    if (highwater == kMaximum) {
      Publish(reply, v1::RESULT_CAPACITY);
      return false;
    }
    if (!Covered(highwater)) {
      Result(reply, v1::RESULT_PROCESSING);
      return false;
    }
    return true;
  }

  v1::TradeSnapshot Snapshot(std::string_view trade = {}) {
    auto row = database.Parameters(
        "SELECT replace(o.trade_id::text,'-',''),o.state,replace(p.asset_id::text,'-',''),"
        "encode(p.sha256,'hex'),coalesce(replace(r.asset_id::text,'-',''),''),"
        "coalesce(encode(q.sha256,'hex'),''),coalesce(replace(r.receipt_id::text,'-',''),''),"
        "o.last_event::text,coalesce(r.delivery_kind,0) FROM platinumwt.offers o "
        "JOIN platinumwt.payloads p ON p.asset_id=o.offered_asset_id "
        "LEFT JOIN platinumwt.receipts r ON r.receipt_id=o.receipt_id "
        "LEFT JOIN platinumwt.payloads q ON q.asset_id=r.asset_id "
        "WHERE o.save_id=$1::uuid AND (($2='' AND o.state<>4) OR o.trade_id=nullif($2,'')::uuid)",
        {HexBytes(auth.save_id()), HexBytes(trade)});
    v1::TradeSnapshot snapshot;
    if (PQntuples(row.get()) == 0) return snapshot;
    if (PQntuples(row.get()) != 1) throw std::runtime_error("ambiguous owned trade");
    snapshot.set_trade_id(UnhexBytes(PQgetvalue(row.get(), 0, 0)));
    snapshot.set_state(static_cast<v1::TradeState>(std::stoi(PQgetvalue(row.get(), 0, 1))));
    snapshot.set_offered_asset_id(UnhexBytes(PQgetvalue(row.get(), 0, 2)));
    snapshot.set_offered_sha256(UnhexBytes(PQgetvalue(row.get(), 0, 3)));
    snapshot.set_received_asset_id(UnhexBytes(PQgetvalue(row.get(), 0, 4)));
    snapshot.set_received_sha256(UnhexBytes(PQgetvalue(row.get(), 0, 5)));
    snapshot.set_receipt_id(UnhexBytes(PQgetvalue(row.get(), 0, 6)));
    snapshot.set_last_event(Unsigned(PQgetvalue(row.get(), 0, 7)));
    snapshot.set_delivery_kind(static_cast<v1::DeliveryKind>(std::stoi(PQgetvalue(row.get(), 0, 8))));
    return snapshot;
  }

  void Record(const v1::MutationContext &context, unsigned char opcode,
              const std::string &hash, v1::ResultCode code, v1::EventKind kind,
              const v1::TradeSnapshot &snapshot = {}, std::string_view rejected_digest = {}) {
    const auto sequence = std::to_string(highwater + 1);
    const bool success = code == v1::RESULT_OK;
    const auto asset = kind == v1::EVENT_SUBMITTED ? snapshot.offered_asset_id() : snapshot.received_asset_id();
    const auto digest = kind == v1::EVENT_SUBMITTED ? snapshot.offered_sha256() : snapshot.received_sha256();
    database.Parameters("INSERT INTO platinumwt.operations VALUES ($1::uuid,$2::uuid,$3::numeric,"
        "$4::smallint,decode($5,'hex'),$6::smallint,$7::numeric,decode($8,'hex'))",
        {HexBytes(auth.save_id()), HexBytes(context.operation_id()), std::to_string(context.counter()),
         std::to_string(opcode), HexBytes(hash), std::to_string(code), sequence,
         success ? HexBytes(detail::EncodeOutcome(highwater + 1, snapshot)) : ""});
    database.Parameters("INSERT INTO platinumwt.trade_events(save_id,sequence,kind,trade_id,operation_id,"
        "asset_id,payload_sha256,receipt_id,state) VALUES ($1::uuid,$2::numeric,$3::smallint,"
        "nullif($4,'')::uuid,$5::uuid,nullif($6,'')::uuid,CASE WHEN $7='' THEN NULL ELSE decode($7,'hex') END,"
        "nullif($8,'')::uuid,$9::smallint)",
        {HexBytes(auth.save_id()), sequence, std::to_string(kind), HexBytes(snapshot.trade_id()),
         HexBytes(context.operation_id()), success ? HexBytes(asset) : "",
         HexBytes(success ? std::string_view(digest) : rejected_digest),
         success && kind != v1::EVENT_SUBMITTED ? HexBytes(snapshot.receipt_id()) : "",
         std::to_string(snapshot.state())});
    transaction.Commit();
  }
};

bool MutationShape(const v1::MutationContext &context) {
  return AuthShape(context.auth()) && detail::NonzeroBytes(context.operation_id(), 16);
}

template <typename Reply>
bool CheckAuthShape(const v1::Credentials &auth, Reply &reply) {
  if (AuthShape(auth)) return true;
  Result(reply, v1::RESULT_UNAUTHENTICATED);
  return false;
}

void Rejected(Session &session, const v1::MutationContext &context, unsigned char opcode,
              const std::string &hash, v1::ResultCode code, v1::MutationReply &reply,
              const v1::TradeSnapshot &snapshot = {}, std::string_view digest = {}) {
  session.Record(context, opcode, hash, code, v1::EVENT_REJECTED, snapshot, digest);
  session.Publish(reply, code);
}

bool RetryAllocation(const detail::DatabaseError &error) {
  return error.SqlState() == "23505" && (error.Constraint() == "payloads_pkey" ||
      error.Constraint() == "offers_pkey" || error.Constraint() == "receipts_pkey");
}

template <typename Work>
void Allocate(Work work) {
  for (int attempt = 0; attempt < 8; ++attempt) {
    try { work(); return; }
    catch (const detail::DatabaseError &error) { if (!RetryAllocation(error)) throw; }
  }
  throw std::runtime_error("trade allocation unavailable");
}

} // namespace

TradeService::TradeService(std::string connection_info, SubmissionValidator validator, RecoveryBarrier recovery)
    : connection_info_(std::move(connection_info)), validator_(std::move(validator)), recovery_(std::move(recovery)),
      visibility_(std::make_shared<VisibilityCache>()) {}

grpc::Status TradeService::Submit(grpc::ServerContext *context, const v1::SubmitRequest *request,
                                  v1::MutationReply *reply) {
  return Guard(context, *reply, [&] {
    if (!CheckAuthShape(request->context().auth(), *reply)) return;
    if (!MutationShape(request->context())) { Result(*reply, v1::RESULT_INVALID_REQUEST); return; }
    if (request->escrow_generation() == 0 || !detail::CanonicalShape(request->pokemon().pk4()) ||
        request->pokemon().sha256().size() != 32 ||
        detail::HashBytes(request->pokemon().pk4()) != request->pokemon().sha256()) {
      Result(*reply, v1::RESULT_INVALID_REQUEST); return;
    }
    std::string body;
    detail::AppendUnsigned(body, request->escrow_generation(), 8);
    body += request->pokemon().pk4();
    body += request->pokemon().sha256();
    const auto hash = detail::MutationHash(2, request->context(), body);
    {
      Session session(connection_info_, request->context().auth(), recovery_, *visibility_, true);
      if (!session.Begin(request->context(), hash, *reply)) return;
      if (session.Snapshot().state() != v1::TRADE_NONE) { session.Publish(*reply, v1::RESULT_CAPACITY); return; }
      session.transaction.Commit();
    }
    // Never hold an identity/database lock while waiting on the validator.
    std::array<unsigned char, 136> data{};
    std::copy(request->pokemon().pk4().begin(), request->pokemon().pk4().end(), data.begin());
    const auto legality = validator_ ? validator_(data) : LegalityDecision{};
    Allocate([&] {
      Session session(connection_info_, request->context().auth(), recovery_, *visibility_, true);
      if (!session.Begin(request->context(), hash, *reply)) return;
      if (session.Snapshot().state() != v1::TRADE_NONE) { session.Publish(*reply, v1::RESULT_CAPACITY); return; }
      if (legality.verdict == LegalityVerdict::Unavailable) { session.Publish(*reply, v1::RESULT_UNAVAILABLE); return; }
      if (legality.verdict == LegalityVerdict::Illegal) {
        Rejected(session, request->context(), 2, hash, v1::RESULT_ILLEGAL_POKEMON, *reply, {}, request->pokemon().sha256());
        return;
      }
      const auto asset = detail::RandomIdentifier(), trade = detail::RandomIdentifier();
      const auto sequence = std::to_string(session.highwater + 1);
      session.database.Parameters("INSERT INTO platinumwt.payloads VALUES ($1::uuid,decode($2,'hex'),decode($3,'hex'))",
          {HexBytes(asset), HexBytes(request->pokemon().pk4()), HexBytes(request->pokemon().sha256())});
      session.database.Parameters("INSERT INTO platinumwt.offers(trade_id,save_id,offered_asset_id,state,last_event) "
          "VALUES ($1::uuid,$2::uuid,$3::uuid,1,$4::numeric)",
          {HexBytes(trade), HexBytes(session.auth.save_id()), HexBytes(asset), sequence});
      session.database.Parameters("INSERT INTO platinumwt.ownership VALUES ($1::uuid,$2::uuid,$3::uuid,1,$4::numeric)",
          {HexBytes(asset), HexBytes(session.auth.save_id()), HexBytes(trade), sequence});
      const auto snapshot = session.Snapshot(trade);
      session.Record(request->context(), 2, hash, v1::RESULT_OK, v1::EVENT_SUBMITTED, snapshot);
      reply->set_operation_event(session.highwater + 1);
      *reply->mutable_outcome() = snapshot;
      session.Publish(*reply, v1::RESULT_OK);
    });
  });
}

grpc::Status TradeService::Status(grpc::ServerContext *context, const v1::StatusRequest *request,
                                  v1::StatusReply *reply) {
  return Guard(context, *reply, [&] {
    if (!CheckAuthShape(request->auth(), *reply)) return;
    if (!request->trade_id().empty() && !detail::NonzeroBytes(request->trade_id(), 16)) {
      Result(*reply, v1::RESULT_INVALID_REQUEST); return;
    }
    Session session(connection_info_, request->auth(), recovery_, *visibility_, false);
    if (!session.authenticated) { Result(*reply, v1::RESULT_UNAUTHENTICATED); return; }
    *reply->mutable_trade() = session.Snapshot(request->trade_id());
    session.Publish(*reply, !request->trade_id().empty() && reply->trade().state() == v1::TRADE_NONE ?
        v1::RESULT_NOT_FOUND : v1::RESULT_OK);
    session.transaction.Commit();
  });
}

grpc::Status TradeService::Cancel(grpc::ServerContext *context, const v1::CancelRequest *request,
                                  v1::MutationReply *reply) {
  return Guard(context, *reply, [&] {
    if (!CheckAuthShape(request->context().auth(), *reply)) return;
    if (!MutationShape(request->context()) || !detail::NonzeroBytes(request->trade_id(), 16)) {
      Result(*reply, v1::RESULT_INVALID_REQUEST); return;
    }
    const auto hash = detail::MutationHash(4, request->context(), request->trade_id());
    Allocate([&] {
      Session session(connection_info_, request->context().auth(), recovery_, *visibility_, true);
      if (!session.Begin(request->context(), hash, *reply)) return;
      auto snapshot = session.Snapshot(request->trade_id());
      if (snapshot.state() != v1::TRADE_QUEUED) {
        Rejected(session, request->context(), 4, hash, snapshot.state() == v1::TRADE_NONE ?
            v1::RESULT_NOT_FOUND : v1::RESULT_CONFLICT, *reply, snapshot); return;
      }
      const auto receipt = detail::RandomIdentifier();
      const auto sequence = std::to_string(session.highwater + 1);
      session.database.Parameters("INSERT INTO platinumwt.receipts(receipt_id,trade_id,save_id,asset_id,delivery_kind) "
          "VALUES ($1::uuid,$2::uuid,$3::uuid,$4::uuid,2)",
          {HexBytes(receipt), HexBytes(request->trade_id()), HexBytes(session.auth.save_id()), HexBytes(snapshot.offered_asset_id())});
      session.database.Parameters("UPDATE platinumwt.offers SET state=3,receipt_id=$3::uuid,last_event=$4::numeric "
          "WHERE trade_id=$1::uuid AND save_id=$2::uuid",
          {HexBytes(request->trade_id()), HexBytes(session.auth.save_id()), HexBytes(receipt), sequence});
      session.database.Parameters("UPDATE platinumwt.ownership SET location=2,last_event=$4::numeric "
          "WHERE asset_id=$1::uuid AND save_id=$2::uuid AND trade_id=$3::uuid",
          {HexBytes(snapshot.offered_asset_id()), HexBytes(session.auth.save_id()), HexBytes(request->trade_id()), sequence});
      snapshot = session.Snapshot(request->trade_id());
      session.Record(request->context(), 4, hash, v1::RESULT_OK, v1::EVENT_CANCELLED, snapshot);
      reply->set_operation_event(session.highwater + 1);
      *reply->mutable_outcome() = snapshot;
      session.Publish(*reply, v1::RESULT_OK);
    });
  });
}

grpc::Status TradeService::Collect(grpc::ServerContext *context, const v1::CollectRequest *request,
                                   v1::CollectReply *reply) {
  return Guard(context, *reply, [&] {
    if (!CheckAuthShape(request->auth(), *reply)) return;
    if (!detail::NonzeroBytes(request->trade_id(), 16)) { Result(*reply, v1::RESULT_INVALID_REQUEST); return; }
    Session session(connection_info_, request->auth(), recovery_, *visibility_, false);
    if (!session.authenticated) { Result(*reply, v1::RESULT_UNAUTHENTICATED); return; }
    const auto snapshot = session.Snapshot(request->trade_id());
    if (snapshot.state() == v1::TRADE_NONE || snapshot.state() == v1::TRADE_QUEUED) {
      session.Publish(*reply, snapshot.state() == v1::TRADE_NONE ? v1::RESULT_NOT_FOUND : v1::RESULT_CONFLICT); return;
    }
    auto row = session.database.Parameters("SELECT encode(pk4,'hex'),encode(sha256,'hex') "
        "FROM platinumwt.payloads WHERE asset_id=$1::uuid", {HexBytes(snapshot.received_asset_id())});
    if (PQntuples(row.get()) != 1) throw std::runtime_error("delivery payload missing");
    *reply->mutable_trade() = snapshot;
    reply->mutable_pokemon()->set_pk4(UnhexBytes(PQgetvalue(row.get(), 0, 0)));
    reply->mutable_pokemon()->set_sha256(UnhexBytes(PQgetvalue(row.get(), 0, 1)));
    session.Publish(*reply, v1::RESULT_OK);
    session.transaction.Commit();
  });
}

grpc::Status TradeService::Acknowledge(grpc::ServerContext *context, const v1::AcknowledgeRequest *request,
                                       v1::MutationReply *reply) {
  return Guard(context, *reply, [&] {
    if (!CheckAuthShape(request->context().auth(), *reply)) return;
    if (!MutationShape(request->context()) || !detail::NonzeroBytes(request->trade_id(), 16) ||
        !detail::NonzeroBytes(request->receipt_id(), 16) || request->delivered_sha256().size() != 32 ||
        request->applied_sha256().size() != 32 || request->applied_generation() == 0) {
      Result(*reply, v1::RESULT_INVALID_REQUEST); return;
    }
    std::string body = request->trade_id() + request->receipt_id() + request->delivered_sha256() + request->applied_sha256();
    detail::AppendUnsigned(body, request->applied_generation(), 8);
    const auto hash = detail::MutationHash(6, request->context(), body);
    Session session(connection_info_, request->context().auth(), recovery_, *visibility_, true);
    if (!session.Begin(request->context(), hash, *reply)) return;
    auto snapshot = session.Snapshot(request->trade_id());
    if ((snapshot.state() != v1::TRADE_MATCHED && snapshot.state() != v1::TRADE_RETURN_READY) ||
        snapshot.receipt_id() != request->receipt_id() || snapshot.received_sha256() != request->delivered_sha256()) {
      Rejected(session, request->context(), 6, hash, snapshot.state() == v1::TRADE_NONE ?
          v1::RESULT_NOT_FOUND : v1::RESULT_CONFLICT, *reply, snapshot); return;
    }
    const auto sequence = std::to_string(session.highwater + 1);
    session.database.Parameters("INSERT INTO platinumwt.acknowledgements VALUES "
        "($1::uuid,$2::uuid,$3::uuid,decode($4,'hex'),$5::numeric)",
        {HexBytes(request->receipt_id()), HexBytes(session.auth.save_id()), HexBytes(request->context().operation_id()),
         HexBytes(request->applied_sha256()), std::to_string(request->applied_generation())});
    session.database.Parameters("UPDATE platinumwt.offers SET state=4,last_event=$3::numeric "
        "WHERE trade_id=$1::uuid AND save_id=$2::uuid",
        {HexBytes(request->trade_id()), HexBytes(session.auth.save_id()), sequence});
    session.database.Parameters("UPDATE platinumwt.ownership SET location=3,last_event=$4::numeric "
        "WHERE asset_id=$1::uuid AND save_id=$2::uuid AND trade_id=$3::uuid",
        {HexBytes(snapshot.received_asset_id()), HexBytes(session.auth.save_id()), HexBytes(request->trade_id()), sequence});
    snapshot = session.Snapshot(request->trade_id());
    session.Record(request->context(), 6, hash, v1::RESULT_OK, v1::EVENT_ACKNOWLEDGED, snapshot);
    reply->set_operation_event(session.highwater + 1);
    *reply->mutable_outcome() = snapshot;
    session.Publish(*reply, v1::RESULT_OK);
  });
}

grpc::Status TradeService::Reconcile(grpc::ServerContext *context, const v1::ReconcileRequest *request,
                                     v1::ReconcileReply *reply) {
  return Guard(context, *reply, [&] {
    if (!CheckAuthShape(request->auth(), *reply)) return;
    if (request->limit() == 0 || request->limit() > 8) { Result(*reply, v1::RESULT_INVALID_REQUEST); return; }
    Session session(connection_info_, request->auth(), recovery_, *visibility_, false);
    if (!session.authenticated) { Result(*reply, v1::RESULT_UNAUTHENTICATED); return; }
    if (request->after_event() > session.highwater) { session.Publish(*reply, v1::RESULT_CURSOR_AHEAD); return; }
    auto rows = session.database.Parameters("SELECT sequence::text,kind,coalesce(replace(trade_id::text,'-',''),''),"
        "coalesce(replace(operation_id::text,'-',''),''),coalesce(replace(asset_id::text,'-',''),''),"
        "coalesce(encode(payload_sha256,'hex'),''),coalesce(replace(receipt_id::text,'-',''),''),state "
        "FROM platinumwt.trade_events AS events WHERE save_id=$1::uuid AND sequence>$2::numeric "
        "ORDER BY events.sequence LIMIT $3::integer",
        {HexBytes(session.auth.save_id()), std::to_string(request->after_event()), std::to_string(request->limit() + 1)});
    const auto expected = std::min<std::uint64_t>(request->limit() + 1, session.highwater - request->after_event());
    if (PQntuples(rows.get()) != static_cast<int>(expected)) throw std::runtime_error("event history incomplete");
    for (int row = 0; row < PQntuples(rows.get()); ++row) {
      if (Unsigned(PQgetvalue(rows.get(), row, 0)) != request->after_event() + row + 1)
        throw std::runtime_error("event history gap");
    }
    reply->set_page_end(request->after_event());
    reply->set_has_more(PQntuples(rows.get()) > static_cast<int>(request->limit()));
    for (int row = 0; row < std::min(PQntuples(rows.get()), static_cast<int>(request->limit())); ++row) {
      auto *event = reply->add_events();
      event->set_sequence(Unsigned(PQgetvalue(rows.get(), row, 0)));
      event->set_kind(static_cast<v1::EventKind>(std::stoi(PQgetvalue(rows.get(), row, 1))));
      event->set_trade_id(UnhexBytes(PQgetvalue(rows.get(), row, 2)));
      event->set_operation_id(UnhexBytes(PQgetvalue(rows.get(), row, 3)));
      event->set_asset_id(UnhexBytes(PQgetvalue(rows.get(), row, 4)));
      event->set_payload_sha256(UnhexBytes(PQgetvalue(rows.get(), row, 5)));
      event->set_receipt_id(UnhexBytes(PQgetvalue(rows.get(), row, 6)));
      event->set_state(static_cast<v1::TradeState>(std::stoi(PQgetvalue(rows.get(), row, 7))));
      reply->set_page_end(event->sequence());
    }
    session.Publish(*reply, v1::RESULT_OK);
    session.transaction.Commit();
  });
}

} // namespace platinumwt::server
