#include "platinumwt/server/server.h"

#include "platinumwt/server/connectivity_service.h"
#include "platinumwt/server/http_listener.h"
#include "http_services.h"

#include <boost/asio/ssl/context.hpp>
#include <openssl/ssl.h>

#include <csignal>
#include <stdexcept>
#include <utility>

namespace platinumwt::server {
namespace asio = boost::asio;

namespace {
std::shared_ptr<asio::ssl::context> TlsContext(const std::optional<TlsFiles> &files) {
  if (!files) return {};
  try {
    auto context = std::make_shared<asio::ssl::context>(asio::ssl::context::tls_server);
    auto *native = context->native_handle();
    if (SSL_CTX_set_min_proto_version(native, TLS1_2_VERSION) != 1 ||
        SSL_CTX_set_cipher_list(native, "ECDHE-ECDSA-CHACHA20-POLY1305") != 1 ||
        SSL_CTX_set_num_tickets(native, 0) != 1) throw std::runtime_error("TLS profile unavailable");
    SSL_CTX_set_options(native, SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION | SSL_OP_NO_TICKET);
    SSL_CTX_set_session_cache_mode(native, SSL_SESS_CACHE_OFF);
    context->use_certificate_chain_file(files->certificate);
    context->use_private_key_file(files->private_key, asio::ssl::context::pem);
    if (SSL_CTX_check_private_key(native) != 1) throw std::runtime_error("TLS key mismatch");
    return context;
  } catch (const std::exception &) {
    throw std::runtime_error("cannot load TLS configuration");
  }
}

std::shared_ptr<grpc::Channel> Channel(const std::string &target) {
  grpc::ChannelArguments arguments;
  arguments.SetMaxReceiveMessageSize(4096);
  arguments.SetMaxSendMessageSize(4096);
  return grpc::CreateCustomChannel(target, grpc::InsecureChannelCredentials(), arguments);
}
} // namespace

void RunServer(const ServerConfig &config) {
  auto tls = TlsContext(config.tls);
  ConnectivityService service;
  grpc::ServerBuilder builder;
  int grpc_port = 0;
  builder.AddListeningPort(config.grpc_address,
                           grpc::InsecureServerCredentials(), &grpc_port);
  builder.RegisterService(&service);
  builder.SetMaxReceiveMessageSize(4096);
  builder.SetMaxSendMessageSize(4096);
  grpc::ResourceQuota resources;
  resources.SetMaxThreads(8).Resize(4 * 1024 * 1024);
  builder.SetResourceQuota(resources);
  auto server = builder.BuildAndStart();
  if (!server || grpc_port == 0) {
    throw std::runtime_error("Cannot start gRPC connectivity service");
  }

  auto channel = Channel(config.grpc_target.value_or("127.0.0.1:" + std::to_string(grpc_port)));
  auto trade_channel = config.trade_target ? Channel(*config.trade_target) : nullptr;
  asio::thread_pool submissions(4), recovery(4);
  asio::io_context io;
  auto services = std::make_shared<HttpServices>(
      std::make_shared<HttpRequestHandler>(std::move(channel), std::move(trade_channel)), submissions, recovery, std::move(tls));
  asio::signal_set signals(io, SIGINT, SIGTERM);
  signals.async_wait([&](boost::system::error_code, int) { io.stop(); });
  std::make_shared<HttpListener>(
      io,
      asio::ip::tcp::endpoint(asio::ip::make_address(config.http_address),
                              config.http_port),
      std::move(services))
      ->Run();
  io.run();
  submissions.join();
  recovery.join();
  server->Shutdown();
}

} // namespace platinumwt::server
