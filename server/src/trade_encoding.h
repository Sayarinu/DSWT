#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "trade.pb.h"

namespace dswt::server::detail {

bool NonzeroBytes(std::string_view bytes, std::size_t size);
bool CanonicalShape(std::string_view bytes);
std::string HexBytes(std::string_view bytes);
std::string UnhexBytes(std::string_view hex);
std::uint64_t Unsigned(std::string_view decimal);
std::string HashBytes(std::string_view bytes);
std::string RandomIdentifier();
void AppendUnsigned(std::string &bytes, std::uint64_t value, std::size_t width);
std::string MutationHash(unsigned char opcode, const v1::MutationContext &context,
                          std::string_view body);
std::string EncodeOutcome(std::uint64_t sequence, const v1::TradeSnapshot &snapshot);
void DecodeOutcome(std::string_view bytes, v1::MutationReply &reply);

} // namespace dswt::server::detail
