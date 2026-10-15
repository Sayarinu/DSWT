#ifndef PLATINUMWT_SERVER_CONFIG_H
#define PLATINUMWT_SERVER_CONFIG_H

#include <optional>
#include <string>

namespace platinumwt::server {

struct ServerConfig {
  std::string http_address;
  unsigned short http_port;
  std::string grpc_address;
  std::optional<std::string> grpc_target;
};

ServerConfig LoadServerConfig();

} // namespace platinumwt::server

#endif
