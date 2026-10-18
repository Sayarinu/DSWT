#pragma once

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace platinumwt::server {

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
  // A conflicting credential returns no identity information.
  std::optional<IdentityId> Enroll(const IdentityId &enrollment_id,
                                   const SaveCredential &credential);
  bool Authenticate(const IdentityId &save_id,
                    const SaveCredential &credential);
  bool OwnsTrade(const IdentityId &save_id, const SaveCredential &credential,
                 const IdentityId &trade_id);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace platinumwt::server
