# Upstream PR Notes

This document describes two independent improvements that should be prepared as
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

Submit as two separate PRs:

1. `full-node: allow archive slice sync over custom overlays`
2. `validator: buffer shard-client masterchain notifications while busy`

They solve different bottlenecks and can be reviewed independently.
