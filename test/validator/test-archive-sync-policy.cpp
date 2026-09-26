// SPDX-License-Identifier: LGPL-2.0-or-later
#include "td/utils/tests.h"
#include "validator/archive-sync-policy.h"

using namespace ton::validator;

TEST(ArchiveSync, HandsBoundedArchiveGapToLiveDownloader) {
  ASSERT_TRUE(archive_sync_near_live(100.0, 70.0, 56.0));
  ASSERT_TRUE(archive_sync_near_live(100.0, 40.001, 40.001));
}

TEST(ArchiveSync, RequiresBothChainsStrictlyWithinWindow) {
  ASSERT_TRUE(!archive_sync_near_live(100.0, 40.0, 99.0));
  ASSERT_TRUE(!archive_sync_near_live(100.0, 99.0, 40.0));
  ASSERT_TRUE(!archive_sync_near_live(100.0, 99.0, 30.0));
  ASSERT_TRUE(!archive_sync_near_live(100.0, 30.0, 99.0));
}

TEST(ArchiveSync, CatchupGraceExpiresEvenWithoutProgress) {
  const double deadline = 100.0 + kLiveSyncCatchupGraceSeconds;
  ASSERT_TRUE(!live_archive_recovery_needed(44.0, 44.0, false, 100.0, deadline));
  ASSERT_TRUE(!live_archive_recovery_needed(100.0, 9.0, true, 159.999, deadline));
  ASSERT_TRUE(live_archive_recovery_needed(100.0, 110.0, true, 159.999, deadline));
  ASSERT_TRUE(live_archive_recovery_needed(100.0, 110.0, true, 160.0, deadline));
  ASSERT_EQ(kLiveSyncCatchupGraceSeconds, 60.0);
}

TEST(ArchiveSync, RecoversSevereShardGapDuringCatchupGrace) {
  ASSERT_TRUE(!live_archive_recovery_needed(1.0, 10.0, true, 105.0, 160.0));
  ASSERT_TRUE(live_archive_recovery_needed(1.0, 10.001, true, 105.0, 160.0));
  ASSERT_TRUE(!live_archive_recovery_needed(1.0, 25.0, false, 105.0, 160.0));
}

TEST(ArchiveSync, NormalRecoveryBoundsResumeAfterGrace) {
  ASSERT_TRUE(!live_archive_recovery_needed(1.0, 2.0, false, 160.0, 160.0));
  ASSERT_TRUE(!live_archive_recovery_needed(30.0, 30.0, false, 160.0, 160.0));
  ASSERT_TRUE(live_archive_recovery_needed(30.001, 1.0, false, 160.0, 160.0));
  ASSERT_TRUE(live_archive_recovery_needed(1.0, 30.001, false, 160.0, 160.0));
  ASSERT_TRUE(live_archive_recovery_needed(1.0, 1.0, true, 160.0, 160.0));
  ASSERT_TRUE(live_archive_recovery_needed(44.0, 44.0, false, 100.0, 0.0));
  ASSERT_EQ(kLiveArchiveSyncRecoveryLagSeconds, 30.0);
}
