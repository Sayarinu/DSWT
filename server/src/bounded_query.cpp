#include "bounded_query.h"

#include <boost/asio/ip/address.hpp>
#include <boost/system/error_code.hpp>

#include <poll.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <string_view>

namespace dswt::server::detail {
namespace {

using Clock = std::chrono::steady_clock;

// libpq's asynchronous connector can still block in DNS/service discovery.
// The observer fails unavailable for those configurations; RPC configuration
// remains unchanged. Explicit hostaddr preserves a TLS verification hostname.
bool NonblockingAddress(const std::string &database) {
  std::string host, address;
  bool service = false;
  if (std::getenv("PGSERVICE")) return false;
  if (const auto value = std::getenv("PGHOST")) host = value;
  if (const auto value = std::getenv("PGHOSTADDR")) address = value;
  const auto read = [&](const PQconninfoOption *options) {
    for (auto option = options; option && option->keyword; ++option) {
      if (!option->val) continue;
      const std::string_view key(option->keyword);
      if (key == "host") host = option->val;
      else if (key == "hostaddr") address = option->val;
      else if (key == "service") service = true;
    }
  };
  std::unique_ptr<PQconninfoOption, decltype(&PQconninfoFree)> explicit_options(PQconninfoParse(database.c_str(), nullptr), PQconninfoFree);
  if (!explicit_options) return false;
  read(explicit_options.get());
  if (service) return false;
  std::string_view remaining(address.empty() ? host : address);
  do {
    const auto comma = remaining.find(',');
    const auto value = remaining.substr(0, comma);
    if (!(address.empty() && (value.empty() || value.front() == '/' || value.front() == '@'))) {
      boost::system::error_code error;
      boost::asio::ip::make_address(std::string(value), error);
      if (error) return false;
    }
    if (comma == std::string_view::npos) break;
    remaining.remove_prefix(comma + 1);
  } while (true);
  return true;
}

bool WaitSocket(PGconn *connection, short events, Clock::time_point deadline, const std::atomic<bool> &stopped) {
  while (!stopped.load() && Clock::now() < deadline) {
    pollfd descriptor{PQsocket(connection), events, 0};
    if (descriptor.fd < 0) return false;
    const auto ready = poll(&descriptor, 1, 25);
    if (ready > 0) return !(descriptor.revents & (POLLERR | POLLNVAL));
    if (ready < 0 && errno != EINTR) return false;
  }
  return false;
}

} // namespace

QueryResult BoundedQuery(const std::string &database, const char *sql, const char *application_name,
                         const std::atomic<bool> &stopped) {
  QueryResult none(nullptr, PQclear);
  if (!NonblockingAddress(database)) return none;
  const char *keys[] = {"dbname", "connect_timeout", "options", "application_name", nullptr};
  const char *values[] = {database.c_str(), "1", "-c statement_timeout=250 -c lock_timeout=100 -c default_transaction_read_only=on", application_name, nullptr};
  std::unique_ptr<PGconn, decltype(&PQfinish)> connection(PQconnectStartParams(keys, values, 1), PQfinish);
  if (!connection || PQstatus(connection.get()) == CONNECTION_BAD) return none;
  const auto connect_deadline = Clock::now() + std::chrono::seconds(1);
  auto state = PGRES_POLLING_WRITING;
  while (state != PGRES_POLLING_OK) {
    if (state == PGRES_POLLING_FAILED || !WaitSocket(connection.get(),
        state == PGRES_POLLING_WRITING ? POLLOUT : POLLIN, connect_deadline, stopped)) return none;
    state = PQconnectPoll(connection.get());
  }
  if (PQsetnonblocking(connection.get(), 1) != 0) return none;
  PQsetNoticeProcessor(connection.get(), [](void *, const char *) {}, nullptr);
  const auto query_deadline = Clock::now() + std::chrono::milliseconds(500);
  if (!PQsendQuery(connection.get(), sql)) return none;
  while (true) {
    const auto flushed = PQflush(connection.get());
    if (flushed == 0) break;
    if (flushed < 0 || !WaitSocket(connection.get(), POLLOUT, query_deadline, stopped)) return none;
  }
  while (PQisBusy(connection.get())) {
    if (!WaitSocket(connection.get(), POLLIN, query_deadline, stopped) || !PQconsumeInput(connection.get())) return none;
  }
  QueryResult row(PQgetResult(connection.get()), PQclear);
  if (!row || PQresultStatus(row.get()) != PGRES_TUPLES_OK) return none;
  return row;
}

} // namespace dswt::server::detail
