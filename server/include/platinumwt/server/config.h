#ifndef PLATINUMWT_SERVER_CONFIG_H
#define PLATINUMWT_SERVER_CONFIG_H

#include <optional>
#include <string>

namespace platinumwt::server {

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
};

struct BackendConfig {
  std::string address;
  std::string connection_info;
  std::string legality_address;
  std::string legality_policy;
  unsigned short metrics_port = 0;
};

ServerConfig LoadServerConfig();
BackendConfig LoadBackendConfig();
unsigned short LoadMetricsPort();

} // namespace platinumwt::server

#endif
