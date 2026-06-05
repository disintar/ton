# dTON fork unique features

This file is a merge guard for local dTON functionality. It is not upstream TON
documentation. When merging fresh `ton-blockchain/ton` changes, preserve the
features below even if upstream moved, deleted, or rewrote nearby code.

Snapshot used for this document:

- Local branch: `dev`, `HEAD=ba75ac771` (`Add gated block propagation trace`)
- Upstream reference: `mainnet/master=8e6f09172`
- Merge base: `9f24a2644d97948392ff2280d4b4a433dcd51be8`
- Local branch is 2268 commits ahead of `mainnet/master` at the time of scan.

## Merge rules

- Do not resolve conflicts in the files below by simply taking upstream.
- If upstream deletes a directory listed here, port the dTON feature first.
- If TL schemas change, re-apply the dTON constructors and regenerate dependent
  code before judging build failures.
- Keep CMake target wiring in sync: many features depend on each other
  (`lite-server-daemon`, `blockchain-indexer`, `validator-engine`, `tvm-python`,
  `cppkafka`, `librdkafka`, `PrometheusExporterActor`).
- `scripts/` was untracked during this scan and is not described here.

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
  `ton_custom_overlay_block_broadcasts_applied_total`

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
- `lite-proxy` modes: `0` means random LS, `1` means fire to all and return the
  first success.
- `LiteServerLimiter` and proxy-side limiter persist users in `rate-limits/`
  RocksDB using `storage.liteserver.*` TL records.
- Admin liteserver queries include add user, get stats, and check item
  published.
- Usage analytics: `DTON_PUSH_USAGE` enables HTTP or HTTPS batch push from
  lite-proxy. Events include query name, request, rps limit, start time, and
  `duration_ms` after request completion.
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

Merge invariant: do not drop the custom TL admin API, rate-limit DB layout,
usage push code, or the read-only manager creation in standalone liteserver.

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
- `liteServer.nonfinal.getValidatorGroups`
- `liteServer.nonfinal.getCandidate`

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
  `ton_custom_overlay_block_broadcasts_applied_total`

Merge invariant: if upstream changes `BlockBroadcast`, broadcast serializers,
validation, or apply flow, keep the trace field and stage logging wired through
the whole path.

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

## Out-of-sync threshold

The fork lowers the last masterchain block freshness threshold in
`ValidatorManagerImpl::out_of_sync()` from upstream's longer window to 80
seconds.

Critical file:

- `validator/manager.cpp`

Important behavior:

- In `out_of_sync()`, preserve
  `last_masterchain_block_handle_->unix_time() + 80 > td::Clocks::system()`.

Merge invariant: if upstream rewrites sync health checks, keep the dTON 80
second threshold unless there is an explicit product decision to change it.

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
rtk rg -n "PrometheusExporterActor|DTON_PUSH_USAGE|DTON_TRACE_BLOCK_PROPAGATION|ValidatorManagerDiskFactory::create\\(.*true|python_ton|liteServer.getParsedBlock|adnl.proxy|BlockPublisherKafka|LiteServerLimiter" .
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
```
