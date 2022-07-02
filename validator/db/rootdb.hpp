/*
    This file is part of TON Blockchain Library.

    TON Blockchain Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    TON Blockchain Library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with TON Blockchain Library.  If not, see <http://www.gnu.org/licenses/>.

    Copyright 2017-2020 Telegram Systems LLP
*/
#pragma once

#include "td/actor/actor.h"
#include "td/db/KeyValueAsync.h"
#include "ton/ton-types.h"

#include "celldb.hpp"
#include "statedb.hpp"
#include "staticfilesdb.hpp"
#include "archive-manager.hpp"
#include <zmq.hpp>
#include "validator/fabric.h"
#include "archiver.hpp"

#include "td/db/RocksDb.h"
#include "ton/ton-tl.hpp"
#include "td/utils/overloaded.h"
#include "common/checksum.h"
#include "validator/stats-merger.h"
#include "td/actor/MultiPromise.h"

#include "json.hpp"
#include "td/utils/logging.h"
#include "td/actor/actor.h"
#include "td/utils/Time.h"
#include "td/utils/filesystem.h"
#include "td/utils/JsonBuilder.h"
#include "auto/tl/ton_api_json.h"
#include "crypto/vm/cp0.h"
#include "ton/ton-types.h"
#include "ton/ton-tl.hpp"
#include "tl/tlblib.hpp"
#include "block/block.h"
#include "block/block-parse.h"
#include "block/block-auto.h"
#include "vm/dict.h"
#include "vm/cells/MerkleProof.h"
#include "vm/vm.h"
#include "td/utils/Slice.h"
#include "td/utils/common.h"
#include "crypto/block/transaction.h"
#include "td/utils/base64.h"
#include "td/utils/OptionParser.h"
#include "td/utils/port/user.h"
#include <utility>
#include "auto/tl/lite_api.h"
#include "adnl/utils.hpp"
#include "json.hpp"
#include "tuple"
#include "vm/boc.h"
#include "crypto/block/mc-config.h"


namespace ton {

namespace validator {

class BlockPublisher {
public:
  explicit BlockPublisher(const std::string& endpoint = "tcp://127.0.0.1:5001")
     : socket(ctx, zmq::socket_type::pub) {
    socket.bind(endpoint);
  }

  void storeBlockData(BlockHandle handle, td::Ref<BlockData> block);

  void storeBlockState(BlockHandle handle, td::Ref<ShardState> state) {
    std::lock_guard<std::mutex> lock(maps_mtx);
    const auto block_id = state->get_block_id();
    const std::string key = std::to_string(block_id.id.workchain) + ":" + std::to_string(block_id.id.shard) +
                            ":" + std::to_string(block_id.id.seqno);
    auto accounts = accounts_keys_.find(key);
    if (accounts == accounts_keys_.end()) {
      states_.insert({key, {handle, state}});
    } else {
      gotState(handle, state, accounts->second);
      accounts_keys_.erase(accounts);
    }
  }

private:
  void gotState(BlockHandle handle, td::Ref<ShardState> state, std::list<td::Bits256> accounts_keys);
  void publishBlockData(const std::string& json) {
    std::lock_guard<std::mutex> guard(net_mtx);
    socket.send(zmq::str_buffer("block/data"), zmq::send_flags::sndmore);
    socket.send(zmq::message_t(json.c_str(), json.size()));
  }

  void publishBlockState(const std::string& json) {
    std::lock_guard<std::mutex> guard(net_mtx);
    socket.send(zmq::str_buffer("block/state"), zmq::send_flags::sndmore);
    socket.send(zmq::message_t(json.c_str(), json.size()));
  }

private:
  zmq::context_t ctx;
  zmq::socket_t socket;
  std::mutex net_mtx;

  std::mutex maps_mtx;
  std::map<std::string, std::pair<BlockHandle, td::Ref<ShardState>>> states_;
  std::map<std::string, std::list<td::Bits256>> accounts_keys_;
};


class RootDb : public Db {
 public:
  enum class Flags : td::uint32 { f_started = 1, f_ready = 2, f_switched = 4, f_archived = 8 };
  RootDb(td::actor::ActorId<ValidatorManager> validator_manager, std::string root_path, bool read_only = false)
      : validator_manager_(validator_manager), root_path_(std::move(root_path)), read_only_(read_only){
  }

  void start_up() override;

  void store_block_data(BlockHandle handle, td::Ref<BlockData> block, td::Promise<td::Unit> promise) override;
  void get_block_data(ConstBlockHandle handle, td::Promise<td::Ref<BlockData>> promise) override;

  void store_block_signatures(BlockHandle handle, td::Ref<BlockSignatureSet> data,
                              td::Promise<td::Unit> promise) override;
  void get_block_signatures(ConstBlockHandle handle, td::Promise<td::Ref<BlockSignatureSet>> promise) override;

  void store_block_proof(BlockHandle handle, td::Ref<Proof> proof, td::Promise<td::Unit> promise) override;
  void get_block_proof(ConstBlockHandle handle, td::Promise<td::Ref<Proof>> promise) override;

  void store_block_proof_link(BlockHandle handle, td::Ref<ProofLink> proof, td::Promise<td::Unit> promise) override;
  void get_block_proof_link(ConstBlockHandle handle, td::Promise<td::Ref<ProofLink>> promise) override;

  void store_block_candidate(BlockCandidate candidate, td::Promise<td::Unit> promise) override;
  void get_block_candidate(PublicKey source, BlockIdExt id, FileHash collated_data_file_hash,
                           td::Promise<BlockCandidate> promise) override;

  void store_block_state(BlockHandle handle, td::Ref<ShardState> state,
                         td::Promise<td::Ref<ShardState>> promise) override;
  void get_block_state(ConstBlockHandle handle, td::Promise<td::Ref<ShardState>> promise) override;

