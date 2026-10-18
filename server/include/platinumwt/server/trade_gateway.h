#pragma once

#include <grpcpp/grpcpp.h>

#include <memory>
#include <string>
#include <string_view>

namespace platinumwt::server {

struct GatewayReply {
  enum class Status { Ok, Invalid, Unavailable };
  Status status = Status::Unavailable;
  std::string packet;
};

// One bounded v1 frame maps to one private gRPC call. The caller supplies the
// cancellation context; credentials, payloads and request bodies are never logs.
class TradeGateway {
public:
  explicit TradeGateway(std::shared_ptr<grpc::Channel> channel);
  ~TradeGateway();
  GatewayReply Exchange(std::string_view packet, grpc::ClientContext &context);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace platinumwt::server
