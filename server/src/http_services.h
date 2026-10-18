#pragma once

#include "platinumwt/server/http_request_handler.h"

#include <boost/asio/ssl/context.hpp>
#include <boost/asio/thread_pool.hpp>

#include <atomic>
#include <functional>

namespace platinumwt::server {

class HttpServices;
struct HttpConnection {
  explicit HttpConnection(std::shared_ptr<HttpServices> owner);
  ~HttpConnection();
  std::shared_ptr<HttpServices> owner;
};

class HttpServices : public std::enable_shared_from_this<HttpServices> {
public:
  using Request = boost::beast::http::request<boost::beast::http::string_body>;
  using Response = boost::beast::http::response<boost::beast::http::string_body>;
  HttpServices(std::shared_ptr<HttpRequestHandler> handler, boost::asio::thread_pool &workers,
                std::shared_ptr<boost::asio::ssl::context> tls);
  std::unique_ptr<HttpConnection> Connect();
  bool Dispatch(Request request, std::shared_ptr<grpc::ClientContext> context, bool secure,
                std::function<void(Response)> complete);
  const std::shared_ptr<boost::asio::ssl::context> &Tls() const { return tls_; }

private:
  friend struct HttpConnection;
  std::shared_ptr<HttpRequestHandler> handler_;
  boost::asio::thread_pool &workers_;
  std::shared_ptr<boost::asio::ssl::context> tls_;
  std::atomic<unsigned> connections_{0}, jobs_{0};
};

} // namespace platinumwt::server
