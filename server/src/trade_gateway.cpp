#include "platinumwt/server/trade_gateway.h"

#include "trade_encoding.h"
#include "trade.grpc.pb.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <stdexcept>

namespace platinumwt::server {
namespace {

class InvalidFrame : public std::runtime_error {
public:
  InvalidFrame() : std::runtime_error("invalid frame") {}
};

void Require(bool condition) {
  if (!condition) throw std::runtime_error("invalid backend reply");
}

std::uint64_t Number(std::string_view bytes, std::size_t offset, std::size_t size) {
  if (size > 8 || offset > bytes.size() || size > bytes.size() - offset) throw InvalidFrame();
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < size; ++i)
    value |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[offset + i])) << (8 * i);
  return value;
}

struct Frame {
  unsigned char operation;
  std::string request_id;
  v1::Credentials auth;
  std::uint64_t counter;
  std::string_view body;

  v1::MutationContext Mutation() const {
    v1::MutationContext context;
    *context.mutable_auth() = auth;
    context.set_operation_id(request_id);
    context.set_counter(counter);
    return context;
  }
};

Frame Decode(std::string_view packet) {
  if (packet.size() < 80 || packet.size() > 4096 || packet.substr(0, 5) != std::string_view("WTDS\1", 5) ||
      Number(packet, 6, 2) != packet.size() - 80) throw InvalidFrame();
  Frame frame{static_cast<unsigned char>(packet[5]), std::string(packet.substr(8, 16)), {}, Number(packet, 72, 8), packet.substr(80)};
  frame.auth.set_save_id(std::string(packet.substr(24, 16)));
  frame.auth.set_credential(std::string(packet.substr(40, 32)));
  const bool enroll = frame.operation == 1;
  const bool mutation = frame.operation == 2 || frame.operation == 4 || frame.operation == 6;
  if (frame.operation < 1 || frame.operation > 7 || !detail::NonzeroBytes(frame.request_id, 16) ||
      !detail::NonzeroBytes(frame.auth.credential(), 32) ||
      enroll == detail::NonzeroBytes(frame.auth.save_id(), 16) ||
      mutation != (frame.counter != 0) || frame.counter == std::numeric_limits<std::uint64_t>::max()) throw InvalidFrame();
  constexpr std::size_t sizes[] = {0, 0, 176, 16, 16, 16, 104, 9};
  if (enroll) {
    if (frame.body.size() < 5 || frame.body.size() > 69 || Number(frame.body, 4, 1) != frame.body.size() - 5) throw InvalidFrame();
  } else if (frame.body.size() != sizes[frame.operation]) throw InvalidFrame();
  if ((frame.operation == 4 || frame.operation == 5 || frame.operation == 6) &&
      !detail::NonzeroBytes(frame.body.substr(0, 16), 16)) throw InvalidFrame();
  if (frame.operation == 2 && Number(frame.body, 0, 8) == 0) throw InvalidFrame();
  if (frame.operation == 6 && (!detail::NonzeroBytes(frame.body.substr(16, 16), 16) || Number(frame.body, 96, 8) == 0)) throw InvalidFrame();
  if (frame.operation == 7 && (Number(frame.body, 8, 1) < 1 || Number(frame.body, 8, 1) > 8)) throw InvalidFrame();
  return frame;
}

bool Known(const google::protobuf::Message &message) {
  return message.GetReflection()->GetUnknownFields(message).empty();
}

