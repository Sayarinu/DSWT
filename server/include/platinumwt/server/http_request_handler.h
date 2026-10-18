#ifndef PLATINUMWT_SERVER_HTTP_REQUEST_HANDLER_H
#define PLATINUMWT_SERVER_HTTP_REQUEST_HANDLER_H

#include "connectivity.grpc.pb.h"
#include "platinumwt/server/trade_gateway.h"

#include <boost/beast/http.hpp>
#include <grpcpp/grpcpp.h>
#include <memory>

namespace platinumwt::server {

class HttpRequestHandler {
public:
  explicit HttpRequestHandler(std::shared_ptr<grpc::Channel> channel,
                               std::shared_ptr<grpc::Channel> trade_channel = {});

  boost::beast::http::response<boost::beast::http::string_body>
  Handle(const boost::beast::http::request<boost::beast::http::string_body>
             &request);

  boost::beast::http::response<boost::beast::http::string_body>
  Handle(const boost::beast::http::request<boost::beast::http::string_body> &request,
         grpc::ClientContext &context, bool secure);

private:
  std::unique_ptr<v1::Connectivity::Stub> stub_;
  std::unique_ptr<TradeGateway> trades_;
};

} // namespace platinumwt::server

#endif
