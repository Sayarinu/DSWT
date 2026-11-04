#pragma once

#include <libpq-fe.h>

#include <atomic>
#include <memory>
#include <string>

namespace platinumwt::server::detail {

using QueryResult = std::unique_ptr<PGresult, decltype(&PQclear)>;

// One read-only statement on its own connection, with client-side deadlines,
// for background observers. Returns null on any failure, including `stopped`
// being set, so neither a stalled database nor shutdown can wait on it.
QueryResult BoundedQuery(const std::string &database, const char *sql, const char *application_name,
                         const std::atomic<bool> &stopped);

} // namespace platinumwt::server::detail