std::string Snapshot(const v1::TradeSnapshot &snapshot, std::uint64_t highwater) {
  Require(Known(snapshot));
  const auto state = snapshot.state();
  const auto kind = snapshot.delivery_kind();
  Require(state >= v1::TRADE_NONE && state <= v1::TRADE_ACKNOWLEDGED && kind >= v1::DELIVERY_NONE && kind <= v1::DELIVERY_RETURN);
  const bool no_delivery = snapshot.received_asset_id().empty() && snapshot.received_sha256().empty() && snapshot.receipt_id().empty();
  if (state == v1::TRADE_NONE) {
    Require(snapshot.trade_id().empty() && snapshot.offered_asset_id().empty() && snapshot.offered_sha256().empty() &&
        snapshot.last_event() == 0 && kind == v1::DELIVERY_NONE && no_delivery);
  } else {
    Require(detail::NonzeroBytes(snapshot.trade_id(), 16) && detail::NonzeroBytes(snapshot.offered_asset_id(), 16) &&
        snapshot.offered_sha256().size() == 32 && snapshot.last_event() > 0 && snapshot.last_event() <= highwater);
    if (state == v1::TRADE_QUEUED) Require(kind == v1::DELIVERY_NONE && no_delivery);
    else {
      Require(detail::NonzeroBytes(snapshot.received_asset_id(), 16) && snapshot.received_sha256().size() == 32 &&
          detail::NonzeroBytes(snapshot.receipt_id(), 16) && kind != v1::DELIVERY_NONE);
      Require(state != v1::TRADE_MATCHED || kind == v1::DELIVERY_MATCH);
      Require(state != v1::TRADE_RETURN_READY || kind == v1::DELIVERY_RETURN);
      if (kind == v1::DELIVERY_RETURN) Require(snapshot.received_asset_id() == snapshot.offered_asset_id() &&
          snapshot.received_sha256() == snapshot.offered_sha256());
      else Require(snapshot.received_asset_id() != snapshot.offered_asset_id());
    }
  }
  return detail::EncodeOutcome(0, snapshot).substr(8);
}

std::string Event(const v1::TradeEvent &event) {
  Require(Known(event) && event.kind() >= v1::EVENT_ENROLLED && event.kind() <= v1::EVENT_REJECTED);
  const auto kind = event.kind();
  Require(event.state() >= v1::TRADE_NONE && event.state() <= v1::TRADE_ACKNOWLEDGED);
  if (kind == v1::EVENT_ENROLLED) {
    Require(event.sequence() == 1 && event.state() == v1::TRADE_NONE && event.trade_id().empty() && event.operation_id().empty() &&
        event.asset_id().empty() && event.payload_sha256().empty() && event.receipt_id().empty());
  } else {
    Require(event.sequence() > 1);
    if (kind == v1::EVENT_REJECTED) {
      Require(detail::NonzeroBytes(event.operation_id(), 16) && event.asset_id().empty() && event.receipt_id().empty() &&
          (event.payload_sha256().empty() || event.payload_sha256().size() == 32));
      Require(event.trade_id().empty() ? event.state() == v1::TRADE_NONE :
          detail::NonzeroBytes(event.trade_id(), 16) && event.state() != v1::TRADE_NONE);
    } else {
      Require(detail::NonzeroBytes(event.trade_id(), 16) && detail::NonzeroBytes(event.asset_id(), 16) && event.payload_sha256().size() == 32);
      Require(kind == v1::EVENT_MATCHED ? event.operation_id().empty() : detail::NonzeroBytes(event.operation_id(), 16));
      Require(kind == v1::EVENT_SUBMITTED ? event.receipt_id().empty() : detail::NonzeroBytes(event.receipt_id(), 16));
      Require(event.state() == static_cast<v1::TradeState>(kind - 1));
    }
  }
  std::string bytes;
  detail::AppendUnsigned(bytes, event.sequence(), 8);
  bytes += static_cast<char>(kind);
  for (const auto &field : {std::pair{&event.trade_id(), 16U}, std::pair{&event.operation_id(), 16U},
       std::pair{&event.asset_id(), 16U}, std::pair{&event.payload_sha256(), 32U}, std::pair{&event.receipt_id(), 16U}})
    bytes += field.first->empty() ? std::string(field.second, '\0') : *field.first;
  bytes += static_cast<char>(event.state());
  Require(bytes.size() == 106);
  return bytes;
}

