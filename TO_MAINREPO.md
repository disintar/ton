# Upstream PR Notes

This document describes several independent improvements that should be prepared as
clean pull requests against `ton-blockchain/ton`.

## 1. Use Custom Overlays For Archive Slice Sync

### Problem

During validator startup, `ValidatorManagerImpl::prestart_sync()` imports archive
slices when the local node is out of sync. The current path is:

`ValidatorManagerImpl::download_next_archive()` ->
`ArchiveImporter::download_shard_archive()` ->
`ValidatorManagerInterface::send_download_archive_request()` ->
`FullNodeImpl::download_archive()` ->
`FullNodeShard::download_archive()` ->
`DownloadArchiveSlice`.

This asks the public full-node shard overlay for `tonNode_getArchiveInfo`,
`tonNode_getShardArchiveInfo`, and `tonNode_getArchiveSlice`.

`FullNodeCustomOverlay` currently only forwards broadcasts. Its overlay callback
has an empty `receive_query()` implementation, so custom/private overlay peers
cannot answer archive queries. A restarting node can therefore receive fresh
blocks through the custom overlay while still being unable to backfill the
archive slice required by prestart sync.

Observed symptom:

`Failed to download archive slice #<seqno> for shard <shard> error: [Error : 651 : no nodes]`

At the same time, custom overlay block broadcasts are received quickly, but
`ValidatorManager` remains not started and drops them.

### Proposed Change

Add archive query support to `FullNodeCustomOverlay`:

- Handle `tonNode_getArchiveInfo`.
- Handle `tonNode_getShardArchiveInfo`.
- Handle `tonNode_getArchiveSlice`.
- Reuse the existing validator manager archive APIs:
  `get_archive_id()` and `get_archive_slice()`.
- Reuse the full-node rate limiter for heavy `getArchiveSlice` replies.
- Reject queries from ADNL ids that are not members of the custom overlay.

Use custom overlays as a best-effort first path in `FullNodeImpl::download_archive()`:

- If the node has a matching custom overlay for the requested shard, try the
  custom overlay peers first.
- If the custom path is not ready, has no peers, has no archive slice, or times
  out, fall back to the existing public shard overlay path.
- Keep the existing public behavior unchanged for nodes without custom overlays
  and for external-client mode.

### Safety

The change is a fallback optimization, not a consensus change.

It does not trust custom peers blindly: downloaded archive slices still go
through the existing archive import/check path before advancing shard-client
state. A bad or missing custom reply falls back to the public path.

The responder is scoped to private overlay members and rate limited. Large
payloads continue to use the existing `DownloadArchiveSlice` chunking and
`getArchiveSlice` size guard.

### Test Plan

- Unit/build: build `validator` target.
- Integration:
  - Run two archive nodes in the same custom overlay with the patch.
  - Restart one node while it is behind enough to trigger prestart archive import.
  - Verify logs contain `Trying custom overlay "<name>" archive slice #...`.
  - Verify the target peer logs custom overlay `getShardArchiveInfo` and
    `getArchiveSlice`.
  - Verify prestart sync completes and manager starts accepting custom overlay
    block broadcasts.
  - Stop or remove the archive slice from custom peers and verify fallback to
    the public overlay still works.

### Upstream Status

Checked with:

`gh api repos/ton-blockchain/ton/contents/validator/full-node-custom-overlays.cpp --jq '.content' | base64 -d`

The upstream `FullNodeCustomOverlay::Callback::receive_query()` body is empty.

## 2. Buffer ShardClient Masterchain Notifications While Busy

### Problem

`ShardClient::new_masterchain_block_notification()` drops incoming masterchain
notifications when `waiting_ == false`.

That means if shard-client is applying/saving the previous masterchain block,
the next masterchain state can arrive and be discarded. After `saved_to_db()`,
the client then has to re-enter the slower path:

`get_block_handle()` -> `wait_block_state()` -> `new_masterchain_block_notification()`

This can create catch-up bursts where different nodes have the same network
propagation but different local `shard_client_masterchain_seqno`, which shows up
as shard seqno spread.

### Proposed Change

Keep a bounded pending notification map:

`seqno -> (BlockHandle, MasterchainState)`

When a notification arrives while shard-client is busy:

- Ignore old/current seqnos.
- Store future seqnos in the pending map.
- Prune old entries.
- Cap the map at `MAX_PENDING_MASTERCHAIN_NOTIFICATIONS` entries.

After `saved_to_db()`:

