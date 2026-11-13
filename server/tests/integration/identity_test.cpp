#include "dswt/server/identity_store.h"

#include "game_variant.h"
#include "postgres.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <exception>
#include <future>
#include <iomanip>
#include <sstream>

namespace dswt::server {
namespace {

IdentityId NextId() {
  static std::atomic<unsigned> serial{1};
  const unsigned value = serial++;
  IdentityId id{};
  id[0] = static_cast<unsigned char>(value >> 8);
  id[1] = static_cast<unsigned char>(value);
  return id;
}

SaveCredential Credential(unsigned char value = 0x51) {
  SaveCredential credential{};
  credential.fill(value);
  return credential;
}

template <std::size_t N>
std::string AsHex(const std::array<unsigned char, N> &bytes) {
  std::ostringstream stream;
  stream << std::hex << std::setfill('0');
  for (auto value : bytes)
    stream << std::setw(2) << static_cast<unsigned>(value);
  return stream.str();
}

class IdentityTest : public ::testing::Test {
protected:
  IdentityStore store{""};
  detail::Database database{""};
  IdentityId enrollment = NextId();
  SaveCredential credential = Credential();

  IdentityId Enroll() {
    const auto result = store.Enroll(enrollment, credential, detail::kLegacyPlatinum);
    if (!result)
      throw std::runtime_error("fixture enrollment failed");
    return *result;
  }

  std::string StoredVariant(const IdentityId &id) {
    auto row = database.Parameters(
        "SELECT variant FROM dswt.identity_variants WHERE save_id=$1::uuid",
        {AsHex(id)});
    return PQntuples(row.get()) == 1 ? PQgetvalue(row.get(), 0, 0) : "";
  }

