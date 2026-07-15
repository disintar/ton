# dTON fork unique features

This file is a merge guard for local dTON functionality. It is not upstream TON
documentation. When merging fresh `ton-blockchain/ton` changes, preserve the
features below even if upstream moved, deleted, or rewrote nearby code.

Snapshot used for this document:

- Local branch before merge: `dev`, `HEAD=de69678d7` (`Improve overlay live sync diagnostics and dedupe`)
- Current merge worktree: `codex/merge-testnet-pr2417`
- Upstream testnet reference applied during merge: `mainnet/testnet=c686c88a7`
- Upstream PR reference applied during merge: `ton-blockchain/ton#2417=612f986af`
- Working tree includes live archive-resync changes for validator-manager,
  coroutine shard-client, fullnode rebase handling, and parallel custom/public
  next-block batch sync as of 2026-06-05.

## Merge rules

- Do not resolve conflicts in the files below by simply taking upstream.
- If upstream deletes a directory listed here, port the dTON feature first.
- If TL schemas change, re-apply the dTON constructors and regenerate dependent
  code before judging build failures.
- Keep CMake target wiring in sync: many features depend on each other
  (`lite-server-daemon`, `blockchain-indexer`, `validator-engine`, `tvm-python`,
  `cppkafka`, `librdkafka`, `PrometheusExporterActor`).
- `scripts/k8s_propagation_monitor.py` is a tracked diagnostic tool and must
  move together with the propagation trace/log format.

## Prometheus actor and analytics

The fork has its own actor-based Prometheus exporter, not just upstream metrics.
It serves `/metrics` and `/live`, aggregates validator-manager status, actor
stats, liteserver stats, liteserver credentials, custom overlay counters, and
fullnode ADNL metadata.

Critical files:

- `validator-engine/prometheus/PrometheusExporterActor.h`
- `validator-engine/prometheus/PrometheusExporterActor.cpp`
- `validator-engine/validator-engine.cpp`
- `validator-engine/validator-engine.hpp`
- `validator/manager.cpp`
- `validator/manager.hpp`
- `validator/interfaces/validator-manager.h`
- `lite-server-daemon/adnl-lite-proxy.cpp`
- `validator/custom-overlay-metrics.h`

Keep these entry points and environment variables:

- CLI: `--prometheus-exporter-port`
- Env: `TON_PROMETHEUS_SHARE_CREDENTIALS`
- Metrics: `ton_node_status_*`, actor stats blob, `ton_balancer_*`,
  `ton_liteserver_credentials`, `ton_fullnode_adnl`,
  `ton_custom_overlay_block_broadcasts_received_total`,
  `ton_custom_overlay_block_broadcasts_applied_total`,
  `ton_custom_overlay_duplicate_block_broadcasts_dropped_total`,
  `ton_custom_overlay_duplicate_block_candidates_dropped_total`,
  `ton_public_overlay_duplicate_block_broadcasts_dropped_total`,
  `ton_public_overlay_duplicate_block_candidates_dropped_total`,
  `ton_custom_overlay_sync_downloads_total`,
  `ton_custom_overlay_sync_download_latency_ms_*`,
  `ton_custom_overlay_sync_peer_downloads_total`,
  `ton_custom_overlay_sync_peer_latency_ms_*`,
  `ton_custom_overlay_sync_fallbacks_total`,
  `ton_public_overlay_sync_downloads_total`

Merge invariant: `ValidatorEngine::started_full_node_masters()` must create the
`PrometheusExporterActor` and pass it into `ValidatorManagerInterface`; lite
proxy must also create the same actor and push liteserver stats into it.

## Read-only DB mode

The fork supports running readers/indexers against a node DB without taking
write ownership. This is more than RocksDB read-only open; it is propagated
through validator DB actors and startup logic.

Critical files:

- `tddb/td/db/RocksDb.h`
- `tddb/td/db/RocksDb.cpp`
- `validator/fabric.h`
- `validator/impl/fabric.cpp`
- `validator/manager-disk.h`
- `validator/manager-disk.hpp`
- `validator/manager-disk.cpp`
- `validator/manager-init.h`
- `validator/manager-init.hpp`
- `validator/manager-init.cpp`
- `validator/db/rootdb.hpp`
- `validator/db/rootdb.cpp`
- `validator/db/celldb.hpp`
- `validator/db/celldb.cpp`
- `validator/db/statedb.hpp`
- `validator/db/statedb.cpp`
- `validator/db/archive-manager.*`
- `validator/db/archive-slice.*`
- `validator/db/archive-db.*`
- `validator/db/package.*`
- `validator/db/staticfilesdb.hpp`

