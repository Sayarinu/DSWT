#include "platinumwt/server/http_session.h"

#include <chrono>
#include <utility>

namespace platinumwt::server {
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = boost::asio::ip::tcp;

HttpSession::HttpSession(tcp::socket socket,
                         std::shared_ptr<grpc::Channel> channel)
    : stream_(std::move(socket)), handler_(std::move(channel)) {
  parser_.header_limit(4096);
  parser_.body_limit(0);
}

void HttpSession::Run() {
  stream_.expires_after(std::chrono::seconds(5));
  http::async_read(
      stream_, buffer_, parser_,
      [self = shared_from_this()](beast::error_code error, std::size_t) {
        if (!error) {
          self->Respond();
        }
      });
}

void HttpSession::Respond() {
  response_ = handler_.Handle(parser_.get());
  stream_.expires_after(std::chrono::seconds(5));
  http::async_write(
      stream_, response_,
      [self = shared_from_this()](beast::error_code, std::size_t) {
        beast::error_code ignored;
        self->stream_.socket().shutdown(tcp::socket::shutdown_both, ignored);
      });
}

} // namespace platinumwt::server
