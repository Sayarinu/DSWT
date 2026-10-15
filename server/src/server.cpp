#include "platinumwt/server/server.h"

#include "platinumwt/server/connectivity_service.h"
#include "platinumwt/server/http_listener.h"

#include <csignal>
#include <stdexcept>
#include <utility>

namespace platinumwt::server {
namespace asio = boost::asio;

void RunServer(const ServerConfig &config) {
  ConnectivityService service;
  grpc::ServerBuilder builder;
  int grpc_port = 0;
  builder.AddListeningPort(config.grpc_address,
                           grpc::InsecureServerCredentials(), &grpc_port);
  builder.RegisterService(&service);
  auto server = builder.BuildAndStart();
  if (!server || grpc_port == 0) {
    throw std::runtime_error("Cannot start gRPC connectivity service");
  }

  auto channel = grpc::CreateChannel(
      config.grpc_target.value_or("127.0.0.1:" + std::to_string(grpc_port)),
      grpc::InsecureChannelCredentials());
  asio::io_context io;
  asio::signal_set signals(io, SIGINT, SIGTERM);
  signals.async_wait([&](boost::system::error_code, int) { io.stop(); });
  std::make_shared<HttpListener>(
      io,
      asio::ip::tcp::endpoint(asio::ip::make_address(config.http_address),
                              config.http_port),
      std::move(channel))
      ->Run();
  io.run();
  server->Shutdown();
}

} // namespace platinumwt::server
