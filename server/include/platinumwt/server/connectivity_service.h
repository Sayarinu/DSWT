#ifndef PLATINUMWT_SERVER_CONNECTIVITY_SERVICE_H
#define PLATINUMWT_SERVER_CONNECTIVITY_SERVICE_H

#include "connectivity.grpc.pb.h"

namespace platinumwt::server {

inline constexpr char kAcknowledgement[] = "PLATINUMWT_ACK_V1\n";

class ConnectivityService final : public v1::Connectivity::Service {
public:
  grpc::Status Ping(grpc::ServerContext *context,
                    const google::protobuf::Empty *request,
                    v1::PingResponse *response) override;
};

} // namespace platinumwt::server

#endif
