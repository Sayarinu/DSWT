#pragma once

#include <libpq-fe.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace platinumwt::server::detail {

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

class Database {
public:
  explicit Database(const std::string &connection_info);
  Result Execute(const char *sql);
  Result Parameters(const char *sql, const std::vector<std::string> &values);

private:
  friend class Transaction;
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

} // namespace platinumwt::server::detail
