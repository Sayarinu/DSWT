#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace dswt::server {

using IdentityId = std::array<unsigned char, 16>;
using SaveCredential = std::array<unsigned char, 32>;

// Local database results, not externally recoverable protocol outcomes.
// Use one instance per worker; a libpq connection cannot be shared
// concurrently.
class IdentityStore {
public:
  using IdGenerator = std::function<IdentityId()>;
  explicit IdentityStore(const std::string &connection_info,
                         IdGenerator generator = {});
  ~IdentityStore();
  IdentityStore(const IdentityStore &) = delete;
  IdentityStore &operator=(const IdentityStore &) = delete;

  // The client must persist the enrollment ID and CSPRNG credential first.
  // The variant is the enrollment build code, stored once and never changed.
  // An invalid variant, or a conflicting credential or variant, returns no
  // identity information.
  std::optional<IdentityId> Enroll(const IdentityId &enrollment_id,
                                   const SaveCredential &credential,
                                   std::uint32_t variant);
  bool Authenticate(const IdentityId &save_id,
                    const SaveCredential &credential);
  bool OwnsTrade(const IdentityId &save_id, const SaveCredential &credential,
                 const IdentityId &trade_id);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace dswt::server
