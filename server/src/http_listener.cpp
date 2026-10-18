#include "platinumwt/server/http_listener.h"

#include "platinumwt/server/http_session.h"

#include <iostream>
#include <utility>

namespace platinumwt::server {
namespace asio = boost::asio;
using tcp = asio::ip::tcp;

HttpListener::HttpListener(asio::io_context &io, tcp::endpoint endpoint,
                           std::shared_ptr<grpc::Channel> channel)
    : acceptor_(io), channel_(std::move(channel)) {
  acceptor_.open(endpoint.protocol());
  acceptor_.set_option(asio::socket_base::reuse_address(true));
  acceptor_.bind(endpoint);
  acceptor_.listen();
}

void HttpListener::Run() {
  std::cout << "HTTP listening on " << acceptor_.local_endpoint() << std::endl;
  Accept();
}

void HttpListener::Accept() {
  acceptor_.async_accept([self = shared_from_this()](
                             boost::system::error_code error,
                             tcp::socket socket) {
    if (!error) {
      std::make_shared<HttpSession>(std::move(socket), self->channel_)->Run();
    }
    if (self->acceptor_.is_open()) {
      self->Accept();
    }
  });
}

} // namespace platinumwt::server
