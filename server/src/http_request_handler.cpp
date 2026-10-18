#include "platinumwt/server/http_request_handler.h"

#include "platinumwt/server/connectivity_service.h"

#include <algorithm>
#include <chrono>
#include <charconv>
#include <iostream>
#include <utility>

namespace platinumwt::server {
namespace http = boost::beast::http;

HttpRequestHandler::HttpRequestHandler(std::shared_ptr<grpc::Channel> channel,
                                       std::shared_ptr<grpc::Channel> trade_channel)
    : stub_(v1::Connectivity::NewStub(std::move(channel))) {
  if (trade_channel) trades_ = std::make_unique<TradeGateway>(std::move(trade_channel));
}

http::response<http::string_body>
HttpRequestHandler::Handle(const http::request<http::string_body> &request) {
  grpc::ClientContext context;
  return Handle(request, context, false);
}

http::response<http::string_body>
HttpRequestHandler::Handle(const http::request<http::string_body> &request,
                           grpc::ClientContext &context, bool secure) {
  http::response<http::string_body> response;
  response.version(request.version());
  response.keep_alive(false);
  response.set(http::field::content_type, "text/plain; charset=utf-8");
  response.set(http::field::cache_control, "no-store");

  if (request.target() == "/wt/v1") {
    const auto length = request[http::field::content_length];
    std::size_t size = 0;
    bool valid_length = false;
    if (!length.empty()) {
      const auto parsed = std::from_chars(length.data(), length.data() + length.size(), size);
      valid_length = parsed.ec == std::errc{} && parsed.ptr == length.data() + length.size();
    }
    if (request.method() != http::verb::post) {
      response.result(http::status::method_not_allowed);
      response.set(http::field::allow, "POST");
    } else if (!secure) response.result(http::status::forbidden);
    else if (request.version() != 11 || request.count(http::field::host) != 1 || request[http::field::host].empty() ||
        request.count(http::field::content_length) != 1 || !valid_length ||
        size != request.body().size() || size > 4096 || request.chunked() || request.count(http::field::transfer_encoding) != 0 ||
        request.count(http::field::content_encoding) != 0 || request.count(http::field::expect) != 0 ||
        request.count(http::field::upgrade) != 0 || request.count(http::field::content_type) != 1 ||
        request[http::field::content_type] != "application/octet-stream" || request.count(http::field::connection) != 1 ||
        !boost::beast::iequals(request[http::field::connection], "close")) response.result(http::status::bad_request);
    else if (!trades_) response.result(http::status::service_unavailable);
    else {
      auto reply = trades_->Exchange(request.body(), context);
      response.result(reply.status == GatewayReply::Status::Ok ? http::status::ok :
          reply.status == GatewayReply::Status::Invalid ? http::status::bad_request : http::status::service_unavailable);
      if (reply.status == GatewayReply::Status::Ok) {
        response.set(http::field::content_type, "application/octet-stream");
        response.body() = std::move(reply.packet);
      }
    }
  } else if (request.method() != http::verb::get) {
    response.result(http::status::method_not_allowed);
    response.set(http::field::allow, "GET");
    response.body() = "Method not allowed\n";
  } else if (request.target() != "/v1/ping") {
    response.result(http::status::not_found);
    response.body() = "Not found\n";
  } else {
    context.set_deadline(std::min(context.deadline(), std::chrono::system_clock::now() + std::chrono::seconds(2)));
    google::protobuf::Empty ping;
    v1::PingResponse acknowledgement;
    const auto status = stub_->Ping(&context, ping, &acknowledgement);
    if (status.ok() && acknowledgement.acknowledgement() == kAcknowledgement) {
      response.result(http::status::ok);
      response.body() = acknowledgement.acknowledgement();
    } else {
      response.result(http::status::service_unavailable);
      response.body() = "Connectivity service unavailable\n";
    }
    std::cout << "ping " << response.result_int() << std::endl;
  }

  response.prepare_payload();
  return response;
}

} // namespace platinumwt::server
