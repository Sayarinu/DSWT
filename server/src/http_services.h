#pragma once

#include "platinumwt/server/http_request_handler.h"
#include "admission.h"

#include <boost/asio/ssl/context.hpp>
#include <boost/asio/thread_pool.hpp>

#include <functional>

namespace platinumwt::server {

struct HttpConnection {
  std::string network;
  std::unique_ptr<detail::Quota::Permit> permit;
};

class HttpServices : public std::enable_shared_from_this<HttpServices> {
public:
  using Request = boost::beast::http::request<boost::beast::http::string_body>;
  using Response = boost::beast::http::response<boost::beast::http::string_body>;
  HttpServices(std::shared_ptr<HttpRequestHandler> handler, boost::asio::thread_pool &submissions,
                boost::asio::thread_pool &recovery,
                std::shared_ptr<boost::asio::ssl::context> tls, unsigned submission_workers = 4);
  std::unique_ptr<HttpConnection> Connect(const boost::asio::ip::address &peer);
  bool Dispatch(Request request, std::shared_ptr<grpc::ClientContext> context, bool secure, std::string_view network,
                std::function<void(Response)> complete);
  const std::shared_ptr<boost::asio::ssl::context> &Tls() const { return tls_; }

private:
  std::shared_ptr<HttpRequestHandler> handler_;
  boost::asio::thread_pool &submissions_, &recovery_;
  std::shared_ptr<boost::asio::ssl::context> tls_;
  detail::Quota connections_{{1024, 64, 128, 64}};
  detail::Quota submission_jobs_;
  detail::Quota recovery_jobs_{{16, 8, 256, 128}};
};

} // namespace platinumwt::server
