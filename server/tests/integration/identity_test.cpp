#include "platinumwt/server/identity_store.h"

#include "postgres.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <exception>
#include <future>
#include <iomanip>
#include <sstream>

namespace platinumwt::server {
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
    const auto result = store.Enroll(enrollment, credential);
    if (!result)
      throw std::runtime_error("fixture enrollment failed");
    return *result;
  }

  IdentityId Offer(const IdentityId &owner) {
    const auto trade = NextId(), asset = NextId(), operation = NextId();
    detail::Transaction transaction(database);
    database.Parameters("INSERT INTO platinumwt.payloads VALUES ($1::uuid, "
                        "decode(repeat('00',136),'hex'), "
                        "pg_catalog.sha256(decode(repeat('00',136),'hex')))",
                        {AsHex(asset)});
    database.Parameters(
        "INSERT INTO "
        "platinumwt.offers(trade_id,save_id,offered_asset_id,state,last_event) "
        "VALUES ($1::uuid,$2::uuid,$3::uuid,1,2)",
        {AsHex(trade), AsHex(owner), AsHex(asset)});
    database.Parameters("INSERT INTO platinumwt.ownership VALUES "
                        "($1::uuid,$2::uuid,$3::uuid,1,2)",
                        {AsHex(asset), AsHex(owner), AsHex(trade)});
    database.Parameters(
        "INSERT INTO platinumwt.operations VALUES ($1::uuid,$2::uuid,1,2,"
        "decode(repeat('11',32),'hex'),1,2,decode(repeat('00',146),'hex'))",
        {AsHex(owner), AsHex(operation)});
    database.Parameters(
        "INSERT INTO "
        "platinumwt.trade_events(save_id,sequence,kind,trade_id,operation_id,"
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
  EXPECT_EQ(store.Enroll(enrollment, credential), id);
  IdentityStore reopened("");
  EXPECT_EQ(reopened.Enroll(enrollment, credential), id);
  EXPECT_TRUE(reopened.Authenticate(id, credential));
}

TEST_F(IdentityTest, ConflictingCredentialRevealsNoIdentity) {
  const auto id = Enroll();
  EXPECT_FALSE(store.Enroll(enrollment, Credential(0x52)));
  EXPECT_FALSE(store.Authenticate(id, Credential(0x52)));
  EXPECT_TRUE(store.Authenticate(id, credential));
}

TEST_F(IdentityTest, UnknownAndZeroInputsDoNotAuthenticateOrEnroll) {
  EXPECT_FALSE(store.Authenticate(NextId(), credential));
  EXPECT_FALSE(store.Authenticate({}, credential));
  EXPECT_FALSE(store.Enroll({}, credential));
  EXPECT_FALSE(store.Enroll(enrollment, {}));
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
  EXPECT_EQ(colliding.Enroll(other_enrollment, Credential(0x52)), replacement);
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
  EXPECT_THROW(colliding.Enroll(other_enrollment, Credential(0x52)),
               std::runtime_error);
  EXPECT_EQ(calls, 8);
  auto row =
      database.Parameters("SELECT count(*) FROM platinumwt.save_identities "
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
  EXPECT_THROW(zero.Enroll(enrollment, credential), std::runtime_error);
  EXPECT_EQ(calls, 8);
  IdentityStore failed("", []() -> IdentityId {
    throw std::runtime_error("randomness unavailable");
  });
  EXPECT_THROW(failed.Enroll(enrollment, credential), std::runtime_error);
  EXPECT_TRUE(store.Enroll(enrollment, credential));
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
                   "setval(pg_get_serial_sequence('platinumwt.audit_journal','"
                   "sequence'),1,false)");
  std::exception_ptr failure;
  try {
    allocating.Enroll(other_enrollment, credential);
  } catch (...) {
    failure = std::current_exception();
  }
  database.Execute(
      "SELECT "
      "setval(pg_get_serial_sequence('platinumwt.audit_journal','sequence'),"
      "(SELECT max(sequence) FROM platinumwt.audit_journal),true)");
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
  const auto second = store.Enroll(NextId(), credential);
  ASSERT_TRUE(second);
  auto row = database.Parameters(
      "SELECT encode(credential_verifier,'hex') FROM "
      "platinumwt.save_identities "
      "WHERE save_id IN ($1::uuid,$2::uuid) ORDER BY save_id",
      {AsHex(first), AsHex(*second)});
  ASSERT_EQ(PQntuples(row.get()), 2);
  EXPECT_TRUE(std::string(PQgetvalue(row.get(), 0, 0)) != AsHex(credential));
  EXPECT_TRUE(std::string(PQgetvalue(row.get(), 0, 0)) !=
              PQgetvalue(row.get(), 1, 0));
  auto binary =
      database.Parameters("SELECT credential_verifier FROM "
                          "platinumwt.save_identities WHERE save_id=$1::uuid",
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
  const auto second = store.Enroll(NextId(), Credential(0x52));
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
    return separate.Enroll(enrollment, credential);
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
    return separate.Enroll(enrollment, secret);
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
} // namespace platinumwt::server
