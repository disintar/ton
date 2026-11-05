# Upstream PR Notes

This document summarizes the current state of the private-overlay sync work and
the pieces that should be split into clean pull requests for
`ton-blockchain/ton`.

## 2026-11-05 Bounded Archive Import Canary

Echo's archive downloads completed over private QUIC in 50-135 ms, but its
master timestamp lag shrank only 42 seconds over 10 minutes and shard lag only
14 seconds. Application/storage, rather than transport, limited catch-up.

The network archive importer now prestores file-hash-checked block bytes in
bounded windows (at most 8 outstanding writes and 32 MiB of block payload).
This exposes independent archive writes to existing async/group-commit paths
before the unchanged sequential proof/state application. Oversized entries
keep the original on-demand path. No fsync, proof, predecessor or state check
is removed, and prestoring bytes does not mark blocks applied.

Eligible state-GC requests yield for 100 ms only while archive recovery is
active. Eligibility checks remain intact and GC continues; live mode restores
the normal cadence. Import summaries expose prestore, master/shard application,
overall elapsed time and MC blocks/s; GC yield summaries are limited to one
per 10 seconds. No unbounded prefetch or whole-archive payload cache is added.

Build on Solana at a pinned commit and compare only echo before/after. Do not
deploy this canary to the other TON nodes. This patch is separate from the
persistent-state serializer's full-preload memory issue.

## 2026-11-02 Next-Block Source Rotation Canary

Prometheus recorded master/shard stalls approaching 180 seconds. Worm and echo
stopped at the same masterchain predecessor while London and Solana advanced.
A short trace also showed repeated empty replies from the same two public
sources, despite successful private downloads from Solana.

The canary patch keeps a single bounded negative-cache entry per peer, scoped
to the exact masterchain predecessor. Empty replies rotate that predecessor
for 500 ms without penalizing the peer's other blocks/shards or its general
reliability. Transport failures rotate for 1 second. Late public responses for
an obsolete predecessor do not affect the current choice.

Private next-block requests hedge a second eligible peer after 100 ms and now
feed failures back to source selection. Local DB success is not credited as a
network RTT. Public source selection expands from 2 to 4 after 1 second without
progress, then to at most 8 after 5 seconds, with staggered starts and existing
deadlines/token limits. Slow rounds log predecessor, block age, stalled time,
candidate count and selected peer IDs at most once per 2 seconds.

Hash/proof/state verification, public fallback, archive recovery thresholds,
QUIC MTU limits and request admission limits are unchanged. Build remotely on
Solana at a pinned SHA and deploy only to echo under a separate canary tag.
This is a source-selection repair, not a claim that all lag causes are solved.

Deployment validation on 2026-11-02:

- Source `6f3a3efd255db5f1b42098531a9b31e4002851dc`, built on Solana through
  `/root/ton-k8s/build-for-k8s.sh` using build/ccache; no local compilation.
- Image `mainnet-v4-dev-nextsync-6f3a3efd`, digest
  `sha256:19bd423282cac19da0860e00b21ac48e18f64f8e501ad0c7db0a41999a7c503f`.
- Echo canary: 939 seconds, 61 metric collection attempts, 44 valid samples;
  unavailable Kubernetes exec responses were excluded, never treated as zero.
  Sampled master lag mean 1.09 seconds, p95 2, maximum 2; shard-client mean
  1.32 seconds, p95 2, maximum 4. No container restarts during the canary.
- Traces confirmed successful private next-block requests to worm, London and
  Solana, plus successful public fallback after source expansion. A stalled
  request after MC 102888819 recovered through public sources in 1.1-1.2 seconds;
  one after 102888780 recovered through London private sync.
- After echo, deployed strictly London -> Solana -> worm. For each database,
  stopped and removed the old pod before starting its replacement. Advanced to
  the next node only after three fresh, advancing master/shard samples with
  both timestamp lags at most 1 second. All replacement containers had zero
  restarts at the end of rollout, and all four used the verified digest.
- Short 3-4 second stalls still occur. This validation does not establish a
  permanent 1-second lag ceiling or zero inter-node block propagation spread.

