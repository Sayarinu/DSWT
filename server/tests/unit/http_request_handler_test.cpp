#include "platinumwt/server/http_request_handler.h"

#include <gtest/gtest.h>

namespace platinumwt::server {
namespace {
namespace http = boost::beast::http;

class HttpRequestHandlerTest : public testing::Test {
protected:
  HttpRequestHandler handler_{
      grpc::CreateChannel("127.0.0.1:0", grpc::InsecureChannelCredentials())};
};

TEST_F(HttpRequestHandlerTest, RejectsUnknownRoute) {
  const auto response = handler_.Handle({http::verb::get, "/unknown", 11});
  EXPECT_EQ(response.result(), http::status::not_found);
  EXPECT_EQ(response.body(), "Not found\n");
}

TEST_F(HttpRequestHandlerTest, RejectsUnsupportedMethodsBeforeRouting) {
  for (const auto method :
       {http::verb::post, http::verb::head, http::verb::put}) {
    for (const char *target : {"/v1/ping", "/unknown"}) {
      SCOPED_TRACE(target);
      const auto response = handler_.Handle({method, target, 11});
      EXPECT_EQ(response.result(), http::status::method_not_allowed);
      EXPECT_EQ(response[http::field::allow], "GET");
      EXPECT_EQ(response.body(), "Method not allowed\n");
    }
  }
}

TEST_F(HttpRequestHandlerTest, DoesNotAcceptPingRouteSuffixes) {
  for (const char *target : {"/v1/ping/", "/v1/ping?extra=1"}) {
    SCOPED_TRACE(target);
    EXPECT_EQ(handler_.Handle({http::verb::get, target, 11}).result(),
              http::status::not_found);
  }
}

TEST_F(HttpRequestHandlerTest, SetsResponseHeadersAndClosesConnection) {
  http::request<http::string_body> request{http::verb::get, "/unknown", 11};
  request.keep_alive(true);
  const auto response = handler_.Handle(request);
  EXPECT_EQ(response.version(), 11);
  EXPECT_EQ(response[http::field::content_type], "text/plain; charset=utf-8");
  EXPECT_EQ(response[http::field::cache_control], "no-store");
  EXPECT_EQ(response[http::field::content_length],
            std::to_string(response.body().size()));
  EXPECT_FALSE(response.keep_alive());
  EXPECT_FALSE(response.chunked());
}

TEST_F(HttpRequestHandlerTest, PreservesHttp10Framing) {
  const auto response = handler_.Handle({http::verb::get, "/unknown", 10});
  EXPECT_EQ(response.version(), 10);
  EXPECT_EQ(response[http::field::content_length], "10");
  EXPECT_FALSE(response.keep_alive());
  EXPECT_FALSE(response.chunked());
}

} // namespace
} // namespace platinumwt::server
