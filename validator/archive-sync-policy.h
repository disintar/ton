// SPDX-License-Identifier: LGPL-2.0-or-later
#pragma once

namespace ton::validator {

// Archive validation itself can take twenty seconds per hundred blocks.
// Let the live downloader close the final bounded gap, without allowing an
// immediate recovery back into archives before it has had time to progress.
constexpr double kPrestartArchiveSyncTargetLagSeconds = 60.0;
constexpr double kLiveArchiveSyncRecoveryLagSeconds = 30.0;
constexpr double kLiveSyncCatchupGraceSeconds = 60.0;
constexpr double kLiveArchiveEmergencyShardLagSeconds = 10.0;
constexpr unsigned int kLiveArchiveEmergencyShardGapBlocks = 32;
constexpr unsigned int kLiveArchiveShardGapBlocks = 16;

inline bool archive_sync_near_live(double now, double masterchain_time, double shard_client_time) {
  return masterchain_time + kPrestartArchiveSyncTargetLagSeconds > now &&
         shard_client_time + kPrestartArchiveSyncTargetLagSeconds > now;
}

// now and grace_until use the monotonic clock; lag values use block timestamps.
inline bool live_archive_recovery_needed(double master_lag, double shard_lag, unsigned int shard_gap_blocks,
                                         double now, double grace_until) {
  if (shard_gap_blocks > kLiveArchiveEmergencyShardGapBlocks ||
      (shard_gap_blocks > kLiveArchiveShardGapBlocks && shard_lag > kLiveArchiveEmergencyShardLagSeconds)) {
    return true;
  }
  if (now < grace_until) {
    return false;
  }
  return master_lag > kLiveArchiveSyncRecoveryLagSeconds ||
         shard_lag > kLiveArchiveSyncRecoveryLagSeconds || shard_gap_blocks > kLiveArchiveShardGapBlocks;
}

}  // namespace ton::validator
