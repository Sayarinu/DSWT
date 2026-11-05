#ifndef DSWT_SERVER_HTTP_SESSION_H
#define DSWT_SERVER_HTTP_SESSION_H

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <grpcpp/grpcpp.h>
#include <memory>
#include <variant>

namespace dswt::server {

class HttpServices;
struct HttpConnection;

class HttpSession : public std::enable_shared_from_this<HttpSession> {
public:
  HttpSession(boost::asio::ip::tcp::socket socket,
              std::shared_ptr<HttpServices> services, std::unique_ptr<HttpConnection> connection);
  ~HttpSession();
  void Run();

private:
  using Stream = std::variant<boost::beast::tcp_stream, boost::beast::ssl_stream<boost::beast::tcp_stream>>;
  enum class Phase { Reading, Waiting, ReadyToWrite, Writing, Closing, Closed };
  boost::beast::tcp_stream &Lowest();
  void BeginRead();
  void Read();
  void Dispatch();
  void Ready(boost::beast::http::response<boost::beast::http::string_body> response);
  void Write();
  void Shutdown();
  void Close();

  std::shared_ptr<HttpServices> services_;
  std::unique_ptr<HttpConnection> connection_;
  Stream stream_;
  boost::asio::any_io_executor executor_;
  boost::asio::steady_timer deadline_;
  boost::beast::flat_buffer buffer_{6144};
  boost::beast::http::request_parser<boost::beast::http::string_body> parser_;
  boost::beast::http::response<boost::beast::http::string_body> response_;
  std::shared_ptr<grpc::ClientContext> context_ = std::make_shared<grpc::ClientContext>();
  Phase phase_ = Phase::Reading;
  char probe_[1]{};
  unsigned version_ = 11;
};

} // namespace dswt::server

#endif
