// SPDX-License-Identifier: LGPL-2.0-or-later
#pragma once

namespace ton::validator {

// Give live block download a bounded catch-up window. Requiring archives to
// reach a two-second head can keep warm restarts in archive mode indefinitely.
// Both chains must be fresh; the gap to recovery avoids immediate mode churn.
constexpr double kPrestartArchiveSyncTargetLagSeconds = 20.0;
constexpr double kLiveArchiveSyncRecoveryLagSeconds = 30.0;
static_assert(kPrestartArchiveSyncTargetLagSeconds < kLiveArchiveSyncRecoveryLagSeconds);

inline bool archive_sync_near_live(double now, double masterchain_time, double shard_client_time) {
  return masterchain_time + kPrestartArchiveSyncTargetLagSeconds > now &&
         shard_client_time + kPrestartArchiveSyncTargetLagSeconds > now;
}

}  // namespace ton::validator
