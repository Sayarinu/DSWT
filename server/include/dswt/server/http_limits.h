#pragma once

#include "dswt/server/wire_layout.h"

#include <chrono>
#include <cstdint>

namespace dswt::server {
inline constexpr unsigned kHttp11 = 11;
inline constexpr std::uint32_t kHeaderLimitBytes = 2048;
inline constexpr auto kBackendTimeout = std::chrono::seconds(4);
} // namespace dswt::server
