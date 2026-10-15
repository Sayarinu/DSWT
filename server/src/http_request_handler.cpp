#include "platinumwt/server/http_request_handler.h"

#include "platinumwt/server/connectivity_service.h"

#include <chrono>
#include <iostream>
#include <utility>

namespace platinumwt::server {
namespace http = boost::beast::http;

HttpRequestHandler::HttpRequestHandler(std::shared_ptr<grpc::Channel> channel)
    : stub_(v1::Connectivity::NewStub(std::move(channel))) {}

http::response<http::string_body>
HttpRequestHandler::Handle(const http::request<http::string_body> &request) {
  http::response<http::string_body> response;
  response.version(request.version());
  response.keep_alive(false);
  response.set(http::field::content_type, "text/plain; charset=utf-8");
  response.set(http::field::cache_control, "no-store");

  if (request.method() != http::verb::get) {
    response.result(http::status::method_not_allowed);
    response.set(http::field::allow, "GET");
    response.body() = "Method not allowed\n";
  } else if (request.target() != "/v1/ping") {
    response.result(http::status::not_found);
    response.body() = "Not found\n";
  } else {
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() +
                         std::chrono::seconds(2));
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
