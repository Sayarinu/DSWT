#include "trade_encoding.h"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <charconv>
#include <stdexcept>

namespace dswt::server::detail {

bool NonzeroBytes(std::string_view bytes, std::size_t size) {
  return bytes.size() == size && std::any_of(bytes.begin(), bytes.end(), [](char c) { return c != 0; });
}

bool CanonicalShape(std::string_view bytes) {
  if (bytes.size() != 136 || bytes[4] != 0 || bytes[5] != 0 ||
      bytes[0x64] != 0 || bytes[0x65] != 0 || bytes[0x66] != 0 || bytes[0x67] != 0)
    return false;
  auto word = [&](std::size_t offset) {
    return static_cast<unsigned char>(bytes[offset]) |
           (static_cast<unsigned char>(bytes[offset + 1]) << 8);
  };
  unsigned sum = 0;
  for (std::size_t offset = 8; offset < bytes.size(); offset += 2) sum += word(offset);
  return (sum & 65535) == static_cast<unsigned>(word(6));
}

std::string HexBytes(std::string_view bytes) {
  constexpr char digits[] = "0123456789abcdef";
  std::string result;
  result.reserve(bytes.size() * 2);
  for (unsigned char value : bytes) {
    result += digits[value >> 4];
    result += digits[value & 15];
  }
  return result;
}

std::string UnhexBytes(std::string_view hex) {
  auto nibble = [](char c) -> unsigned char {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    throw std::runtime_error("invalid stored binary value");
  };
  if (hex.size() % 2 != 0) throw std::runtime_error("invalid stored binary size");
  std::string result;
  result.reserve(hex.size() / 2);
  for (std::size_t i = 0; i < hex.size(); i += 2)
    result += static_cast<char>(nibble(hex[i]) * 16 + nibble(hex[i + 1]));
  return result;
}

std::uint64_t Unsigned(std::string_view decimal) {
  std::uint64_t result = 0;
  auto parsed = std::from_chars(decimal.data(), decimal.data() + decimal.size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr != decimal.data() + decimal.size())
    throw std::runtime_error("invalid stored counter");
  return result;
}

std::string HashBytes(std::string_view bytes) {
  std::string digest(32, '\0');
  std::size_t length = 0;
  if (EVP_Q_digest(nullptr, "SHA256", nullptr, bytes.data(), bytes.size(),
                   reinterpret_cast<unsigned char *>(digest.data()), &length) != 1 || length != 32)
    throw std::runtime_error("digest unavailable");
  return digest;
}

std::string RandomIdentifier() {
  for (int attempt = 0; attempt < 8; ++attempt) {
    std::string id(16, '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char *>(id.data()), 16) != 1)
      throw std::runtime_error("identifier randomness unavailable");
    if (NonzeroBytes(id, 16)) return id;
  }
  throw std::runtime_error("identifier allocation unavailable");
}

void AppendUnsigned(std::string &bytes, std::uint64_t value, std::size_t width) {
  for (std::size_t i = 0; i < width; ++i) {
    bytes += static_cast<char>(value & 255);
    value >>= 8;
  }
}

std::string MutationHash(unsigned char opcode, const v1::MutationContext &context,
                          std::string_view body) {
  std::string frame = "WTDS";
  frame += '\1';
  frame += static_cast<char>(opcode);
  AppendUnsigned(frame, body.size(), 2);
  frame += context.operation_id();
  frame += context.auth().save_id();
  AppendUnsigned(frame, context.counter(), 8);
  frame += body;
  return HashBytes(frame);
}

std::string EncodeOutcome(std::uint64_t sequence, const v1::TradeSnapshot &snapshot) {
  auto fixed = [](std::string_view field, std::size_t size) {
    if (field.empty()) return std::string(size, '\0');
    if (field.size() != size) throw std::runtime_error("invalid stored snapshot");
    return std::string(field);
  };
  std::string bytes;
  AppendUnsigned(bytes, sequence, 8);
  bytes += fixed(snapshot.trade_id(), 16);
  bytes += static_cast<char>(snapshot.state());
  bytes += static_cast<char>(snapshot.delivery_kind());
  bytes += fixed(snapshot.offered_asset_id(), 16);
  bytes += fixed(snapshot.offered_sha256(), 32);
  bytes += fixed(snapshot.received_asset_id(), 16);
  bytes += fixed(snapshot.received_sha256(), 32);
  bytes += fixed(snapshot.receipt_id(), 16);
  AppendUnsigned(bytes, snapshot.last_event(), 8);
  return bytes;
}

void DecodeOutcome(std::string_view bytes, v1::MutationReply &reply) {
  if (bytes.size() != 146) throw std::runtime_error("invalid stored outcome");
  auto take = [&](std::size_t count) {
    auto value = bytes.substr(0, count);
    bytes.remove_prefix(count);
    return std::string(value);
  };
  auto number = [&](std::size_t count) {
    std::uint64_t value = 0;
    auto part = take(count);
    for (std::size_t i = 0; i < count; ++i)
      value |= static_cast<std::uint64_t>(static_cast<unsigned char>(part[i])) << (8 * i);
    return value;
  };
  auto field = [&](std::size_t size) {
    auto value = take(size);
    return NonzeroBytes(value, size) ? value : std::string{};
  };
  reply.set_operation_event(number(8));
  auto *snapshot = reply.mutable_outcome();
  snapshot->set_trade_id(field(16));
  snapshot->set_state(static_cast<v1::TradeState>(number(1)));
  snapshot->set_delivery_kind(static_cast<v1::DeliveryKind>(number(1)));
  snapshot->set_offered_asset_id(field(16));
  snapshot->set_offered_sha256(take(32));
  snapshot->set_received_asset_id(field(16));
  auto received_hash = take(32);
  if (!snapshot->received_asset_id().empty()) snapshot->set_received_sha256(received_hash);
  snapshot->set_receipt_id(field(16));
  snapshot->set_last_event(number(8));
}

} // namespace dswt::server::detail
