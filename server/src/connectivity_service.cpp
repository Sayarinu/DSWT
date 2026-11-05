#include "dswt/server/connectivity_service.h"

namespace dswt::server {

grpc::Status ConnectivityService::Ping(grpc::ServerContext *,
                                       const google::protobuf::Empty *,
                                       v1::PingResponse *response) {
  response->set_acknowledgement(kAcknowledgement);
  return grpc::Status::OK;
}

} // namespace dswt::server