  IdentityId Offer(const IdentityId &owner) {
    const auto trade = NextId(), asset = NextId(), operation = NextId();
    detail::Transaction transaction(database);
    database.Parameters("INSERT INTO dswt.payloads VALUES ($1::uuid, "
                        "decode(repeat('00',136),'hex'), "
                        "pg_catalog.sha256(decode(repeat('00',136),'hex')))",
                        {AsHex(asset)});
    database.Parameters(
        "INSERT INTO "
        "dswt.offers(trade_id,save_id,offered_asset_id,state,last_event) "
        "VALUES ($1::uuid,$2::uuid,$3::uuid,1,2)",
        {AsHex(trade), AsHex(owner), AsHex(asset)});
    database.Parameters("INSERT INTO dswt.offer_destinations VALUES ($1::uuid,2)", {AsHex(trade)});
    database.Parameters("INSERT INTO dswt.ownership VALUES "
                        "($1::uuid,$2::uuid,$3::uuid,1,2)",
                        {AsHex(asset), AsHex(owner), AsHex(trade)});
    database.Parameters(
        "INSERT INTO dswt.operations VALUES ($1::uuid,$2::uuid,1,2,"
        "decode(repeat('11',32),'hex'),1,2,decode(repeat('00',146),'hex'))",
        {AsHex(owner), AsHex(operation)});
    database.Parameters(
        "INSERT INTO "
        "dswt.trade_events(save_id,sequence,kind,trade_id,operation_id,"
        "asset_id,payload_sha256,state) VALUES "
        "($1::uuid,2,2,$2::uuid,$3::uuid,$4::uuid,"
        "pg_catalog.sha256(decode(repeat('00',136),'hex')),1)",
        {AsHex(owner), AsHex(trade), AsHex(operation), AsHex(asset)});
    transaction.Commit();
    return trade;
  }
};

TEST_F(IdentityTest, RetryAndReconnectPreserveIdentity) {
  const auto id = Enroll();
  EXPECT_EQ(store.Enroll(enrollment, credential, detail::kLegacyPlatinum), id);
  IdentityStore reopened("");
  EXPECT_EQ(reopened.Enroll(enrollment, credential, detail::kLegacyPlatinum), id);
  EXPECT_TRUE(reopened.Authenticate(id, credential));
}

TEST_F(IdentityTest, ConflictingCredentialRevealsNoIdentity) {
  const auto id = Enroll();
  EXPECT_FALSE(store.Enroll(enrollment, Credential(0x52), detail::kLegacyPlatinum));
  EXPECT_FALSE(store.Authenticate(id, Credential(0x52)));
  EXPECT_TRUE(store.Authenticate(id, credential));
}

TEST_F(IdentityTest, EveryGameFamilyStoresItsVariant) {
  using detail::GameFamily;
  const std::uint32_t variants[] = {
      detail::kLegacyPlatinum,
      detail::GameVariant(GameFamily::DiamondPearl, 10, 1, 0),
      detail::GameVariant(GameFamily::DiamondPearl, 11, 8, 0),
      detail::GameVariant(GameFamily::Platinum, 12, 2, 1),
      detail::GameVariant(GameFamily::Platinum, 12, 5, 0),
      detail::GameVariant(GameFamily::HeartGoldSoulSilver, 7, 3, 0),
      detail::GameVariant(GameFamily::HeartGoldSoulSilver, 8, 7, 0)};
  for (const auto variant : variants) {
    const auto nonce = NextId();
    const auto id = store.Enroll(nonce, credential, variant);
    ASSERT_TRUE(id) << variant;
    EXPECT_EQ(StoredVariant(*id), std::to_string(variant));
    EXPECT_EQ(store.Enroll(nonce, credential, variant), id);
  }
}

TEST_F(IdentityTest, UnknownVariantNeverCreatesIdentity) {
  using detail::GameFamily;
  for (const std::uint32_t variant :
       {0U, 2U, 0xFFFFFFFFU, detail::GameVariant(GameFamily::None, 12, 2, 0),
        detail::GameVariant(GameFamily::Platinum, 10, 2, 0),
        detail::GameVariant(GameFamily::DiamondPearl, 12, 2, 0),
        detail::GameVariant(GameFamily::HeartGoldSoulSilver, 9, 2, 0),
        detail::GameVariant(GameFamily::Platinum, 12, 0, 0),
        detail::GameVariant(GameFamily::Platinum, 12, 6, 0),
        detail::GameVariant(GameFamily::Platinum, 12, 9, 0),
        detail::GameVariant(static_cast<GameFamily>(4), 12, 2, 0)}) {
    EXPECT_FALSE(store.Enroll(enrollment, credential, variant)) << variant;
  }
  auto row = database.Parameters("SELECT count(*) FROM dswt.save_identities "
                                 "WHERE enrollment_id=$1::uuid",
                                 {AsHex(enrollment)});
  EXPECT_EQ(std::string(PQgetvalue(row.get(), 0, 0)), "0");
}

TEST_F(IdentityTest, RetryWithAnotherVariantConflictsAndKeepsStoredVariant) {
  using detail::GameFamily;
  const auto pearl = detail::GameVariant(GameFamily::DiamondPearl, 11, 2, 0);
  const auto id = store.Enroll(enrollment, credential, pearl);
  ASSERT_TRUE(id);
  for (const auto variant :
       {detail::kLegacyPlatinum,
        detail::GameVariant(GameFamily::DiamondPearl, 10, 2, 0),
        detail::GameVariant(GameFamily::DiamondPearl, 11, 3, 0),
        detail::GameVariant(GameFamily::DiamondPearl, 11, 2, 1)})
    EXPECT_FALSE(store.Enroll(enrollment, credential, variant)) << variant;
  EXPECT_EQ(StoredVariant(*id), std::to_string(pearl));
  EXPECT_EQ(store.Enroll(enrollment, credential, pearl), id);
}

TEST_F(IdentityTest, LegacyAliasMatchesEnglishPlatinumWithoutChangingIt) {
  using detail::GameFamily;
  const auto english = detail::GameVariant(GameFamily::Platinum, 12, 2, 1);
  const auto id = Enroll();
  EXPECT_EQ(store.Enroll(enrollment, credential, english), id);
  EXPECT_FALSE(store.Enroll(enrollment, credential,
                            detail::GameVariant(GameFamily::Platinum, 12, 3, 1)));
  EXPECT_EQ(StoredVariant(id), "1");
  const auto other = NextId();
  const auto explicit_id = store.Enroll(other, credential, english);
  ASSERT_TRUE(explicit_id);
  EXPECT_EQ(store.Enroll(other, credential, detail::kLegacyPlatinum),
            explicit_id);
  EXPECT_EQ(StoredVariant(*explicit_id), std::to_string(english));
}

TEST_F(IdentityTest, UnknownAndZeroInputsDoNotAuthenticateOrEnroll) {
  EXPECT_FALSE(store.Authenticate(NextId(), credential));
  EXPECT_FALSE(store.Authenticate({}, credential));
  EXPECT_FALSE(store.Enroll({}, credential, detail::kLegacyPlatinum));
  EXPECT_FALSE(store.Enroll(enrollment, {}, detail::kLegacyPlatinum));
  const auto id = Enroll();
  EXPECT_FALSE(store.Authenticate(id, {}));
  EXPECT_FALSE(store.OwnsTrade(id, credential, {}));
}

TEST_F(IdentityTest, SaveIdCollisionRetriesWithoutChangingExistingIdentity) {
  const auto existing = Enroll(), replacement = NextId(),
             other_enrollment = NextId();
  int calls = 0;
  IdentityStore colliding(
      "", [&] { return calls++ == 0 ? existing : replacement; });
  EXPECT_EQ(colliding.Enroll(other_enrollment, Credential(0x52), detail::kLegacyPlatinum), replacement);
  EXPECT_EQ(calls, 2);
  EXPECT_TRUE(store.Authenticate(existing, credential));
  EXPECT_TRUE(store.Authenticate(replacement, Credential(0x52)));
}

TEST_F(IdentityTest, CollisionExhaustionDoesNotLeavePartialIdentity) {
  const auto existing = Enroll(), other_enrollment = NextId();
  int calls = 0;
  IdentityStore colliding("", [&] {
    ++calls;
    return existing;
  });
  EXPECT_THROW(colliding.Enroll(other_enrollment, Credential(0x52), detail::kLegacyPlatinum),
               std::runtime_error);
  EXPECT_EQ(calls, 8);
  auto row =
      database.Parameters("SELECT count(*) FROM dswt.save_identities "
                          "WHERE enrollment_id=$1::uuid",
                          {AsHex(other_enrollment)});
  EXPECT_EQ(std::string(PQgetvalue(row.get(), 0, 0)), "0");
  EXPECT_TRUE(store.Authenticate(existing, credential));
}

TEST_F(IdentityTest, ZeroOrFailedRandomnessNeverCreatesIdentity) {
  int calls = 0;
  IdentityStore zero("", [&] {
    ++calls;
    return IdentityId{};
  });
  EXPECT_THROW(zero.Enroll(enrollment, credential, detail::kLegacyPlatinum), std::runtime_error);
  EXPECT_EQ(calls, 8);
  IdentityStore failed("", []() -> IdentityId {
    throw std::runtime_error("randomness unavailable");
  });
  EXPECT_THROW(failed.Enroll(enrollment, credential, detail::kLegacyPlatinum), std::runtime_error);
  EXPECT_TRUE(store.Enroll(enrollment, credential, detail::kLegacyPlatinum));
}

TEST_F(IdentityTest, UnexpectedUniqueFailureIsNotRetried) {
  Enroll();
  const auto other_enrollment = NextId();
  int calls = 0;
  IdentityStore allocating("", [&] {
    ++calls;
    return NextId();
  });
  database.Execute("SELECT "
                   "setval(pg_get_serial_sequence('dswt.audit_journal','"
                   "sequence'),1,false)");
  std::exception_ptr failure;
  try {
    allocating.Enroll(other_enrollment, credential, detail::kLegacyPlatinum);
  } catch (...) {
    failure = std::current_exception();
  }
  database.Execute(
      "SELECT "
      "setval(pg_get_serial_sequence('dswt.audit_journal','sequence'),"
      "(SELECT max(sequence) FROM dswt.audit_journal),true)");
  ASSERT_TRUE(failure);
  try {
    std::rethrow_exception(failure);
  } catch (const detail::DatabaseError &error) {
    EXPECT_EQ(error.Constraint(), "audit_journal_pkey");
  }
  EXPECT_EQ(calls, 1);
}

TEST_F(IdentityTest, VerifierIsBoundToSaveAndCannotBeUsedAsCredential) {
  const auto first = Enroll();
  const auto second = store.Enroll(NextId(), credential, detail::kLegacyPlatinum);
  ASSERT_TRUE(second);
  auto row = database.Parameters(
      "SELECT encode(credential_verifier,'hex') FROM "
      "dswt.save_identities "
      "WHERE save_id IN ($1::uuid,$2::uuid) ORDER BY save_id",
      {AsHex(first), AsHex(*second)});
  ASSERT_EQ(PQntuples(row.get()), 2);
  EXPECT_TRUE(std::string(PQgetvalue(row.get(), 0, 0)) != AsHex(credential));
  EXPECT_TRUE(std::string(PQgetvalue(row.get(), 0, 0)) !=
              PQgetvalue(row.get(), 1, 0));
  auto binary =
      database.Parameters("SELECT credential_verifier FROM "
                          "dswt.save_identities WHERE save_id=$1::uuid",
                          {AsHex(first)});
  std::size_t length = 0;
  auto *decoded = PQunescapeBytea(
      reinterpret_cast<const unsigned char *>(PQgetvalue(binary.get(), 0, 0)),
      &length);
  ASSERT_EQ(length, credential.size());
  SaveCredential verifier{};
  std::copy_n(decoded, length, verifier.begin());
  PQfreemem(decoded);
  EXPECT_FALSE(store.Authenticate(first, verifier));
  EXPECT_TRUE(store.Authenticate(first, credential));
}

TEST_F(IdentityTest, TradeLookupBindsBothCredentialAndOwner) {
  const auto first = Enroll();
  const auto second = store.Enroll(NextId(), Credential(0x52), detail::kLegacyPlatinum);
  ASSERT_TRUE(second);
  const auto trade = Offer(first);
  EXPECT_TRUE(store.OwnsTrade(first, credential, trade));
  EXPECT_FALSE(store.OwnsTrade(*second, Credential(0x52), trade));
  EXPECT_FALSE(store.OwnsTrade(first, Credential(0x52), trade));
  EXPECT_FALSE(store.OwnsTrade(first, credential, NextId()));
}

TEST_F(IdentityTest, ConcurrentCopiedSaveEnrollmentHasOneIdentity) {
  std::promise<void> ready;
  auto start = ready.get_future().share();
  auto enroll = [&] {
    IdentityStore separate("");
    start.wait();
    return separate.Enroll(enrollment, credential, detail::kLegacyPlatinum);
  };
  auto first = std::async(std::launch::async, enroll);
  auto second = std::async(std::launch::async, enroll);
  ready.set_value();
  const auto a = first.get(), b = second.get();
  ASSERT_TRUE(a);
  EXPECT_EQ(a, b);
  EXPECT_TRUE(store.Authenticate(*a, credential));
  const auto trade = Offer(*a);
  IdentityStore copied("");
  EXPECT_TRUE(store.OwnsTrade(*a, credential, trade));
  EXPECT_TRUE(copied.OwnsTrade(*a, credential, trade));
}

TEST_F(IdentityTest, ConflictingConcurrentEnrollmentHasOneAuthorizedWinner) {
  std::promise<void> ready;
  auto start = ready.get_future().share();
  auto enroll = [&](SaveCredential secret) {
    IdentityStore separate("");
    start.wait();
    return separate.Enroll(enrollment, secret, detail::kLegacyPlatinum);
  };
  auto first = std::async(std::launch::async, enroll, credential);
  auto second = std::async(std::launch::async, enroll, Credential(0x52));
  ready.set_value();
  const auto a = first.get(), b = second.get();
  ASSERT_NE(static_cast<bool>(a), static_cast<bool>(b));
  const auto winner = a ? *a : *b;
  EXPECT_EQ(store.Authenticate(winner, credential), static_cast<bool>(a));
  EXPECT_EQ(store.Authenticate(winner, Credential(0x52)), static_cast<bool>(b));
}

} // namespace
} // namespace dswt::server