Important behavior:

- `td::RocksDb::open(path, options, read_only)` uses
  `rocksdb::OptimisticTransactionDB::OpenForReadOnly` when requested.
- `ValidatorManagerDiskFactory::create(..., bool read_only)` must keep the
  boolean parameter.
- `RootDb` must pass `read_only_` into `CellDb`, `StateDb`, `StaticFilesDb`,
  and `ArchiveManager`.
- `ValidatorManagerMasterchainStarter` must wait for visible state/cells in
  read-only mode and return before GC/init write paths.
- Archive/package code must avoid fatal write/truncate behavior in read-only
  mode.

Known users of this mode:

- `blockchain-indexer/indexer.cpp` creates `ValidatorManagerDiskFactory` with
  `true`.
- `lite-server-daemon/lite-server-daemon.cpp` creates a read-only manager for
  standalone liteserver access to an existing DB.

Merge invariant: never remove the read-only parameter because "upstream does not
need it"; indexer and standalone liteserver depend on it.

## Indexer and Kafka publisher

The fork has both a standalone historical indexer and a live validator-engine
publisher.

Critical files:

- `blockchain-indexer/`
- `validator-engine/IBlockParser.*`
- `validator-engine/BlockParserAsync.*`
- `validator-engine/BlockPublisherKafka.*`
- `validator-engine/BlockRequestReceiverKafka.*`
- `validator-engine/ClusterSyncer.*`
- `validator-engine/validator-engine.cpp`
- `validator/manager.cpp`
- `validator/manager.hpp`
- `CMakeLists.txt`
- `validator-engine/CMakeLists.txt`
- `blockchain-indexer/CMakeLists.txt`
- `third-party/cppkafka`
- `third-party/librdkafka`

Standalone `blockchain-indexer` behavior:

- Reads from DB through read-only validator manager.
- Supports seqno ranges, seqno files, whitelist files, and simple
  `wc:shard:seqno` files.
- Dumps joined block/state JSON pairs, ids, loners, errors, and summary files.
- Important options include `-D`, `-C`, `-t`, `-c`, `-s`, `-f`, `-w`, `-p`,
  `-g`, `-S`.

Live publisher behavior:

- `validator-engine -P/--publish <kafka-endpoint>` attaches `BlockParser`.
- Publishes applied block, block data, state data, out messages, and errors.
- Kafka env overrides: `KAFKA_APPLY_TOPIC`, `KAFKA_BLOCK_TOPIC`,
  `KAFKA_STATE_TOPIC`, `KAFKA_OUTMSG_TOPIC`, `KAFKA_ERROR_TOPIC`.
- Startup replay envs: `KAFKA_STARTUP_REPLAY`,
  `STARTUP_BLOCKS_DOWNLOAD_BEFORE`, `STARTUP_BLOCKS_DOWNLOAD_AFTER`.
- Shard parsing gates: `PARSE_SHARDE_<partition>`.
- Cluster sync envs: `CLUSTER_SYNC_KEY`, `CLUSTER_SYNC_HOST`,
  `CLUSTER_SYNC_PORT`.
- `BlockRequestReceiverKafka` consumes `block-request`.

Merge invariant: preserve `Db::set_block_publisher`, manager calls that feed
block/state/apply events into `BlockParser`, and CMake links to `cppkafka`.

## Lite-server daemon, proxy, balancer, usage analytics

The fork adds standalone liteserver tooling and a dTON lite-proxy/balancer.
This is a core product feature.

Critical files:

- `lite-server-daemon/CMakeLists.txt`
- `lite-server-daemon/lite-server-daemon.cpp`
- `lite-server-daemon/lite-server-config.hpp`
- `lite-server-daemon/adnl-lite-proxy.cpp`
- `lite-server-daemon/README.md`
- `validator/lite-server-rate-limiter.h`
- `validator/lite-server-rate-limiter.cpp`
- `validator/manager-disk.*`
- `validator/impl/liteserver.cpp`
- `validator/impl/liteserver.hpp`
- `tl/generate/scheme/lite_api.tl`
- `tl/generate/scheme/ton_api.tl`

