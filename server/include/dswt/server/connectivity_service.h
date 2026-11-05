#ifndef DSWT_SERVER_CONNECTIVITY_SERVICE_H
#define DSWT_SERVER_CONNECTIVITY_SERVICE_H

#include "connectivity.grpc.pb.h"

namespace dswt::server {

inline constexpr char kAcknowledgement[] = "DSWT_ACK_V1\n";

class ConnectivityService final : public v1::Connectivity::Service {
public:
  grpc::Status Ping(grpc::ServerContext *context,
                    const google::protobuf::Empty *request,
                    v1::PingResponse *response) override;
};

} // namespace dswt::server

#endif
