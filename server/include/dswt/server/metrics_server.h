#pragma once

#include "dswt/server/telemetry.h"

#include <memory>
#include <string>

namespace dswt::server {

// Port zero disables export. Enabled listeners bind only IPv4 loopback.
// A single bounded read-only ledger sampler runs independently of RPC workers.
class MetricsServer {
public:
  MetricsServer(unsigned short port, std::string database = {});
  ~MetricsServer();
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace dswt::server
