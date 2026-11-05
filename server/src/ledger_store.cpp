#include "dswt/server/ledger_store.h"

#include "postgres.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <regex>
#include <stdexcept>
#include <vector>

namespace dswt::server {
namespace {

using detail::Transaction;

struct Migration {
  std::string version;
  std::string body;
};

std::vector<Migration> ReadMigrations(const std::filesystem::path &directory) {
  std::vector<std::filesystem::path> paths;
  const std::regex name("[0-9]{4}_[a-z0-9_]+\\.sql");
  for (const auto &entry : std::filesystem::directory_iterator(directory)) {
    if (entry.path().extension() != ".sql")
      continue;
    if (entry.is_symlink() || !entry.is_regular_file() ||
        !std::regex_match(entry.path().filename().string(), name) ||
        entry.file_size() > 1024 * 1024) {
      throw std::runtime_error("invalid migration input");
    }
    paths.push_back(entry.path());
  }
  std::sort(paths.begin(), paths.end());
  std::vector<Migration> migrations;
  for (const auto &path : paths) {
    const auto version = path.filename().string().substr(0, 4);
    if (std::stoul(version) != migrations.size() + 1) {
      throw std::runtime_error(
          "migrations must start at one and be contiguous");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input)
      throw std::runtime_error("cannot read migration");
    std::string body{std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>()};
    if (input.bad() || body.empty() || body.find('\0') != std::string::npos) {
      throw std::runtime_error("invalid migration contents");
    }
    migrations.push_back({version, std::move(body)});
  }
  if (migrations.empty())
    throw std::runtime_error("no migrations found");
  return migrations;
}

} // namespace

struct LedgerStore::Impl {
  detail::Database database;
  explicit Impl(const std::string &info) : database(info) {}
};

LedgerStore::LedgerStore(const std::string &connection_info)
    : impl_(std::make_unique<Impl>(connection_info)) {}
LedgerStore::~LedgerStore() = default;

void LedgerStore::Migrate(const std::filesystem::path &directory) {
  const auto migrations = ReadMigrations(directory);
  Transaction transaction(impl_->database);
  impl_->database.Execute("SELECT pg_advisory_xact_lock(578892145137)");
  impl_->database.Execute(
      "CREATE SCHEMA IF NOT EXISTS dswt_meta; "
      "REVOKE ALL ON SCHEMA dswt_meta FROM PUBLIC; "
      "CREATE TABLE IF NOT EXISTS dswt_meta.schema_migrations "
      "(version integer PRIMARY KEY CHECK (version > 0), sha256 "
      "bytea NOT NULL CHECK (octet_length(sha256) = 32)); "
      "REVOKE ALL ON dswt_meta.schema_migrations FROM PUBLIC");
  auto installed = impl_->database.Execute(
      "SELECT version FROM dswt_meta.schema_migrations ORDER BY version");
  const auto count = static_cast<std::size_t>(PQntuples(installed.get()));
  if (count > migrations.size())
    throw std::runtime_error(
        "database migration version is newer than this build");
  for (std::size_t index = 0; index < migrations.size(); ++index) {
    const auto &migration = migrations[index];
    if (index < count) {
      auto matches = impl_->database.Parameters(
          "SELECT sha256 = pg_catalog.sha256(convert_to($2, 'UTF8')) "
          "FROM dswt_meta.schema_migrations WHERE version = $1::integer",
          {migration.version, migration.body});
      if (PQntuples(matches.get()) != 1 ||
          std::string(PQgetvalue(matches.get(), 0, 0)) != "t") {
        throw std::runtime_error(
            "installed migration has changed or is missing");
      }
    } else {
      impl_->database.Execute(migration.body.c_str());
      impl_->database.Parameters(
          "INSERT INTO dswt_meta.schema_migrations VALUES "
          "($1::integer, pg_catalog.sha256(convert_to($2, 'UTF8')))",
          {migration.version, migration.body});
    }
  }
  transaction.Commit();
}

std::uint64_t LedgerStore::VerifyReplay() {
  Transaction transaction(impl_->database,
                          "BEGIN ISOLATION LEVEL REPEATABLE READ");
  impl_->database.Execute("SET LOCAL statement_timeout = '10min'");
  auto result = impl_->database.Execute("SELECT dswt.verify_replay()");
  const auto count = std::stoull(PQgetvalue(result.get(), 0, 0));
  transaction.Commit();
  return count;
}

} // namespace dswt::server
