#include "platinumwt/server/wal_archive_barrier.h"

#include <gtest/gtest.h>

namespace platinumwt::server {
namespace {

constexpr std::uint64_t kSegment = 16 * 1024 * 1024;

TEST(WalCoverageTest, LastArchivedSegmentCoversThroughItsEnd) {
  const auto coverage = NextWalCoverage({}, "000000010000000000000003", "000000010000000000000005", kSegment);
  EXPECT_EQ(coverage.timeline, 1U);
  EXPECT_EQ(coverage.end, 4 * kSegment);
}

TEST(WalCoverageTest, SegmentNamesCountWithinLogicalFiles) {
  // 256 16 MiB segments fill one 4 GiB logical file.
  const auto coverage = NextWalCoverage({}, "000000010000000200000001", "000000010000000200000002", kSegment);
  EXPECT_EQ(coverage.end, 2 * 0x100000000ULL + 2 * kSegment);
}

TEST(WalCoverageTest, HonorsNonDefaultSegmentSizes) {
  const std::uint64_t size = 1024 * 1024;
  EXPECT_EQ(NextWalCoverage({}, "000000010000000000000009", "00000001000000000000000A", size).end, 10 * size);
}

TEST(WalCoverageTest, NothingArchivedCoversNothing) {
  const WalCoverage prior{1, 4 * kSegment};
  EXPECT_EQ(NextWalCoverage(prior, std::nullopt, "000000010000000000000005", kSegment).end, 0U);
}

TEST(WalCoverageTest, OlderTimelineCoversNothing) {
  const auto coverage = NextWalCoverage({1, 4 * kSegment}, "000000010000000000000003", "000000020000000000000005", kSegment);
  EXPECT_EQ(coverage.end, 0U);
}

TEST(WalCoverageTest, ArchivedPastCurrentPositionCoversNothing) {
  EXPECT_EQ(NextWalCoverage({}, "000000010000000000000009", "000000010000000000000005", kSegment).end, 0U);
}

TEST(WalCoverageTest, PriorPastCurrentPositionIsDiscarded) {
  const auto coverage = NextWalCoverage({1, 40 * kSegment}, "000000010000000000000003", "000000010000000000000005", kSegment);
  EXPECT_EQ(coverage.end, 4 * kSegment);
}

TEST(WalCoverageTest, HistoryFilesKeepCoverageOnTheSameTimelineOnly) {
  const WalCoverage prior{1, 4 * kSegment};
  for (const auto *name : {"000000010000000000000003.00000028.backup", "00000002.history"}) {
    EXPECT_EQ(NextWalCoverage(prior, name, "000000010000000000000005", kSegment).end, prior.end);
    EXPECT_EQ(NextWalCoverage({}, name, "000000010000000000000005", kSegment).end, 0U);
    EXPECT_EQ(NextWalCoverage(prior, name, "000000020000000000000005", kSegment).end, 0U);
  }
}

TEST(WalCoverageTest, MalformedObservationsCoverNothing) {
  const WalCoverage prior{1, 4 * kSegment};
  EXPECT_EQ(NextWalCoverage(prior, "000000010000000000000003", "", kSegment).end, 0U);
  EXPECT_EQ(NextWalCoverage(prior, "000000010000000000000003", "000000010000000000000005", 0).end, 0U);
  EXPECT_EQ(NextWalCoverage(prior, "000000010000000000000003", "000000010000000000000005", 3 * 1024 * 1024).end, 0U);
  EXPECT_EQ(NextWalCoverage({}, "00000001000000000000000a", "000000010000000000000010", kSegment).end, 0U);
}

TEST(WalArchiveBarrierTest, UnreachableDatabaseStaysClosedAndStopsPromptly) {
  RecoveryBarrier barrier;
  {
    WalArchiveBarrier archive("host=/nonexistent port=1 dbname=none");
    barrier = archive.Barrier();
    EXPECT_FALSE(barrier(1));
  }
  EXPECT_FALSE(barrier(1));
}

} // namespace
} // namespace platinumwt::server