Important behavior:

- Builds `lite-server` and `lite-proxy` targets.
- `lite-server` can run against an existing DB in read-only mode and proxy
  external messages through fullnode slave config.
- `lite-proxy` exposes liteserver ADNL identities, tracks upstream private
  liteserver freshness, chooses up-to-date servers, supports lazy update mode,
  and can fire/refire requests to multiple liteservers.
- Balancer selection checks shard-client freshness as well as masterchain
  freshness. `TON_BALANCER_MAX_SHARD_LAG` controls the tolerated shard lag and
  Prometheus exports per-server shard status/lag.
- `lite-proxy` modes: `0` means random LS, `1` means fire to all and return the
  first success.
- `LiteServerLimiter` and proxy-side limiter persist users in `rate-limits/`
  RocksDB using `storage.liteserver.*` TL records.
- Admin liteserver queries include add user, get stats, and check item
  published.
- Usage analytics: `DTON_PUSH_USAGE` enables HTTP or HTTPS batch push from
  lite-proxy. Events include client/destination ADNL, query name, compiled
  request, raw request base64, request size, rps limit, start time, refire id,
  `duration_ms`, success/ratelimit status, and error text when present.
  Batches flush every five seconds in a detached HTTP/TLS thread; events wait
  for `duration_ms` when possible and are forced out after 30 seconds.
- Kafka lite-proxy topics include `lite-call-logs` and `lite-messages`.
- Proxy Prometheus includes `ton_balancer_*` and credential metrics.

Key proxy options:

- `-S/--server-config`
- `-D/--db`
- `-C/--config`
- `-I/--ip`
- `-L/--lite-port`
- `-A/--adnl-port`
- `-P/--publisher`
- `-m/--mode`
- `-t/--threads`
- Env: `DTON_PUSH_USAGE`, `TON_BALANCER_MAX_SHARD_LAG`

Merge invariant: do not drop the custom TL admin API, rate-limit DB layout,
usage push code, shard-aware balancer selection, or the read-only manager
creation in standalone liteserver.

## LiteServer protocol extensions

The fork extends `lite_api.tl` and `ton_api.tl`.

Important `lite_api.tl` additions:

- `int64`
- `liteServer.parsedBlockData`
- `liteServer.newUser`
- `liteServer.itemPublished`
- `liteServer.statItem`
- `liteServer.stats`
- `liteServer.addUser`
- `liteServer.getStatData`
- `liteServer.checkItemPublished`
- `liteServer.getParsedBlock`
- `liteServer.adminQuery`
- `liteServer.waitMasterchainSeqno`
- `liteServer.nonfinal.getPendingShardBlocks`

Important `ton_api.tl` additions:

- `adnl.proxyToFastHash`
- `adnl.proxyToFast`
- `adnl.proxy.none`
- `adnl.proxy.fast`
- `adnl.proxyPacketHeader`
- `adnl.proxyControlPacketPing/Pong/Register`
- `engine.addrProxy`
- `engine.liteserver.config`
- `engine.adnlProxy.*`
- `engine.validator.addProxy`
- `engine.validator.delProxy`
- `storage.liteserver.user`
- `storage.liteserver.users`

Merge invariant: if upstream edits these schemas, preserve dTON constructor
semantics and update all generated/compiled call sites together. Do not remove
custom liteserver admin types just because they are not in upstream.

## Python and TonPay-facing bindings

The fork has Python bindings used by downstream dTON/TonPay workflows. The
string `tonpay` is not present in code, but the custom bindings are here.

Critical files:

- `CMakeLists.txt`
- `CMake/BundleStaticLibraryDeps.cmake`
- `tvm-python/`
- `crypto/tl/tlbc-gen-py.cpp`
- `crypto/tl/tlbc-gen-py.h`
- `crypto/tl/tlbc.cpp`
- `crypto/tl/tlbc-data.h`
- `third-party/pybind11`
- `.github/workflows/build-universal*.yaml`
- `.github/workflows/build-windows-master.yaml`
- `assembly/native/build-universal-static.sh`

