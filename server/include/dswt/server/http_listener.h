#ifndef DSWT_SERVER_HTTP_LISTENER_H
#define DSWT_SERVER_HTTP_LISTENER_H

#include <boost/asio.hpp>
#include <grpcpp/grpcpp.h>
#include <memory>

namespace dswt::server {

class HttpServices;

class HttpListener : public std::enable_shared_from_this<HttpListener> {
public:
  HttpListener(boost::asio::io_context &io,
               boost::asio::ip::tcp::endpoint endpoint,
               std::shared_ptr<HttpServices> services);
  void Run();

private:
  void Accept();

  boost::asio::ip::tcp::acceptor acceptor_;
  std::shared_ptr<HttpServices> services_;
};

} // namespace dswt::server

#endif
