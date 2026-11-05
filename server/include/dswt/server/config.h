#ifndef DSWT_SERVER_CONFIG_H
#define DSWT_SERVER_CONFIG_H

#include <optional>
#include <string>

namespace dswt::server {

// Concurrent Submit handlers when WT_SUBMISSION_WORKERS is unset.
inline constexpr unsigned kDefaultSubmissionWorkers = 4;
// New saves one network may enroll per day when WT_ENROLLMENTS_PER_NETWORK is unset.
inline constexpr unsigned kDefaultEnrollmentsPerNetwork = 16;
// New saves overall per hour when WT_ENROLLMENTS_PER_HOUR is unset. Each can force a
// 1 MB WAL file out, so this also bounds how fast fake enrollments can use disk.
inline constexpr unsigned kDefaultEnrollmentsPerHour = 200;

struct TlsFiles {
  std::string certificate;
  std::string private_key;
};

struct ServerConfig {
  std::string http_address;
  unsigned short http_port;
  std::string grpc_address;
  std::optional<std::string> grpc_target;
  std::optional<std::string> trade_target;
  std::optional<TlsFiles> tls;
  unsigned short metrics_port = 0;
  unsigned submission_workers = kDefaultSubmissionWorkers;
  unsigned enrollments_per_hour = kDefaultEnrollmentsPerHour;
  unsigned enrollments_per_network = kDefaultEnrollmentsPerNetwork;
};

struct BackendConfig {
  std::string address;
  std::string connection_info;
  std::string legality_address;
  std::string legality_policy;
  unsigned short metrics_port = 0;
  unsigned submission_workers = kDefaultSubmissionWorkers;
};

struct MatcherConfig {
  std::string connection_info;
  unsigned short metrics_port = 0;
  unsigned workers = 1;
};

ServerConfig LoadServerConfig();
BackendConfig LoadBackendConfig();
MatcherConfig LoadMatcherConfig();
unsigned short LoadMetricsPort();
// Concurrent Submit handlers per process (WT_SUBMISSION_WORKERS, 1-16). Size it to the legality
// validator's isolated workers (WT_VALIDATOR_WORKERS, 1-8); the gateway queues at most twice this many.
unsigned LoadSubmissionWorkers();

} // namespace dswt::server

#endif
