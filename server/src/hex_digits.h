#pragma once

namespace dswt::server {
inline constexpr unsigned kNibbleBits = 4;
inline constexpr unsigned kHexRadix = 1 << kNibbleBits;
inline constexpr unsigned kNibbleMask = kHexRadix - 1;
inline constexpr unsigned kHexLetterBase = 10;
} // namespace dswt::server
