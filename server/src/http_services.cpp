#include "http_services.h"

#include <boost/asio/post.hpp>

namespace platinumwt::server {

HttpConnection::HttpConnection(std::shared_ptr<HttpServices> service) : owner(std::move(service)) {}
HttpConnection::~HttpConnection() { --owner->connections_; }

HttpServices::HttpServices(std::shared_ptr<HttpRequestHandler> handler, boost::asio::thread_pool &workers,
                           std::shared_ptr<boost::asio::ssl::context> tls)
    : handler_(std::move(handler)), workers_(workers), tls_(std::move(tls)) {}

std::unique_ptr<HttpConnection> HttpServices::Connect() {
  if (connections_.fetch_add(1) >= 1024) { --connections_; return {}; }
  try { return std::make_unique<HttpConnection>(shared_from_this()); }
  catch (...) { --connections_; throw; }
}

bool HttpServices::Dispatch(Request request, std::shared_ptr<grpc::ClientContext> context, bool secure,
                            std::function<void(Response)> complete) {
  if (jobs_.fetch_add(1) >= 64) { --jobs_; return false; }
  try {
    boost::asio::post(workers_, [self = shared_from_this(), request = std::move(request), context = std::move(context), secure,
                               complete = std::move(complete)]() mutable {
      Response response;
      try { response = self->handler_->Handle(request, *context, secure); }
      catch (const std::exception &) {
        response.version(request.version());
        response.result(boost::beast::http::status::service_unavailable);
        response.keep_alive(false);
        response.set(boost::beast::http::field::cache_control, "no-store");
        response.prepare_payload();
      }
      --self->jobs_;
      complete(std::move(response));
    });
  } catch (...) { --jobs_; throw; }
  return true;
}

} // namespace platinumwt::server
