#include "postgres.h"

#include <utility>

namespace platinumwt::server::detail {
namespace {

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

Database::Database(const std::string &info) {
  const char *keys[] = {"dbname", "connect_timeout", "application_name",
                        nullptr};
  const char *values[] = {info.c_str(), "3", "platinumwt-ledger", nullptr};
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

} // namespace platinumwt::server::detail