- Try to apply the exact next masterchain block from the pending map.
- Require both seqno and full block id/hash to match `one_next(true)`.
- If no exact next notification is buffered, fall back to the existing
  `get_block_handle()` / `wait_block_state()` path.

### Safety

This is memory bounded. The current patch caps the pending map at 16
masterchain notifications. If shard-client is stuck for a long time, the map
does not grow without bound; far-future entries are dropped and the old fallback
path remains available.

The code does not skip masterchain blocks. It applies only the exact next block
expected by `masterchain_block_handle_->one_next(true)` and drops hash
mismatches.

The change is local scheduling/caching. It does not affect validation rules,
block data, signatures, or consensus.

### Test Plan

- Unit/build: build `validator` target.
- Integration:
  - Enable propagation/shard-client trace on two nodes.
  - Compare `shardclient.mc_notification -> saved_to_db` before/after.
  - Monitor `shard_client_masterchain_seqno` spread and shard seqno spread.
  - Confirm no unbounded memory growth during high block rate or a stuck
    shard-client.

### Upstream Status

Checked with:

`gh api repos/ton-blockchain/ton/contents/validator/shard-client.cpp --jq '.content' | base64 -d`

The upstream `ShardClient::new_masterchain_block_notification()` still contains
`if (!waiting_) { return; }`.

## Suggested PR Split

## 3. Use Custom Overlay Peers For Block Catch-Up Downloads

### Problem

Custom overlays deliver live block broadcasts quickly, but a restarting node can
still be behind by hundreds or thousands of masterchain blocks. The sequential
catch-up path asks for block data through `DownloadBlockNew`.

The public shard path creates `DownloadBlockNew("downloadnext", ...)` with an
explicit public overlay peer chosen by `choose_neighbour()`. The custom overlay
path must be equally explicit. Sending a custom download with a zero
`download_from` lets `DownloadBlockNew` pick one random overlay peer and fail
the whole attempt if that peer has no data or is slow.

Observed symptom:

- `ton_custom_overlay_block_broadcasts_received_total` grows quickly.
- `ton_custom_overlay_block_broadcasts_applied_total` stays at zero while
  `ValidatorManager` is not started.
- `last_masterchain_block_ago` and `shard_client_ago` keep large lag despite
  live custom broadcasts arriving in milliseconds.

### Proposed Change

Use custom overlay membership for explicit block catch-up downloads:

- Prefer `block_senders_` as download peers.
- Fall back to all custom overlay `nodes_`.
- Exclude the local ADNL and zero ids.
- Try peers in order for `downloadBlockFull` and `downloadNextBlockFull`.
- Keep the public overlay fallback if custom peers cannot serve the block.

Also use the custom overlay first from `FullNodeShardImpl::try_get_next_block()`
so startup `downloadNextBlockFull` can use private overlay data before falling
back to the public shard overlay.

### Safety

The downloaded block still goes through the existing proof and hash checks in
`DownloadBlockNew`. A custom peer that returns empty data, invalid data, or times
out does not advance state and falls back to the next peer or to the public
overlay.

This is bounded by the configured custom overlay peer list. It does not create an
unbounded queue or cache.

### Test Plan

- Build `validator-engine`.
- Restart one private-overlay full node while it is behind.
- Verify custom `downloadNextBlockFull` requests reach explicit custom peers.
- Verify `last_masterchain_block_ago` catches up faster than the public-only
  path.
- Verify live broadcasts switch from `node_not_started` drops to normal
  validation/application after catch-up.

## 4. Make Initial Sync Delay Configurable For Fast Private Nodes

### Problem

`FullNodeOptions::initial_sync_delay_` defaults to 60 seconds. Even when block
catch-up is already complete, this adds an artificial restart delay before
`initial_read_complete()` lets `ValidatorManager` accept broadcasts and
liteserver queries.

### Proposed Change

For private-overlay deployments that are expected to restart and catch up
quickly, set `--initial-sync-delay 0` or make the deployment entrypoint expose an
environment variable for this flag.

The local test image changes the binary default to `0.0` so the current k8s
entrypoint, which does not pass the flag, can validate the behavior.

### Safety

This is an operational startup delay, not a consensus rule. Operators that still
want the old grace period can explicitly set `--initial-sync-delay 60`.

## Suggested PR Split

Submit as separate PRs:

1. `full-node: allow archive slice sync over custom overlays`
2. `validator: buffer shard-client masterchain notifications while busy`
3. `full-node: use custom overlay peers for block catch-up downloads`
4. `validator-engine: expose fast initial sync delay for private deployments`

They solve different bottlenecks and can be reviewed independently.
