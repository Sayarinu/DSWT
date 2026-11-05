#pragma once

#include <string_view>

namespace dswt::server::detail {

// Shared verification boundary for identity-scoped database operations.
bool CredentialMatches(std::string_view save_id, std::string_view credential,
                       std::string_view stored_hex);

} // namespace dswt::server::detail
