#ifndef PLATINUMWT_SERVER_HTTP_LISTENER_H
#define PLATINUMWT_SERVER_HTTP_LISTENER_H

#include <boost/asio.hpp>
#include <grpcpp/grpcpp.h>
#include <memory>

namespace platinumwt::server {

class HttpListener : public std::enable_shared_from_this<HttpListener> {
public:
  HttpListener(boost::asio::io_context &io,
               boost::asio::ip::tcp::endpoint endpoint,
               std::shared_ptr<grpc::Channel> channel);
  void Run();

private:
  void Accept();

  boost::asio::ip::tcp::acceptor acceptor_;
  std::shared_ptr<grpc::Channel> channel_;
};

} // namespace platinumwt::server

#endif