Important behavior:

- `TON_USE_PYTHON` enables `pybind11` and `tvm-python`.
- `python_ton` statically bundles TON dependencies through
  `BundleStaticLibraryDeps`.
- Exposes `PyCell`, `PyCellSlice`, `PyCellBuilder`, `PyDict`, `PyTVM`,
  `PyFift`, `PyFunc`, `PyEmulator`, `PyKeys`, `PyTools`, `PyLiteClient`.
- `PyLiteClient` exposes custom liteserver methods including
  `get_ParsedBlockInfo`, admin add/check/stat calls, `wait_masterchain_seqno`,
  `get_listBlockTransactionsExt`, account state, transactions, config, block,
  libraries, and shards.
- `codegen_python_tlb` exposes Python code generation from TLB text.

Merge invariant: keep `python_ton` import-compatible and keep CI artifact names
of the form `ton-cpython-<py>-<arch>-<os>`.

## ADNL proxy and fast proxy

The fork adds ADNL proxy support, including standalone `adnl-proxy`, proxy TL
types, validator-engine config/control API, and network-manager integration.

Critical files:

- `adnl/adnl-proxy.cpp`
- `adnl/adnl-proxy-types.h`
- `adnl/adnl-proxy-types.hpp`
- `adnl/adnl-proxy-types.cpp`
- `adnl/adnl-network-manager.*`
- `adnl/CMakeLists.txt`
- `validator-engine/validator-engine.cpp`
- `validator-engine/validator-engine.hpp`
- `validator-engine-console/validator-engine-console-query.*`
- `tl/generate/scheme/ton_api.tl`

Important behavior:

- `adnl-proxy` default config path:
  `/var/ton-work/etc/adnl-proxy.conf.json`
- Supports `adnl.proxy.none` and `adnl.proxy.fast`.
- `AdnlNetworkManager` can add proxy addresses and register proxy endpoints.
- Validator control supports `engine.validator.addProxy` and
  `engine.validator.delProxy`.
- Console command: `add-proxy-addr`.

Merge invariant: when upstream changes ADNL networking, port proxy wrapping,
packet validation, duplicate protection, and control packet handling.

## Fast sync through private/custom overlays

The fork extends custom overlays from broadcast-only propagation into a sync
transport for archive recovery, block catch-up, next-block catch-up, and proof
queries. Public overlay sync must remain a working fallback in every path.

Critical files:

- `validator/full-node-custom-overlays.*`
- `validator/full-node.*`
- `validator/full-node-shard.*`
- `validator/full-node-shard-queries.hpp`
- `validator/full-node-serializer.*`
- `validator/net/download-archive-slice.*`
- `validator/import-db-slice.*`
- `validator/custom-overlay-metrics.h`
- `validator/shard-client.*`
- `validator/manager.*`
- `validator/validator.h`
- `validator/interfaces/validator-manager.h`
- `validator-engine/prometheus/PrometheusExporterActor.cpp`
- `overlay/overlay.cpp`
- `scripts/k8s_propagation_monitor.py`
- `TO_MAINREPO.md`

Custom overlay query behavior:

- `FullNodeCustomOverlay::receive_query()` accepts only authorized custom
  overlay peers and rate-limits expensive archive/block queries.
- Private peers can answer `tonNode_getArchiveInfo`,
  `tonNode_getShardArchiveInfo`, `tonNode_getArchiveSlice`,
  `tonNode_downloadBlockFull`, `tonNode_downloadNextBlockFull`,
  `tonNode_downloadNextBlocksFull`, `tonNode_prepareBlockProof`,
  `tonNode_downloadBlockProof`, and `tonNode_downloadBlockProofLink`.
- `BlockFullSender` serves full block data plus proof/proof-link from the local
  DB and retries briefly while `next` is not yet initialized.
- `NextBlocksFullSender` serves up to 10 consecutive masterchain blocks in a
  single response, bounded by the same 8 MiB total-size cap used by PR #2417.
- Custom overlay archive requests use the overlay sender path for prepare and
  slice queries; the implementation still has optional peer pre-resolve support,
  but the current hot path skips DHT pre-resolve to avoid slow startup waits.

Archive and catch-up behavior:

