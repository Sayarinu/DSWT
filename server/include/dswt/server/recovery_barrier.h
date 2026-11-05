#pragma once

#include <cstdint>
#include <functional>

namespace dswt::server {

// Attest that an off-site, independently recoverable WAL prefix covers this
// PostgreSQL flush position in the current cluster and timeline. Do not block;
// restart services after recovery. An absent provider always fails closed.
using RecoveryBarrier = std::function<bool(std::uint64_t)>;

} // namespace dswt::server
