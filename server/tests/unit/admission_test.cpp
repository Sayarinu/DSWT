#include "admission.h"

#include <gtest/gtest.h>

#include <atomic>
#include <future>
#include <stdexcept>
#include <thread>
#include <vector>

namespace dswt::server::detail {
namespace {

TEST(AdmissionTest, RatesAndParallelLimitsHaveIndependentRecoveryBudgets) {
  Quota::Clock::time_point now{};
  Quota quota({2, 1, 2, 1}, [&] { return now; });
  auto first = quota.Acquire("one");
  ASSERT_TRUE(first);
  EXPECT_FALSE(quota.Acquire("one"));
  auto second = quota.Acquire("two");
  ASSERT_TRUE(second);
  EXPECT_FALSE(quota.Acquire("three"));
  first.reset();
  first = quota.Acquire("one");
  ASSERT_TRUE(first);
  first.reset();
  EXPECT_FALSE(quota.Acquire("one"));
  now += std::chrono::milliseconds(999);
  EXPECT_FALSE(quota.Acquire("one"));
  now += std::chrono::milliseconds(1);
  EXPECT_TRUE(quota.Acquire("one"));
  EXPECT_FALSE(quota.Acquire(""));
  EXPECT_FALSE(quota.Acquire(std::string(65, 'x')));
}

TEST(AdmissionTest, KeyChurnCannotEvictActiveOrRecentlySpentBudgets) {
  Quota::Clock::time_point now{};
  Quota quota({2, 1, 1, 1}, [&] { return now; });
  auto held = quota.Acquire("held");
  ASSERT_TRUE(held);
  for (unsigned i = 0; i < 4095; ++i) ASSERT_TRUE(quota.Acquire(std::to_string(i)));
  EXPECT_FALSE(quota.Acquire("extra"));
  now += std::chrono::seconds(1);
  EXPECT_TRUE(quota.Acquire("0")); // Existing keys still work when the table is full.
  EXPECT_FALSE(quota.Acquire("extra"));
  now += std::chrono::seconds(60);
  EXPECT_TRUE(quota.Acquire("extra"));
  EXPECT_FALSE(quota.Acquire("held"));
  held.reset();
  EXPECT_TRUE(quota.Acquire("held"));
}

TEST(AdmissionTest, ConcurrencyIsAtomicAndLeasesOutliveTheirOwner) {
  Quota quota({4, 1, 16, 1});
  std::promise<void> release;
  auto released = release.get_future().share();
  std::atomic<unsigned> attempted{0}, admitted{0};
  std::vector<std::future<void>> pending;
  for (unsigned i = 0; i < 16; ++i) pending.push_back(std::async(std::launch::async, [&, i] {
    auto lease = quota.Acquire(std::to_string(i));
    if (lease) ++admitted;
    ++attempted;
    released.wait();
  }));
  while (attempted != 16) std::this_thread::yield();
  EXPECT_EQ(admitted, 4U);
  release.set_value();
  for (auto &future : pending) future.get();
  EXPECT_TRUE(quota.Acquire("after"));
  std::unique_ptr<Quota::Permit> survivor;
  { Quota temporary({1, 1, 1, 1}); survivor = temporary.Acquire("survivor"); }
  survivor.reset();
}

TEST(AdmissionTest, CredentialsFenceOwnerBudgetsAndNewTradesCannotUseRecoverySlots) {
  ServiceAdmission admission;
  auto verified = [&](RequestKind kind, std::string_view id, std::string_view credential) {
    auto permit = admission.Acquire(kind);
    if (!permit || !admission.Authorize(*permit, id, credential)) return std::unique_ptr<Quota::Permit>{};
    return permit;
  };
  const std::string owner(16, 'A'), secret(32, 'Q');
  std::vector<std::unique_ptr<Quota::Permit>> submissions, recovery;
  for (int i = 0; i < 2; ++i) submissions.push_back(verified(RequestKind::Submission, owner, secret));
  ASSERT_TRUE(submissions[0]);
  ASSERT_TRUE(submissions[1]);
  EXPECT_FALSE(verified(RequestKind::Submission, owner, secret));
  EXPECT_TRUE(verified(RequestKind::Submission, owner, std::string(32, 'X')));
  EXPECT_TRUE(admission.Authorize(*submissions[0], owner, secret)); // A second transaction does not recharge.
  for (int i = 0; i < 2; ++i) submissions.push_back(verified(RequestKind::Enrollment, std::string(16, 'B' + i), secret));
  ASSERT_TRUE(submissions[2]);
  ASSERT_TRUE(submissions[3]);
  EXPECT_FALSE(verified(RequestKind::Enrollment, std::string(16, 'D'), secret));
  for (int i = 0; i < 8; ++i) {
    recovery.push_back(verified(RequestKind::Recovery, std::string(16, 'A' + i / 2), secret));
    ASSERT_TRUE(recovery.back());
  }
  EXPECT_FALSE(verified(RequestKind::Recovery, std::string(16, 'E'), secret));
  recovery.clear();
  EXPECT_TRUE(verified(RequestKind::Recovery, owner, secret));
}

TEST(AdmissionTest, ConfiguredSubmissionCapacityKeepsPerIdentityAndRecoveryLimits) {
  ServiceAdmission admission(6);
  const std::string secret(32, 'Q');
  std::vector<std::unique_ptr<Quota::Permit>> held;
  for (int i = 0; i < 6; ++i) {
    held.push_back(admission.Acquire(RequestKind::Submission));
    ASSERT_TRUE(held.back());
    ASSERT_EQ(admission.Authorize(*held.back(), std::string(16, 'A' + i / 2), secret), true);
  }
  EXPECT_FALSE(admission.Acquire(RequestKind::Submission));
  held.pop_back();
  auto third = admission.Acquire(RequestKind::Submission);
  ASSERT_TRUE(third);
  EXPECT_FALSE(admission.Authorize(*third, std::string(16, 'A'), secret)); // Still two per identity.
  EXPECT_TRUE(admission.Acquire(RequestKind::Recovery));
  EXPECT_THROW(ServiceAdmission(0), std::invalid_argument);
}

TEST(AdmissionTest, UnauthenticatedWorkCannotPopulateTheIdentityCache) {
  ServiceAdmission admission;
  for (unsigned i = 0; i < 8192; ++i) ASSERT_TRUE(admission.Acquire(RequestKind::Recovery));
  auto verified = admission.Acquire(RequestKind::Recovery);
  ASSERT_TRUE(verified);
  for (unsigned i = 0; i < 100; ++i)
    ASSERT_TRUE(admission.Authorize(*verified, std::string(16, 'A'), std::string(32, 'Q')));
}

TEST(AdmissionTest, NetworksNormalizeMappedAddressesAndGroupIpv6PrivacyAddresses) {
  auto key = [](const char *ip) { return NetworkKey(boost::asio::ip::make_address(ip)); };
  EXPECT_EQ(key("192.0.2.1"), key("::ffff:192.0.2.1"));
  EXPECT_NE(key("192.0.2.1"), key("192.0.2.2"));
  EXPECT_EQ(key("2001:db8:1234:5678::1"), key("2001:db8:1234:5678:ffff:ffff:ffff:ffff"));
  EXPECT_NE(key("2001:db8:1234:5678::1"), key("2001:db8:1234:5679::1"));
  EXPECT_NE(key("::1"), key("0.0.0.1"));
}

} // namespace
} // namespace dswt::server::detail
