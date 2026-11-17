#include "postgres.h"

#include <poll.h>

#include <mutex>
#include <unordered_map>
#include <utility>

namespace dswt::server::detail {
namespace {

// Opening a session forks a PostgreSQL backend, which dominated database CPU on
// the 2-core VPS. Idle sessions are kept per connection string instead.
class IdleSessions {
public:
  PGconn *Take(const std::string &info) {
    for (;;) {
      PGconn *connection = nullptr;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        auto &idle = idle_[info];
        if (idle.empty()) return nullptr;
        connection = idle.back();
        idle.pop_back();
      }
      // An idle session has nothing to say. Anything waiting on its socket, such as
      // the error a terminating or restarting server sends before closing, retires it.
      pollfd socket{PQsocket(connection), POLLIN, 0};
      if (socket.fd >= 0 && poll(&socket, 1, 0) == 0 && PQstatus(connection) == CONNECTION_OK &&
          PQtransactionStatus(connection) == PQTRANS_IDLE)
        return connection;
      PQfinish(connection);
    }
  }

  void Give(const std::string &info, PGconn *connection) {
    if (PQstatus(connection) == CONNECTION_OK && PQtransactionStatus(connection) == PQTRANS_IDLE) {
      std::lock_guard<std::mutex> lock(mutex_);
      auto &idle = idle_[info];
      if (idle.size() < kMaxIdle) { idle.push_back(connection); return; }
    }
    PQfinish(connection);
  }

private:
  static constexpr std::size_t kMaxIdle = 16;
  std::mutex mutex_;
  std::unordered_map<std::string, std::vector<PGconn *>> idle_;
};

// Never destroyed, so no request can outlive it at process exit.
IdleSessions &Sessions() {
  static auto *sessions = new IdleSessions;
  return *sessions;
}

Result CheckResult(PGresult *raw) {
  Result result(raw, PQclear);
  if (!raw || (PQresultStatus(raw) != PGRES_COMMAND_OK &&
               PQresultStatus(raw) != PGRES_TUPLES_OK)) {
    const char *state =
        raw ? PQresultErrorField(raw, PG_DIAG_SQLSTATE) : nullptr;
    const char *constraint =
        raw ? PQresultErrorField(raw, PG_DIAG_CONSTRAINT_NAME) : nullptr;
    throw DatabaseError(state ? state : "unknown",
                        constraint ? constraint : "");
  }
  return result;
}

} // namespace

DatabaseError::DatabaseError(std::string state, std::string constraint)
    : std::runtime_error("database command failed (SQLSTATE " + state + ")"),
      state_(std::move(state)), constraint_(std::move(constraint)) {}

Database::Database(const std::string &info) : info_(info) {
  if (auto *idle = Sessions().Take(info_)) {
    connection_.reset(idle);
    return;
  }
  const char *keys[] = {"dbname", "connect_timeout", "application_name",
                        nullptr};
  const char *values[] = {info.c_str(), "3", "dswt-ledger", nullptr};
  connection_.reset(PQconnectdbParams(keys, values, 1));
  if (!connection_ || PQstatus(connection_.get()) != CONNECTION_OK)
    throw std::runtime_error("database connection failed");
  // Diagnostics can contain private parameters; expose only SQLSTATE to
  // callers.
  PQsetNoticeProcessor(connection_.get(), [](void *, const char *) {}, nullptr);
  if (PQserverVersion(connection_.get()) < 160000)
    throw std::runtime_error("PostgreSQL 16 or later is required");
  Execute(
      "SET search_path = pg_catalog; SET synchronous_commit = on; "
      "SET statement_timeout = '10s'; SET lock_timeout = '2s'; "
      "SET idle_in_transaction_session_timeout = '15s'; SET timezone = 'UTC'; "
      "SET bytea_output = 'hex'; SET client_min_messages = error");
  auto settings = Execute(
      "SELECT current_setting('fsync'), current_setting('full_page_writes'), "
      "current_setting('synchronous_commit'), pg_is_in_recovery()");
  if (std::string(PQgetvalue(settings.get(), 0, 0)) != "on" ||
      std::string(PQgetvalue(settings.get(), 0, 1)) != "on" ||
      std::string(PQgetvalue(settings.get(), 0, 2)) != "on" ||
      std::string(PQgetvalue(settings.get(), 0, 3)) != "f")
    throw std::runtime_error("writable durable database settings are required");
}

Database::~Database() {
  if (connection_) Sessions().Give(info_, connection_.release());
}

Result Database::Execute(const char *sql) {
  return CheckResult(PQexec(connection_.get(), sql));
}

Result Database::Parameters(const char *sql,
                            const std::vector<std::string> &values) {
  std::vector<const char *> parameters;
  for (const auto &value : values)
    parameters.push_back(value.c_str());
  return CheckResult(PQexecParams(connection_.get(), sql,
                                  static_cast<int>(parameters.size()), nullptr,
                                  parameters.data(), nullptr, nullptr, 0));
}

Transaction::Transaction(Database &database, const char *begin)
    : database_(database) {
  database_.Execute(begin);
}

Transaction::~Transaction() {
  if (!committed_)
    PQclear(PQexec(database_.connection_.get(), "ROLLBACK"));
}

void Transaction::Commit() {
  if (PQtransactionStatus(database_.connection_.get()) != PQTRANS_INTRANS)
    throw std::runtime_error("database transaction is not committable");
  database_.Execute("COMMIT");
  committed_ = true;
}

} // namespace dswt::server::detail