std::string Envelope(const Frame &frame, const v1::ReplyContext &context, const std::string &body) {
  Require(Known(context) && context.result() >= v1::RESULT_OK && context.result() <= v1::RESULT_UNAVAILABLE);
  Require((context.next_counter() == 0) == (context.event_highwater() == 0));
  if (context.result() == v1::RESULT_OK) Require(context.next_counter() > 0 && context.event_highwater() > 0);
  else Require(body.empty());
  if (context.result() == v1::RESULT_UNAUTHENTICATED || context.result() == v1::RESULT_PROCESSING)
    Require(context.next_counter() == 0 && context.event_highwater() == 0);
  std::string bytes = "WTDS";
  bytes += '\1'; bytes += static_cast<char>(frame.operation);
  detail::AppendUnsigned(bytes, body.size(), 2);
  bytes += frame.request_id;
  detail::AppendUnsigned(bytes, context.result(), 2);
  detail::AppendUnsigned(bytes, 0, 2);
  detail::AppendUnsigned(bytes, context.next_counter(), 8);
  detail::AppendUnsigned(bytes, context.event_highwater(), 8);
  bytes += body;
  Require(bytes.size() <= 4096);
  return bytes;
}

template <typename Reply, typename Encode>
GatewayReply Finish(const Frame &frame, const grpc::Status &status, const Reply &reply, Encode encode) {
  if (!status.ok()) return {};
  Require(reply.ByteSizeLong() <= 4096 && Known(reply) && reply.has_context());
  std::string body;
  if (reply.context().result() == v1::RESULT_OK) body = encode(reply);
  else {
    auto extra = reply;
    extra.clear_context();
    Require(extra.ByteSizeLong() == 0);
  }
  return {GatewayReply::Status::Ok, Envelope(frame, reply.context(), body)};
}

std::string Outcome(const Frame &frame, const v1::MutationReply &reply) {
  Require(reply.has_outcome() && reply.operation_event() > 0 && reply.operation_event() <= reply.context().event_highwater() &&
      reply.operation_event() == reply.outcome().last_event() && reply.context().next_counter() > frame.counter);
  const auto expected = frame.operation == 2 ? v1::TRADE_QUEUED : frame.operation == 4 ? v1::TRADE_RETURN_READY : v1::TRADE_ACKNOWLEDGED;
  Require(reply.outcome().state() == expected);
  if (frame.operation == 2) Require(reply.outcome().offered_sha256() == frame.body.substr(144, 32));
  else Require(reply.outcome().trade_id() == frame.body.substr(0, 16));
  if (frame.operation == 6) Require(reply.outcome().receipt_id() == frame.body.substr(16, 16) &&
      reply.outcome().received_sha256() == frame.body.substr(32, 32));
  std::string bytes;
  detail::AppendUnsigned(bytes, reply.operation_event(), 8);
  return bytes + Snapshot(reply.outcome(), reply.context().event_highwater());
}

} // namespace

struct TradeGateway::Impl {
  std::unique_ptr<v1::SaveIdentity::Stub> identities;
  std::unique_ptr<v1::WonderTrade::Stub> trades;
  explicit Impl(const std::shared_ptr<grpc::Channel> &channel)
      : identities(v1::SaveIdentity::NewStub(channel)), trades(v1::WonderTrade::NewStub(channel)) {}
};

TradeGateway::TradeGateway(std::shared_ptr<grpc::Channel> channel) : impl_(std::make_unique<Impl>(channel)) {}
TradeGateway::~TradeGateway() = default;

