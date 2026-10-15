#include "platinumwt/server/config.h"

#include <cstdlib>
#include <stdexcept>

namespace platinumwt::server {
namespace {

std::string Environment(const char *name, const char *fallback) {
  const char *value = std::getenv(name);
  return value ? value : fallback;
}

unsigned short HttpPort() {
  const auto value = Environment("WT_HTTP_PORT", "8080");
  std::size_t parsed = 0;
  const auto port = std::stoul(value, &parsed);
  if (parsed != value.size() || port > 65535) {
    throw std::invalid_argument("WT_HTTP_PORT must be between 0 and 65535");
  }
  return static_cast<unsigned short>(port);
}

} // namespace

ServerConfig LoadServerConfig() {
  ServerConfig config{Environment("WT_HTTP_ADDRESS", "0.0.0.0"), HttpPort(),
                      Environment("WT_GRPC_ADDRESS", "127.0.0.1:50051"),
                      std::nullopt};
  if (const char *target = std::getenv("WT_GRPC_TARGET")) {
    config.grpc_target = target;
  }
  return config;
}

} // namespace platinumwt::server
