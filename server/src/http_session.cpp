#include "platinumwt/server/http_session.h"

#include "http_services.h"

#include <chrono>
#include <utility>

namespace platinumwt::server {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

HttpSession::HttpSession(tcp::socket socket, std::shared_ptr<HttpServices> services,
                         std::unique_ptr<HttpConnection> connection)
    : services_(std::move(services)), connection_(std::move(connection)),
      stream_(services_->Tls() ? Stream(std::in_place_index<1>, std::move(socket), *services_->Tls()) :
                                Stream(std::in_place_index<0>, std::move(socket))),
      executor_(Lowest().get_executor()), deadline_(executor_) {
  parser_.header_limit(2048);
  parser_.body_limit(4096);
  parser_.eager(true);
}

HttpSession::~HttpSession() = default;

beast::tcp_stream &HttpSession::Lowest() {
  return std::visit([](auto &stream) -> beast::tcp_stream & { return beast::get_lowest_layer(stream); }, stream_);
}

void HttpSession::Run() {
  if (!services_->Tls()) { BeginRead(); return; }
  Lowest().expires_after(std::chrono::seconds(30));
  std::get<1>(stream_).async_handshake(asio::ssl::stream_base::server,
      [self = shared_from_this()](beast::error_code error) {
        if (error) self->Close(); else self->BeginRead();
      });
}

void HttpSession::BeginRead() {
  deadline_.expires_after(std::chrono::seconds(15));
  deadline_.async_wait([self = shared_from_this()](beast::error_code error) { if (!error) self->Close(); });
  Read();
}

void HttpSession::Read() {
  Lowest().expires_after(std::chrono::seconds(5));
  std::visit([self = shared_from_this()](auto &stream) {
    http::async_read_some(stream, self->buffer_, self->parser_, [self](beast::error_code error, std::size_t) {
      if (self->phase_ != Phase::Reading) return;
      if (error) self->Close();
      else if (!self->parser_.is_done()) self->Read();
      else if (self->buffer_.size() != 0) self->Close();
      else self->Dispatch();
    });
  }, stream_);
}

void HttpSession::Dispatch() {
  phase_ = Phase::Waiting;
  version_ = parser_.get().version();
  context_->set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(4));
  Lowest().expires_after(std::chrono::seconds(5));
  // Watch EOF or extra pipelined bytes while gRPC runs on a bounded worker.
  std::visit([self = shared_from_this()](auto &stream) {
    stream.async_read_some(asio::buffer(self->probe_), [self](beast::error_code error, std::size_t) {
      if (self->phase_ == Phase::Closed) return;
      if (self->phase_ == Phase::ReadyToWrite && error == asio::error::operation_aborted) self->Write();
      else self->Close();
    });
  }, stream_);
  const bool started = services_->Dispatch(parser_.release(), context_, services_->Tls() != nullptr, connection_->network,
      [self = shared_from_this()](HttpServices::Response response) {
        asio::post(self->executor_, [self, response = std::move(response)]() mutable { self->Ready(std::move(response)); });
      });
  if (!started) {
    HttpServices::Response response{http::status::service_unavailable, version_};
    response.keep_alive(false);
    response.set(http::field::cache_control, "no-store");
    response.prepare_payload();
    Ready(std::move(response));
  }
}

void HttpSession::Ready(http::response<http::string_body> response) {
  if (phase_ != Phase::Waiting) return;
  response_ = std::move(response);
  phase_ = Phase::ReadyToWrite;
  // Finish the read cancellation before starting a write or TLS shutdown.
  beast::error_code error;
  Lowest().socket().cancel(error);
  if (error) Close();
}

void HttpSession::Write() {
  phase_ = Phase::Writing;
  Lowest().expires_after(std::chrono::seconds(5));
  std::visit([self = shared_from_this()](auto &stream) {
    http::async_write(stream, self->response_, [self](beast::error_code error, std::size_t) {
      if (error) self->Close(); else self->Shutdown();
    });
  }, stream_);
}

void HttpSession::Shutdown() {
  if (phase_ == Phase::Closed) return;
  if (!services_->Tls()) { Close(); return; }
  phase_ = Phase::Closing;
  Lowest().expires_after(std::chrono::seconds(2));
  std::get<1>(stream_).async_shutdown([self = shared_from_this()](beast::error_code) { self->Close(); });
}

void HttpSession::Close() {
  if (phase_ == Phase::Closed) return;
  phase_ = Phase::Closed;
  deadline_.cancel();
  context_->TryCancel();
  beast::error_code ignored;
  Lowest().socket().shutdown(tcp::socket::shutdown_both, ignored);
  Lowest().socket().close(ignored);
}

} // namespace platinumwt::server
