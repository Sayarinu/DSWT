#include "http_services.h"
#include "dswt/server/telemetry.h"

#include <boost/asio/post.hpp>

#include <stdexcept>

namespace dswt::server {

HttpServices::HttpServices(std::shared_ptr<HttpRequestHandler> handler, boost::asio::thread_pool &submissions,
                           boost::asio::thread_pool &recovery,
                           std::shared_ptr<boost::asio::ssl::context> tls, unsigned submission_workers)
    : handler_(std::move(handler)), submissions_(submissions), recovery_(recovery), tls_(std::move(tls)),
      submission_jobs_({2 * submission_workers, 8, 64, 32}) {}

std::unique_ptr<HttpConnection> HttpServices::Connect(const boost::asio::ip::address &peer) {
  auto network = detail::NetworkKey(peer);
  auto permit = connections_.Acquire(network);
  if (!permit) { Telemetry::Global().Refused(); return {}; }
  return std::make_unique<HttpConnection>(HttpConnection{std::move(network), std::move(permit)});
}

bool HttpServices::Dispatch(Request request, std::shared_ptr<grpc::ClientContext> context, bool secure,
                            std::string_view network, std::function<void(Response)> complete) {
  const auto &body = request.body();
  // Classification reserves workers, never grants authentication or ownership.
  const bool recovering = secure && request.target() == "/wt/v1" && body.size() >= 80 &&
      body.compare(0, 5, "WTDS\1", 5) == 0 && body[5] >= 3 && body[5] <= 7;
  auto permit = (recovering ? recovery_jobs_ : submission_jobs_).Acquire(network);
  if (!permit) { Telemetry::Global().Refused(); return false; }
  boost::asio::post(recovering ? recovery_ : submissions_,
      [self = shared_from_this(), request = std::move(request), context = std::move(context), secure,
       permit = std::move(permit), complete = std::move(complete)]() mutable {
      Response response;
      try {
        if (std::chrono::system_clock::now() >= context->deadline()) throw std::runtime_error("request expired in queue");
        response = self->handler_->Handle(request, *context, secure);
      }
      catch (const std::exception &) {
        response.version(request.version());
        response.result(boost::beast::http::status::service_unavailable);
        response.keep_alive(false);
        response.set(boost::beast::http::field::cache_control, "no-store");
        response.prepare_payload();
      }
      permit.reset();
      complete(std::move(response));
    });
  return true;
}

} // namespace dswt::server
