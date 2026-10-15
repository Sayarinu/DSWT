#ifndef PLATINUMWT_SERVER_HTTP_SESSION_H
#define PLATINUMWT_SERVER_HTTP_SESSION_H

#include "platinumwt/server/http_request_handler.h"

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <memory>

namespace platinumwt::server {

class HttpSession : public std::enable_shared_from_this<HttpSession> {
public:
  HttpSession(boost::asio::ip::tcp::socket socket,
              std::shared_ptr<grpc::Channel> channel);
  void Run();

private:
  void Respond();

  boost::beast::tcp_stream stream_;
  boost::beast::flat_buffer buffer_{8192};
  boost::beast::http::request_parser<boost::beast::http::string_body> parser_;
  boost::beast::http::response<boost::beast::http::string_body> response_;
  HttpRequestHandler handler_;
};

} // namespace platinumwt::server

#endif