  void store_block_handle(BlockHandle handle, td::Promise<td::Unit> promise) override;
  void get_block_handle(BlockIdExt id, td::Promise<BlockHandle> promise) override;
  void get_block_handle_external(BlockIdExt id, bool force, td::Promise<BlockHandle> promise) {
    td::actor::send_closure(validator_manager_, &ValidatorManager::get_block_handle, id, force, std::move(promise));
  }

  void store_persistent_state_file(BlockIdExt block_id, BlockIdExt masterchain_block_id, td::BufferSlice state,
                                   td::Promise<td::Unit> promise) override;
  void store_persistent_state_file_gen(BlockIdExt block_id, BlockIdExt masterchain_block_id,
                                       std::function<td::Status(td::FileFd&)> write_data,
                                       td::Promise<td::Unit> promise) override;
  void get_persistent_state_file(BlockIdExt block_id, BlockIdExt masterchain_block_id,
                                 td::Promise<td::BufferSlice> promise) override;
  void get_persistent_state_file_slice(BlockIdExt block_id, BlockIdExt masterchain_block_id, td::int64 offset,
                                       td::int64 max_length, td::Promise<td::BufferSlice> promise) override;
  void check_persistent_state_file_exists(BlockIdExt block_id, BlockIdExt masterchain_block_id,
                                          td::Promise<bool> promise) override;
  void store_zero_state_file(BlockIdExt block_id, td::BufferSlice state, td::Promise<td::Unit> promise) override;
  void get_zero_state_file(BlockIdExt block_id, td::Promise<td::BufferSlice> promise) override;
  void check_zero_state_file_exists(BlockIdExt block_id, td::Promise<bool> promise) override;

  void try_get_static_file(FileHash file_hash, td::Promise<td::BufferSlice> promise) override;

  void apply_block(BlockHandle handle, td::Promise<td::Unit> promise) override;
  void get_block_by_lt(AccountIdPrefixFull account, LogicalTime lt, td::Promise<ConstBlockHandle> promise) override;
  void get_block_by_unix_time(AccountIdPrefixFull account, UnixTime ts, td::Promise<ConstBlockHandle> promise) override;
  void get_block_by_seqno(AccountIdPrefixFull account, BlockSeqno seqno,
                          td::Promise<ConstBlockHandle> promise) override;

  void update_init_masterchain_block(BlockIdExt block, td::Promise<td::Unit> promise) override;
  void get_init_masterchain_block(td::Promise<BlockIdExt> promise) override;

  void update_gc_masterchain_block(BlockIdExt block, td::Promise<td::Unit> promise) override;
  void get_gc_masterchain_block(td::Promise<BlockIdExt> promise) override;

  void update_shard_client_state(BlockIdExt masterchain_block_id, td::Promise<td::Unit> promise) override;
  void get_shard_client_state(td::Promise<BlockIdExt> promise) override;

  void update_destroyed_validator_sessions(std::vector<ValidatorSessionId> sessions,
                                           td::Promise<td::Unit> promise) override;
  void get_destroyed_validator_sessions(td::Promise<std::vector<ValidatorSessionId>> promise) override;

  void update_async_serializer_state(AsyncSerializerState state, td::Promise<td::Unit> promise) override;
  void get_async_serializer_state(td::Promise<AsyncSerializerState> promise) override;

  void update_hardforks(std::vector<BlockIdExt> blocks, td::Promise<td::Unit> promise) override;
  void get_hardforks(td::Promise<std::vector<BlockIdExt>> promise) override;

  void archive(BlockHandle handle, td::Promise<td::Unit> promise) override;

  void allow_state_gc(BlockIdExt block_id, td::Promise<bool> promise);
  void allow_block_gc(BlockIdExt block_id, td::Promise<bool> promise);
  //void allow_gc(FileDb::RefId ref_id, bool is_archive, td::Promise<bool> promise);

  void prepare_stats(td::Promise<std::vector<std::pair<std::string, std::string>>> promise) override;

  void truncate(BlockSeqno seqno, ConstBlockHandle handle, td::Promise<td::Unit> promise) override;

  void add_key_block_proof(td::Ref<Proof> proof, td::Promise<td::Unit> promise) override;
  void add_key_block_proof_link(td::Ref<ProofLink> proof_link, td::Promise<td::Unit> promise) override;
  void get_key_block_proof(BlockIdExt block_id, td::Promise<td::Ref<Proof>> promise) override;
  void get_key_block_proof_link(BlockIdExt block_id, td::Promise<td::Ref<ProofLink>> promise) override;
  void check_key_block_proof_exists(BlockIdExt block_id, td::Promise<bool> promise) override;
  void check_key_block_proof_link_exists(BlockIdExt block_id, td::Promise<bool> promise) override;

  void get_archive_id(BlockSeqno masterchain_seqno, td::Promise<td::uint64> promise) override;
  void get_archive_slice(td::uint64 archive_id, td::uint64 offset, td::uint32 limit,
                         td::Promise<td::BufferSlice> promise) override;
  void set_async_mode(bool mode, td::Promise<td::Unit> promise) override;

  void run_gc(UnixTime ts, UnixTime archive_ttl) override;

 private:
  td::actor::ActorId<ValidatorManager> validator_manager_;

  std::string root_path_;
  bool read_only_;

  td::actor::ActorOwn<CellDb> cell_db_;
  td::actor::ActorOwn<StateDb> state_db_;
  td::actor::ActorOwn<StaticFilesDb> static_files_db_;
  td::actor::ActorOwn<ArchiveManager> archive_db_;

  BlockPublisher publisher_;
};

}  // namespace validator

}  // namespace ton