GatewayReply TradeGateway::Exchange(std::string_view packet, grpc::ClientContext &context) {
  try {
    const auto frame = Decode(packet);
    context.set_deadline(std::min(context.deadline(), std::chrono::system_clock::now() + std::chrono::seconds(4)));
    if (frame.operation == 1) {
      v1::EnrollRequest request;
      request.set_enrollment_id(frame.request_id);
      request.set_credential(frame.auth.credential());
      request.set_client_build(Number(frame.body, 0, 4));
      request.set_hints(std::string(frame.body.substr(5)));
      v1::EnrollReply reply;
      const auto status = impl_->identities->Enroll(&context, request, &reply);
      return Finish(frame, status, reply, [](const auto &value) {
        Require(detail::NonzeroBytes(value.save_id(), 16));
        return value.save_id();
      });
    }
    if (frame.operation == 2) {
      v1::SubmitRequest request;
      *request.mutable_context() = frame.Mutation();
      request.set_escrow_generation(Number(frame.body, 0, 8));
      request.mutable_pokemon()->set_pk4(std::string(frame.body.substr(8, 136)));
      request.mutable_pokemon()->set_sha256(std::string(frame.body.substr(144, 32)));
      v1::MutationReply reply;
      const auto status = impl_->trades->Submit(&context, request, &reply);
      return Finish(frame, status, reply, [&](const auto &value) { return Outcome(frame, value); });
    }
    if (frame.operation == 3) {
      v1::StatusRequest request;
      *request.mutable_auth() = frame.auth;
      if (detail::NonzeroBytes(frame.body, 16)) request.set_trade_id(std::string(frame.body));
      v1::StatusReply reply;
      const auto status = impl_->trades->Status(&context, request, &reply);
      return Finish(frame, status, reply, [&](const auto &value) {
        Require(value.has_trade());
        if (detail::NonzeroBytes(frame.body, 16)) Require(value.trade().trade_id() == frame.body);
        return Snapshot(value.trade(), value.context().event_highwater());
      });
    }
    if (frame.operation == 4) {
      v1::CancelRequest request;
      *request.mutable_context() = frame.Mutation();
      request.set_trade_id(std::string(frame.body));
      v1::MutationReply reply;
      const auto status = impl_->trades->Cancel(&context, request, &reply);
      return Finish(frame, status, reply, [&](const auto &value) { return Outcome(frame, value); });
    }
    if (frame.operation == 5) {
      v1::CollectRequest request;
      *request.mutable_auth() = frame.auth;
      request.set_trade_id(std::string(frame.body));
      v1::CollectReply reply;
      const auto status = impl_->trades->Collect(&context, request, &reply);
      return Finish(frame, status, reply, [&](const auto &value) {
        Require(value.has_trade() && value.trade().state() >= v1::TRADE_MATCHED && value.has_pokemon() && Known(value.pokemon()) &&
            detail::CanonicalShape(value.pokemon().pk4()) && detail::HashBytes(value.pokemon().pk4()) == value.pokemon().sha256() &&
            value.pokemon().sha256() == value.trade().received_sha256());
        Require(value.trade().trade_id() == frame.body);
        return Snapshot(value.trade(), value.context().event_highwater()) + value.pokemon().pk4();
      });
    }
    if (frame.operation == 6) {
      v1::AcknowledgeRequest request;
      *request.mutable_context() = frame.Mutation();
      request.set_trade_id(std::string(frame.body.substr(0, 16)));
      request.set_receipt_id(std::string(frame.body.substr(16, 16)));
      request.set_delivered_sha256(std::string(frame.body.substr(32, 32)));
      request.set_applied_sha256(std::string(frame.body.substr(64, 32)));
      request.set_applied_generation(Number(frame.body, 96, 8));
      v1::MutationReply reply;
      const auto status = impl_->trades->Acknowledge(&context, request, &reply);
      return Finish(frame, status, reply, [&](const auto &value) { return Outcome(frame, value); });
    }
    v1::ReconcileRequest request;
    *request.mutable_auth() = frame.auth;
    request.set_after_event(Number(frame.body, 0, 8));
    request.set_limit(Number(frame.body, 8, 1));
    v1::ReconcileReply reply;
    const auto status = impl_->trades->Reconcile(&context, request, &reply);
    return Finish(frame, status, reply, [&](const auto &value) {
      Require(value.events_size() <= static_cast<int>(request.limit()) && value.context().event_highwater() >= request.after_event() &&
          value.events_size() <= value.context().event_highwater() - request.after_event());
      auto sequence = request.after_event();
      std::string events;
      for (const auto &event : value.events()) { Require(event.sequence() == ++sequence); events += Event(event); }
      Require(value.page_end() == sequence && value.has_more() == (sequence < value.context().event_highwater()) &&
          (!value.has_more() || value.events_size() == static_cast<int>(request.limit())));
      std::string bytes;
      bytes += static_cast<char>(value.events_size()); bytes += static_cast<char>(value.has_more());
      detail::AppendUnsigned(bytes, sequence, 8);
      return bytes + events;
    });
  } catch (const InvalidFrame &) {
    return {GatewayReply::Status::Invalid, {}};
  } catch (const std::exception &) {
    return {};
  }
}

} // namespace platinumwt::server