Diagnostic captures: `/tmp/echo-nextsync-6f3a3efd/` and
`/tmp/nextsync-cluster-rollout-20261102-131633/` on the operator workstation.

## 2026-10-23 Merge Regression Repair

The v2026.10 integration lost upstream `9c07336d`'s custom-overlay
`send_to_custom` guard and retained 256-entry forwarding caches. Custom
receipts could be re-originated as fresh twostep broadcasts before validator
rejection of stale shard descriptions, amplifying stale traffic indefinitely.

Restore the upstream block-sender guard for blocks, candidates, and shard
descriptions, plus bounded 10000-entry LRUs. Keep public/fast-sync forwarding
and normal validation/download fallback intact. New aggregate Prometheus
counters distinguish received, re-originated, suppressed, and deduplicated
shard descriptions; logs are capped to one summary per 10 seconds.

This is a fork merge repair, not an upstream feature request. Rollout starts
on London only. Other old members can continue to inject looping traffic;
do not declare the entire overlay fixed from a single canary's incoming rate.

London canary validation on 2026-10-23:

- Image `mainnet-v4-dev-ton2026-10-stormguard-421347bc`, source
  `421347bc31a5bb966365a9dfc22c267b5ca0c280`, registry digest
  `sha256:7fb4dc057e17224c3797018d6dcd475ec5d443c6ac3828d7b9595210414586d1`.
- Restart recovered master and shard sync; three consecutive samples had both
  lags at most 1 second. The new pod had zero restarts during observation.
- A subsequent 90-second capture measured about 660 received shard
  descriptions/s, matched by 660 suppressed/s; sent remained zero. Normal
  custom block broadcasts continued to arrive. This proves the forwarding
  guard, not disappearance of the storm from other members.
- Total pod TX did not decrease: about 15.8 MB/s before versus 17.1 MB/s after;
  RX was about 13.2 versus 22.2 MB/s. These include all pod traffic, not only
  custom overlay traffic, and are not evidence of a cluster-wide improvement.
- The 90-second window had master p95/max 1/2 seconds and shard p95/max 10/15
  seconds; its last 30 seconds were around 0-1 seconds. Do not claim all sync
  spikes resolved. Remaining old members must stop re-originating the storm.
- File and runtime console inventories on all four K8s nodes showed the same
  six members, still including `R43cwaCI...`; no replacement archive ADNLs
  were present. Membership was not guessed or silently changed.
- Diagnostics are saved under `/tmp/ton-storm-before` and
  `/tmp/ton-storm-after` on the operator workstation. Temporary London
  verbosity was restored to 2.

Cluster rollout completed later on 2026-10-23 after user approval:

- Updated Solana, then worm, then echo to the same stormguard image/digest.
  Each next deployment waited for three fresh, advancing samples with both
  master and shard lag at most 1 second. London was not restarted again.
- Worm recovered through archive to live; its logs showed private QUIC
  archive fetches from echo, which stayed online until worm caught up.
- All four running pods had zero restarts and matching image digests at the
  final check. Master/shard lag was London 0/0, Solana 1/1, worm 0/0, echo 1/1.
- A simultaneous 10-second sample on all four nodes showed zero increments
  of received, sent, suppressed, and deduplicated shard-description counters.
  This is short-window evidence that the observed stale-description loop
  stopped, not a guarantee that all future sync spikes are eliminated.
- Fixed-London baseline immediately before the remaining rollout was about
  989 stale descriptions/s, RX 17.76 MB/s and TX 14.80 MB/s. After completing
  the cluster rollout, London measured RX 3.12 MB/s and TX 5.25 MB/s, with
  zero description-counter increments. These are total pod traffic rates.
- Existing QUIC stream-size-limit and Plumtree repair-cap warnings were still
  visible during echo startup; this repair does not claim to fix those
  distinct transport issues. Membership and environment were unchanged.
- Final evidence: `/tmp/ton-storm-cluster-rollout-final.json`; deployment
  baseline: `/tmp/ton-storm-remaining-before.yaml` on the operator workstation.

The previously recorded deployment status below is historical, not the
current image inventory.

## Historical Sync Validation

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