- Archive recovery races custom overlay archive download and public overlay
  fallback. A bad, slow, or empty private peer must not make recovery slower than
  public sync.
- Public archive fallback can use custom overlay members as public peer hints,
  retries archive slices across neighbours, and logs selected peer/result.
- Archive recovery is not limited to startup. After initial sync completes,
  `ValidatorManagerImpl::alarm()` periodically checks whether masterchain or
  shard-client lag has grown again. If lag is above the live-resync threshold,
  or if shard-client is more than 16 MC seqnos behind, it re-enters the same
  archive importer path with custom/private sync first and public overlay as
  fallback.
- Block and single next-block downloads try explicit custom overlay peers, race
  bounded peer requests, record per-peer results, and fall back to public
  overlay on no-peer, timeout, not-ready, exhausted, or invalid data.
- Batched next-block catch-up uses the upstream PR #2417 `DownloadNextBlocks`
  actor for both transports. `FullNodeShardImpl::get_next_blocks_loop()` starts
  private custom-overlay batch sync and public-overlay batch sync concurrently;
  the first successful `BlockHandle` wins. Public overlay must not wait for a
  slow or broken private peer.
- `FullNodeCustomOverlay::download_next_blocks_from_custom_peers()` fans out the
  same batch request to all configured custom peers and returns the fastest
  successful peer. Exhaustion, timeout, and no-peer are logged and surfaced as
  not-ready errors to the custom side of the race only.
- Custom sync uses `CustomOverlaySyncKind::{block,next_block,archive}`,
  `CustomOverlaySyncSender::{rldp2,quic}`, and result labels
  `attempt/ok/error/no_peer/timeout/not_ready/shard_not_served/exhausted`.
- Downloaded data still goes through the existing proof/hash/import/validation
  path. The private path changes peer selection and timeout/fallback behavior,
  not trust rules.

Shard-client recovery behavior:

- `ShardClient` is based on the coroutine loop from upstream PR #2417. It waits
  for initialized next masterchain handles, applies all shard states in parallel,
  preprocesses upcoming shard states, and saves progress through
  `update_shard_client_state`.
- The dTON merge adds `latest_shards_` export, verbose `[shardclient-sync]`
  stage logs, block-propagation trace stages, stale-save detection after
  fullnode/archive rebase, and live `force_update_shard_client_ex()`.
- Live handoff can rebase the coroutine shard-client during the archive-to-live
  transition, and `ValidatorManagerImpl::out_of_sync()` keeps archive recovery
  close to live before switching to ordinary live sync.
- `ValidatorManagerImpl::out_of_sync()` must consider both masterchain and
  shard-client freshness. Startup archive sync must not finish only because the
  latest masterchain handle is fresh while the shard-client handle is still old.
- Live archive resync calls `ValidatorManagerInterface::Callback::
  archive_sync_complete()` so fullnode can rebase its current masterchain shard
  actor to the new top block without replaying the whole startup completion
  flow.
- `FullNodeShardImpl::set_handle()` supports post-start fast-forward rebase.
  Old or equal handles are ignored, newer handles reset the next-block attempt
  counter and restart `get_next_block()`.
- `FullNodeShardImpl::got_next_block()` must tolerate stale async replies after
  a rebase. Do not restore a hard `CHECK(next_seqno == old_seqno + 1)` without
  also proving old callbacks cannot arrive after live archive resync.
- `ShardClient::force_update_shard_client_ex()` supports live fast-forward when
  `started_` is true. It replaces the current MC handle/state, applies shards
  for the new top state, updates DB progress, and notifies the coroutine loop.
- Seqno comparisons are stabilized so stale/current callbacks after rebase do
  not overwrite newer progress and do not skip the next required masterchain
  block.

Diagnostics and log markers:

- `[archive-sync]` logs archive race start/done, random public peer selection,
  archive-info/slice chunk start/done, peer counts, transport, result, elapsed
  milliseconds, fallback decisions, live-resync start/done, shard-client rebase,
  and fullnode rebase.
- `[custom-overlay-sync]` logs custom block/next-block races, per-peer attempts,
  sender (`rldp2`/`quic`), local/peer ADNL, target block, result, and elapsed
  milliseconds. These logs are gated by `DTON_TRACE_BLOCK_PROPAGATION`.
