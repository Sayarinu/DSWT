#include "dswt/server/identity_store.h"

#include "game_variant.h"
#include "postgres.h"
#include "identity_auth.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace dswt::server {
namespace {

template <std::size_t N>
bool Nonzero(const std::array<unsigned char, N> &bytes) {
  return std::any_of(bytes.begin(), bytes.end(),
                     [](auto value) { return value != 0; });
}

template <std::size_t N>
std::string Hex(const std::array<unsigned char, N> &bytes) {
  constexpr char digits[] = "0123456789abcdef";
  std::string result;
  result.reserve(N * 2);
  for (auto value : bytes) {
    result += digits[value >> 4];
    result += digits[value & 15];
  }
  return result;
}

template <std::size_t N>
std::array<unsigned char, N> Unhex(std::string_view text) {
  if (text.size() != N * 2)
    throw std::runtime_error("invalid stored identity data");
  auto nibble = [](char value) -> unsigned char {
    if (value >= '0' && value <= '9')
      return value - '0';
    if (value >= 'a' && value <= 'f')
      return value - 'a' + 10;
    throw std::runtime_error("invalid stored identity data");
  };
  std::array<unsigned char, N> result{};
  for (std::size_t index = 0; index < N; ++index)
    result[index] = static_cast<unsigned char>(nibble(text[index * 2]) * 16 +
                                               nibble(text[index * 2 + 1]));
  return result;
}

IdentityId RandomId() {
  IdentityId result{};
  if (RAND_bytes(result.data(), result.size()) != 1)
    throw std::runtime_error("identity randomness unavailable");
  return result;
}

SaveCredential Verifier(const IdentityId &id,
                        const SaveCredential &credential) {
  constexpr char domain[] = "DSWT save credential v1";
  std::array<unsigned char, sizeof(domain) + 16 + 32> input{};
  std::copy_n(domain, sizeof(domain), input.begin());
  std::copy(id.begin(), id.end(), input.begin() + sizeof(domain));
  std::copy(credential.begin(), credential.end(),
            input.begin() + sizeof(domain) + 16);
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  std::size_t size = 0;
  const int status = EVP_Q_digest(nullptr, "SHA256", nullptr, input.data(),
                                  input.size(), digest.data(), &size);
  OPENSSL_cleanse(input.data(), input.size());
  if (status != 1 || size != 32)
    throw std::runtime_error("credential verification unavailable");
  SaveCredential result{};
  std::copy_n(digest.begin(), result.size(), result.begin());
  OPENSSL_cleanse(digest.data(), digest.size());
  return result;
}

bool Matches(const IdentityId &id, const SaveCredential &credential,
             std::string_view stored) {
  const auto expected = Verifier(id, credential);
  const auto actual = stored.empty() ? SaveCredential{} : Unhex<32>(stored);
  return CRYPTO_memcmp(expected.data(), actual.data(), actual.size()) == 0;
}

} // namespace

bool detail::CredentialMatches(std::string_view save_id,
                               std::string_view credential,
                               std::string_view stored_hex) {
  if (save_id.size() != 16 || credential.size() != 32)
    return false;
  IdentityId id{};
  SaveCredential secret{};
  std::copy(save_id.begin(), save_id.end(), id.begin());
  std::copy(credential.begin(), credential.end(), secret.begin());
  const bool result = Matches(id, secret, stored_hex);
  OPENSSL_cleanse(secret.data(), secret.size());
  return result;
}

struct IdentityStore::Impl {
  detail::Database database;
  IdGenerator generator;
  Impl(const std::string &info, IdGenerator random)
      : database(info), generator(random ? std::move(random) : RandomId) {}
};

IdentityStore::IdentityStore(const std::string &info, IdGenerator generator)
    : impl_(std::make_unique<Impl>(info, std::move(generator))) {}
IdentityStore::~IdentityStore() = default;

std::optional<IdentityId>
IdentityStore::Enroll(const IdentityId &enrollment_id,
                      const SaveCredential &credential,
                      std::uint32_t variant) {
  if (!Nonzero(enrollment_id) || !Nonzero(credential) ||
      !detail::ValidVariant(variant))
    return std::nullopt;
  for (int attempt = 0; attempt < 8; ++attempt) {
    try {
      detail::Transaction transaction(impl_->database);
      auto existing = impl_->database.Parameters(
          "SELECT replace(save_id::text, '-', ''), encode(credential_verifier, "
          "'hex'), variant "
          "FROM dswt.save_identities JOIN dswt.identity_variants "
          "USING (save_id) WHERE enrollment_id = $1::uuid",
          {Hex(enrollment_id)});
      if (PQntuples(existing.get()) == 1) {
        const auto id = Unhex<16>(PQgetvalue(existing.get(), 0, 0));
        if (!Matches(id, credential, PQgetvalue(existing.get(), 0, 1)) ||
            !detail::SameVariant(static_cast<std::uint32_t>(std::stoul(
                                     PQgetvalue(existing.get(), 0, 2))),
                                 variant))
          return std::nullopt;
        transaction.Commit();
        return id;
      }
      const auto id = impl_->generator();
      if (!Nonzero(id))
        continue;
      auto inserted = impl_->database.Parameters(
          "INSERT INTO dswt.save_identities(save_id, enrollment_id, "
          "credential_verifier) "
          "VALUES ($1::uuid, $2::uuid, decode($3, 'hex')) ON CONFLICT "
          "(enrollment_id) DO NOTHING RETURNING save_id",
          {Hex(id), Hex(enrollment_id), Hex(Verifier(id, credential))});
      if (PQntuples(inserted.get()) == 0)
        continue;
      impl_->database.Parameters(
          "INSERT INTO dswt.trade_events(save_id, sequence, kind, state) "
          "VALUES ($1::uuid, 1, 1, 0)",
          {Hex(id)});
      impl_->database.Parameters(
          "INSERT INTO dswt.identity_variants VALUES ($1::uuid, $2::bigint)",
          {Hex(id), std::to_string(variant)});
      transaction.Commit();
      return id;
    } catch (const detail::DatabaseError &error) {
      if (error.SqlState() != "23505" ||
          error.Constraint() != "save_identities_pkey")
        throw;
    }
  }
  throw std::runtime_error("identity allocation unavailable");
}

bool IdentityStore::Authenticate(const IdentityId &save_id,
                                 const SaveCredential &credential) {
  if (!Nonzero(save_id) || !Nonzero(credential))
    return false;
  auto row = impl_->database.Parameters(
      "SELECT encode(credential_verifier, 'hex') "
      "FROM dswt.save_identities WHERE save_id = $1::uuid",
      {Hex(save_id)});
  const bool found = PQntuples(row.get()) == 1;
  const bool equal =
      Matches(save_id, credential, found ? PQgetvalue(row.get(), 0, 0) : "");
  return found && equal;
}

bool IdentityStore::OwnsTrade(const IdentityId &save_id,
                              const SaveCredential &credential,
                              const IdentityId &trade_id) {
  if (!Nonzero(trade_id) || !Authenticate(save_id, credential))
    return false;
  auto row = impl_->database.Parameters(
      "SELECT 1 FROM dswt.offers "
      "WHERE save_id = $1::uuid AND trade_id = $2::uuid",
      {Hex(save_id), Hex(trade_id)});
  return PQntuples(row.get()) == 1;
}

} // namespace dswt::server
