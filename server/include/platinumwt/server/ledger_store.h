#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace platinumwt::server {

// Operator-only migration/replay interface. No trading endpoint is enabled
// here.
class LedgerStore {
public:
  explicit LedgerStore(const std::string &connection_info);
  ~LedgerStore();
  LedgerStore(const LedgerStore &) = delete;
  LedgerStore &operator=(const LedgerStore &) = delete;

  void Migrate(const std::filesystem::path &directory);
  std::uint64_t VerifyReplay();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace platinumwt::server
