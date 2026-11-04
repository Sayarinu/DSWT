#include "platinumwt/server/config.h"

#include <cstdlib>
#include <charconv>
#include <stdexcept>
#include <string_view>

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

void PrivateAddress(const std::string &address, bool listener) {
  const auto separator = address.rfind(':');
  if (separator == std::string::npos ||
      (address.substr(0, separator) != "127.0.0.1" && address.substr(0, separator) != "[::1]"))
    throw std::invalid_argument("internal gRPC address must use loopback");
  const std::string_view port(address.data() + separator + 1, address.size() - separator - 1);
  unsigned value = 0;
  const auto parsed = std::from_chars(port.data(), port.data() + port.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != port.data() + port.size() || value > 65535 || (!listener && value == 0))
    throw std::invalid_argument("invalid internal gRPC port");
}

std::string Required(const char *name) {
  const auto value = Environment(name, "");
  if (value.empty()) throw std::invalid_argument(std::string(name) + " is required");
  return value;
}

} // namespace

unsigned short LoadMetricsPort() {
  const auto port = Environment("WT_METRICS_PORT", "0");
  unsigned value = 0;
  const auto parsed = std::from_chars(port.data(), port.data() + port.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != port.data() + port.size() || value > 65535)
    throw std::invalid_argument("WT_METRICS_PORT must be between 0 and 65535");
  return static_cast<unsigned short>(value);
}

unsigned LoadSubmissionWorkers() {
  const auto workers = Environment("WT_SUBMISSION_WORKERS", "");
  if (workers.empty() && !std::getenv("WT_SUBMISSION_WORKERS")) return kDefaultSubmissionWorkers;
  unsigned value = 0;
  const auto parsed = std::from_chars(workers.data(), workers.data() + workers.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != workers.data() + workers.size() || value < 1 || value > 16)
    throw std::invalid_argument("WT_SUBMISSION_WORKERS must be between 1 and 16");
  return value;
}

ServerConfig LoadServerConfig() {
  ServerConfig config{Environment("WT_HTTP_ADDRESS", "0.0.0.0"), HttpPort(),
                      Environment("WT_GRPC_ADDRESS", "127.0.0.1:50051"),
                      std::nullopt, std::nullopt, std::nullopt};
  PrivateAddress(config.grpc_address, true);
  if (const char *target = std::getenv("WT_GRPC_TARGET")) {
    config.grpc_target = target;
  }
  if (const char *target = std::getenv("WT_TRADE_TARGET")) {
    PrivateAddress(target, false);
    config.trade_target = target;
  }
  const char *certificate = std::getenv("WT_TLS_CERTIFICATE");
  const char *key = std::getenv("WT_TLS_PRIVATE_KEY");
  if (certificate || key) {
    if (!certificate || !key || !*certificate || !*key) throw std::invalid_argument("both TLS credential files are required");
    config.tls = TlsFiles{certificate, key};
  }
  config.metrics_port = LoadMetricsPort();
  config.submission_workers = LoadSubmissionWorkers();
  return config;
}

BackendConfig LoadBackendConfig() {
  BackendConfig config{Environment("WT_BACKEND_ADDRESS", "127.0.0.1:50053"), Required("PLATINUMWT_DATABASE_URL"),
      Environment("WT_LEGALITY_TARGET", "127.0.0.1:50052"), Required("WT_LEGALITY_POLICY")};
  PrivateAddress(config.address, true);
  PrivateAddress(config.legality_address, false);
  config.metrics_port = LoadMetricsPort();
  config.submission_workers = LoadSubmissionWorkers();
  return config;
}

MatcherConfig LoadMatcherConfig() {
  MatcherConfig config{Required("PLATINUMWT_DATABASE_URL")};
  config.metrics_port = LoadMetricsPort();
  const auto workers = Environment("WT_MATCHER_WORKERS", "1");
  const auto parsed = std::from_chars(workers.data(), workers.data() + workers.size(), config.workers);
  if (parsed.ec != std::errc{} || parsed.ptr != workers.data() + workers.size() || config.workers < 1 || config.workers > 8)
    throw std::invalid_argument("WT_MATCHER_WORKERS must be between 1 and 8");
  return config;
}

} // namespace platinumwt::server
