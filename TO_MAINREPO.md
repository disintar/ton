# Upstream PR Notes

This document summarizes the current state of the private-overlay sync work and
the pieces that should be split into clean pull requests for
`ton-blockchain/ton`.

## Current Status

The production test image `mainnet-v4-dev` now contains the final tested state
from commit `f7275ed4a1e650e9e6a79a19327157c11dc1c02b`.

It was rolled out only to `worm` during validation. After restart, `worm`
transitioned from archive catch-up into live sync without the previous shard
client bounce:

- `shard_client_ago` dropped from roughly `82s` to `22s`, then to about `1s`.
- `last_masterchain_block_seqno` and `shard_client_masterchain_seqno` stayed
  equal during the follow-up monitor window.
- Live shard-client logs showed the expected chain:
  `use_pending -> wait_state.done(ms=0..18) -> applied_all_shards`.
- `Non solvable` broadcast noise during archive recovery was quieted; real
  errors such as bad signatures still remain warnings.

No other nodes were restarted for this validation pass.

## What Was Wrong

### 1. Custom Overlay Was Broadcast-Only For Archive Recovery

Private/custom overlays could deliver live block broadcasts, but
`FullNodeCustomOverlay::Callback::receive_query()` did not answer archive
queries. A node that restarted behind the network could see fresh private
overlay broadcasts while still being forced to recover missing history through
the public overlay.

This produced startup failures and slow recovery such as:

`failed to download and import archive slice: [Error : 651 : no nodes]`

The node was not started yet, so fresh broadcasts were normal to receive but not
usable until archive recovery completed.

### 2. Private Overlay Catch-Up Did Not Have Public Fallback Everywhere

Some private-overlay catch-up attempts could spend too long on an unreachable or
slow custom peer. When custom sync did not answer quickly, fallback to the
public overlay needed to happen in every recovery path, not only in selected
archive paths.

The desired behavior is:

- Try private overlay peers first.
- Keep all waits bounded.
- If private peers cannot serve the data, immediately fall back to the public
  full-node overlay.
- Never make public sync worse than upstream behavior.

### 3. Archive Recovery Stopped Too Far From Live

The archive recovery handoff used a large freshness window. That allowed the
archive path to stop while the node was still tens of seconds behind live.
After that, shard-client had to bridge a live gap through ordinary live
notifications and block-state waits, which made the graph show a visible
archive-to-live bounce.

### 4. ShardClient Dropped Masterchain Notifications While Busy

`ShardClient::new_masterchain_block_notification()` used the upstream pattern of
ignoring notifications when the shard client was not waiting. During archive
catch-up and save-to-db work, fresh masterchain notifications could arrive and
be dropped. After `saved_to_db()`, shard-client then had to rediscover the next
state through the slower database/state-wait path.

This was the direct cause of the persistent post-archive bounce: masterchain
could be live while shard-client stayed behind or repeatedly retried the same
transition.

### 5. Recovery Logs Were Noisy

While a node is still in archive recovery, it can legitimately fail to process
some broadcasts because the required context is not yet available. Logging
`broadcast is forbidden`, temporary bans, and `Non solvable` as warnings made it
hard to see real transport or state-sync failures.

## What Changed

### Custom Overlay Archive Sync

Custom overlays can now participate in archive recovery:

- Answer archive info and archive slice requests from private overlay members.
- Resolve custom overlay peers through ADNL/DHT when only the short ADNL is
  present in the overlay config.
- Use the configured overlay sender path for custom archive queries.
- Add bounded waits around DHT resolve, archive-info, and archive-slice chunks.
- Fall back to public archive sync on no peers, timeout, bad data, or import
  failure.
- Export custom/public sync metrics and add grep-friendly trace logs for custom
  archive attempts.

Safety: downloaded archive data still goes through the existing archive import
and validation path. A bad private peer cannot advance state.

### Custom Overlay Block Catch-Up

Block catch-up now tries explicit private overlay peers before public fallback:

- Prefer custom overlay senders and members as download peers.
- Race bounded private peers instead of relying on one random peer.
- Use custom overlay path for block and next-block catch-up where applicable.
- Keep public overlay fallback for private timeout, no data, invalid data, or
  unavailable peers.
- Add metrics/logging for custom sync attempts and fallback decisions.

Safety: downloaded blocks still pass the existing proof, hash, and validation
checks. The private path only changes peer selection and fallback timing.

### Faster Public Fallback

Public archive recovery was also hardened:

- Retry archive slices across public neighbours.
- Race public fallback paths where a single selected peer can stall.
- Add transport logs for public archive peer choice and result.
- Keep public fallback available after failed custom import.

This preserves upstream behavior as the fallback floor while making recovery
less dependent on one unlucky public peer.

### Near-Live Archive Handoff

`ValidatorManagerImpl::out_of_sync()` now keeps archive recovery running closer
to live before switching to live processing. The tested value reduces the
handoff window from roughly 80 seconds to roughly 8 seconds.

This makes the archive-to-live transition small enough that live shard-client
notifications can bridge it immediately.

### ShardClient Pending Masterchain Notifications

Shard-client now keeps a bounded pending map of masterchain notifications:

`seqno -> (BlockHandle, MasterchainState)`

When a notification arrives while shard-client is busy:

- Old/current seqnos are ignored.
- Future seqnos are buffered.
- The buffer is capped and pruned.

After `saved_to_db()`:

- Shard-client first tries the exact next pending masterchain notification.
- The exact next block id/hash must match.
- If no exact pending notification exists, it falls back to the original
  `get_block_handle()` / `wait_block_state()` path.

This fixed the observed live handoff. On `worm`, pending live notifications were
applied immediately after archive catch-up and shard-client stayed in lockstep
with masterchain.

Safety: the buffer is bounded, does not skip masterchain blocks, and does not
change validation rules.

### Recovery Log Levels

Expected recovery-time broadcast failures were reduced from warning to info:

- `broadcast is forbidden`
- `peer is temporary banned`
- `Non solvable`

Real failures, for example bad signatures, remain warnings.

## Upstream PR Split

These should be prepared as separate upstream PRs:

1. `full-node: allow archive slice sync over custom overlays`
2. `full-node: use custom overlay peers for block catch-up downloads`
3. `full-node: keep public fallback fast for archive and block sync`
4. `validator: keep archive recovery close to live before handoff`
5. `validator: buffer shard-client masterchain notifications while busy`
6. `overlay: reduce expected recovery broadcast log noise`

The first three are transport and peer-selection improvements. The shard-client
pending-notification change is independent and should be reviewed separately.
The log-level change is also independent and can be a small PR.

## Test Plan For Upstream

- Build `validator-engine` and `validator`.
- Run at least two archive nodes in the same private/custom overlay.
- Restart one node while it is behind enough to trigger archive import.
- Verify private overlay archive attempts are visible in logs/metrics.
- Verify public fallback still completes sync when private peers are disabled or
  unreachable.
- Verify `shard_client_masterchain_seqno` catches up without a post-archive
  bounce.
- Verify `wait_state.done` after live handoff is normally in milliseconds.
- Verify the pending notification buffer does not grow without bound during a
  deliberately stalled shard-client.
- Verify recovery-time `Non solvable` broadcast logs are not warnings, while
  bad signatures and real errors remain warnings.

## Files Touched By This Work

- `validator/full-node-custom-overlays.*`
- `validator/full-node.*`
- `validator/full-node-shard.*`
- `validator/net/download-archive-slice.*`
- `validator/custom-overlay-metrics.h`
- `validator/shard-client.*`
- `validator/manager.*`
- `validator/import-db-slice.*`
- `validator/interfaces/validator-manager.h`
- `overlay/overlay.cpp`
- `validator-engine/prometheus/PrometheusExporterActor.cpp`

There are also local product changes in `lite-server-daemon/adnl-lite-proxy.cpp`
for LiteServer usage reporting and balancer behavior. Those should not be mixed
into the TON upstream sync PRs.
