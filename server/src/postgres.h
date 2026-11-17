#pragma once

#include <libpq-fe.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace dswt::server::detail {

using Result = std::unique_ptr<PGresult, decltype(&PQclear)>;

class DatabaseError : public std::runtime_error {
public:
  DatabaseError(std::string state, std::string constraint);
  const std::string &SqlState() const { return state_; }
  const std::string &Constraint() const { return constraint_; }

private:
  std::string state_;
  std::string constraint_;
};

// One request's connection. Its PostgreSQL session is reused by a later request
// only after this one ends cleanly outside a transaction; requests change no
// session state beyond the fixed settings applied when it was opened.
class Database {
public:
  explicit Database(const std::string &connection_info);
  ~Database();
  Database(const Database &) = delete;
  Database &operator=(const Database &) = delete;
  Result Execute(const char *sql);
  Result Parameters(const char *sql, const std::vector<std::string> &values);

private:
  friend class Transaction;
  std::string info_;
  std::unique_ptr<PGconn, decltype(&PQfinish)> connection_{nullptr, PQfinish};
};

class Transaction {
public:
  explicit Transaction(Database &database, const char *begin = "BEGIN");
  ~Transaction();
  Transaction(const Transaction &) = delete;
  Transaction &operator=(const Transaction &) = delete;
  void Commit();

private:
  Database &database_;
  bool committed_ = false;
};

} // namespace dswt::server::detail
