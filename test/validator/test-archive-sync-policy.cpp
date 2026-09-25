// SPDX-License-Identifier: LGPL-2.0-or-later
#include "td/utils/tests.h"
#include "validator/archive-sync-policy.h"

using namespace ton::validator;

TEST(ArchiveSync, HandsSmallWarmRestartGapToLiveDownloader) {
  ASSERT_TRUE(archive_sync_near_live(100.0, 90.0, 85.0));
  ASSERT_TRUE(archive_sync_near_live(100.0, 80.001, 80.001));
}

TEST(ArchiveSync, RequiresBothChainsStrictlyWithinWindow) {
  ASSERT_TRUE(!archive_sync_near_live(100.0, 80.0, 99.0));
  ASSERT_TRUE(!archive_sync_near_live(100.0, 99.0, 80.0));
  ASSERT_TRUE(!archive_sync_near_live(100.0, 99.0, 70.0));
  ASSERT_TRUE(!archive_sync_near_live(100.0, 70.0, 99.0));
}

TEST(ArchiveSync, LeavesRecoveryHysteresis) {
  // A just-admitted state remains below the recovery threshold even after
  // nine seconds without progress. Recovery itself keeps its thirty-second bound.
  const double oldest_admitted = 80.001;
  ASSERT_TRUE(archive_sync_near_live(100.0, oldest_admitted, oldest_admitted));
  ASSERT_TRUE(109.0 - oldest_admitted < kLiveArchiveSyncRecoveryLagSeconds);
  ASSERT_EQ(kLiveArchiveSyncRecoveryLagSeconds, 30.0);
}