- `[shardclient.*]` block-propagation stages show notification, wait-state,
  apply-all-shards, and save-to-db timing.
- Expected recovery-time broadcast failures (`broadcast is forbidden`, temporary
  peer ban, and `Non solvable`) are quieted so real transport/validation errors
  remain visible.
- `scripts/k8s_propagation_monitor.py` correlates `[block-propagation]` logs
  with `/metrics`, reports propagation delays, shard seqno spread, shard lag,
  and actor max delay for selected Kubernetes nodes.

Merge invariant: custom overlay sync must always have bounded waits and public
fallback; startup and live archive sync must require both masterchain and
shard-client to be fresh; live archive resync must be able to rebase shard-client
and fullnode without crashing on stale callbacks; batched next-block sync must
exist on both public and custom overlay paths; recovery log quieting must not
hide real bad signature, bad proof, or import validation errors.

## Block propagation tracing and custom overlay metrics

Recent local commits add env-gated block propagation tracing and custom overlay
block metrics.

Critical files:

- `validator/block-propagation-trace.h`
- `validator/types.h`
- `validator/full-node-custom-overlays.*`
- `validator/full-node.cpp`
- `validator/manager.cpp`
- `validator/validate-broadcast.*`
- `validator/apply-block.*`
- `validator/shard-client.cpp`
- `validator/custom-overlay-metrics.h`
- `validator-engine/prometheus/PrometheusExporterActor.cpp`
- `scripts/k8s_propagation_monitor.py`

Important behavior:

- Env: `DTON_TRACE_BLOCK_PROPAGATION`
- Env: `DTON_TRACE_BLOCK_PROPAGATION_SLOW_MS`
- `BlockBroadcast` carries `BlockPropagationTrace`.
- Trace stages include custom recv/deserialize, fullnode process/send custom,
  manager validate create, validate stages, apply stages, and shard-client
  waiting/saving stages.
- Log marker: `[block-propagation]`
- Metrics:
  `ton_custom_overlay_block_broadcasts_received_total`,
  `ton_custom_overlay_block_broadcasts_applied_total`,
  duplicate broadcast/candidate counters for custom and public overlays, and
  custom/public sync attempt, latency, peer, and fallback counters.
- Custom/public broadcast dedupe tracks non-final and final signature sets
  separately, so a later final custom block broadcast is not dropped only because
  an earlier non-final broadcast was seen.

Merge invariant: if upstream changes `BlockBroadcast`, broadcast serializers,
validation, or apply flow, keep the trace field and stage logging wired through
the whole path. If upstream changes fullnode broadcast dedupe, keep the
non-final/final split and the duplicate counters.

## Fullnode and liteserver rate limits

The fork has request-rate controls beyond upstream defaults.

Critical files:

- `validator/lite-server-rate-limiter.*`
- `validator/manager-disk.*`
- `validator/full-node.h`
- `validator/full-node.cpp`
- `validator-engine/validator-engine.cpp`
- `validator-engine/validator-engine.hpp`

Important CLI options:

- `--fullnode-ratelimit-window-size`
- `--fullnode-ratelimit-global`
- `--fullnode-ratelimit-heavy`
- `--fullnode-ratelimit-medium`

Merge invariant: preserve limiter actor integration in manager-disk and the
fullnode request-cost limits in `FullNodeOptions`.

## Out-of-sync threshold and live archive resync

The fork lowers the last masterchain block freshness threshold in
`ValidatorManagerImpl::out_of_sync()` from upstream's longer window to about 8
seconds so archive recovery stays close to live before handoff. In addition,
dTON treats an old shard-client handle as out-of-sync even if masterchain itself
is fresh.

Critical file:

- `validator/manager.cpp`
- `validator/manager.hpp`
- `validator/shard-client.cpp`
- `validator/full-node.cpp`
- `validator/full-node.hpp`
- `validator/full-node-shard.cpp`
- `validator/validator.h`

Important behavior:

- In `out_of_sync()`, preserve
  `last_masterchain_block_handle_->unix_time() + 8 > td::Clocks::system()`.
- Also preserve the shard-client freshness check:
  `shard_client_handle_->unix_time() + 8 <= td::Clocks::system()` means the
  node is still out of sync.
