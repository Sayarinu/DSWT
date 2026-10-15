#ifndef PLATINUMWT_SERVER_HTTP_REQUEST_HANDLER_H
#define PLATINUMWT_SERVER_HTTP_REQUEST_HANDLER_H

#include "connectivity.grpc.pb.h"

#include <boost/beast/http.hpp>
#include <grpcpp/grpcpp.h>
#include <memory>

namespace platinumwt::server {

class HttpRequestHandler {
public:
  explicit HttpRequestHandler(std::shared_ptr<grpc::Channel> channel);

  boost::beast::http::response<boost::beast::http::string_body>
  Handle(const boost::beast::http::request<boost::beast::http::string_body>
             &request);

private:
  std::unique_ptr<v1::Connectivity::Stub> stub_;
};

} // namespace platinumwt::server

#endif