- After startup, `maybe_start_live_archive_resync()` must keep re-entering
  archive sync when masterchain or shard-client lag grows again. The current
  threshold is intentionally low enough to catch the archive-to-live bounce
  before it becomes minutes of lag.
- Live archive-resync completion must call `archive_sync_complete()` and must
  not call the normal startup `initial_read_complete()` path again.

Merge invariant: if upstream rewrites sync health checks, keep the dTON
8-second near-live handoff threshold and the shard-client freshness requirement
unless there is an explicit product decision to change them. Do not reintroduce
a post-archive state where masterchain is live but shard-client is allowed to
lag indefinitely.

## Deployment and build process

The fork has its own deployment/build flow. Upstream's many platform workflows
were intentionally replaced by dTON workflows.

Critical files:

- `.github/workflows/build-universal.yaml`
- `.github/workflows/build-universal-master.yaml`
- `.github/workflows/build-windows-master.yaml`
- `assembly/native/build-universal-static.sh`
- `assembly/native/build-windows-2022.bat`
- `assembly/native/build-windows-github-2022.bat`
- `Dockerfile`
- `docker/init.sh`
- `docker/control.template`
- `docker/README.md`
- `README.md`
- `CMakeLists.txt`
- `CMake/Build*.cmake`
- `CMake/Find*.cmake`
- `CMake/BundleStaticLibraryDeps.cmake`
- `third-party/cppkafka`
- `third-party/librdkafka`

Important behavior:

- GitHub Actions build static `python_ton` artifacts for Linux, macOS, and
  Windows across Python 3.10-3.13.
- `build-universal.yaml` targets `dev`; `build-universal-master.yaml` and
  `build-windows-master.yaml` target `master` releases.
- Linux/macOS workflows run `assembly/native/build-universal-static.sh`.
- Windows workflow builds and smoke-tests `import python_ton`.
- Docker image includes validator-engine, liteserver tools, proxy tools,
  emulator, fift/func, tolk, and dTON init logic.
- `docker/init.sh` downloads global config, initializes validator-engine,
  optionally restores dumps, configures control/liteserver certificates, and
  starts validator-engine with env-driven TTL/thread/verbosity/custom args.

Merge invariant: do not restore upstream workflow set by default. Keep dTON
artifact/release naming and the static Python build path.

## Legacy compatibility stack kept by fork

Compared with current upstream, this fork still carries and links legacy
`catchain` and old `validator-session` code in several targets. It is not safe
to delete during merge just because upstream removed it.

Critical directories:

- `catchain/`
- `validator-session/`

Targets that still depend on them include:

- `validator-engine`
- `blockchain-indexer`
- `lite-server-daemon`
- `tvm-python/python_ton`

Merge invariant: remove this stack only after all fork targets are migrated away
from those libraries.

## Quick post-merge checks

Useful grep check:

```bash
rtk rg -n "PrometheusExporterActor|DTON_PUSH_USAGE|TON_BALANCER_MAX_SHARD_LAG|DTON_TRACE_BLOCK_PROPAGATION|ton_custom_overlay_sync|custom-overlay-sync|archive-sync|live_archive_resync|archive_sync_complete|shardclient.rebase|fullnode.rebase|force_update_shard_client_ex|saved_to_db.*stale|latest_shards|ValidatorManagerDiskFactory::create\\(.*true|python_ton|liteServer.getParsedBlock|adnl.proxy|BlockPublisherKafka|LiteServerLimiter" .
```

Useful target check after a configured build exists:

```bash
rtk cmake --build build --target validator-engine lite-server lite-proxy blockchain-indexer python_ton
```

At minimum, confirm these files still exist after any upstream merge:

```bash
rtk test -f validator-engine/prometheus/PrometheusExporterActor.cpp
rtk test -f lite-server-daemon/adnl-lite-proxy.cpp
rtk test -f blockchain-indexer/indexer.cpp
rtk test -f tvm-python/python_ton.cpp
rtk test -f validator/block-propagation-trace.h
rtk test -f adnl/adnl-proxy.cpp
rtk test -f validator/custom-overlay-metrics.h
rtk test -f validator/full-node-custom-overlays.cpp
rtk test -f validator/net/download-archive-slice.cpp
rtk test -f scripts/k8s_propagation_monitor.py
```
