//
// Created by tvorogme on 5/21/22.
//

#include "td/utils/logging.h"
#include "td/actor/actor.h"
#include "td/utils/Time.h"
#include "td/utils/filesystem.h"
#include "td/utils/JsonBuilder.h"
#include "auto/tl/ton_api_json.h"
#include "crypto/vm/cp0.h"
#include "validator/validator.h"
#include "validator/manager-disk.h"
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
#include "td/utils/OptionParser.h"
#include "td/utils/port/user.h"
#include <utility>
#include <fstream>
#include "auto/tl/lite_api.h"
#include "adnl/utils.hpp"
#include "json.hpp"
#include "json-utils.hpp"
#include "tuple"
#include "crypto/block/mc-config.h"
#include <algorithm>
#include <queue>
#include <chrono>
#include <thread>
#include <csignal>
#include "common/delay.h"

int verbosity = 0;

namespace ton {

namespace validator {

class Dumper {
 public:
  explicit Dumper(std::string prefix, const std::size_t buffer_size)
      : prefix(std::move(prefix)), buffer_size(buffer_size) {
    joined.reserve(buffer_size);
  }

  ~Dumper() {
    forceDump();
  }

  void storeBlock(std::string id, json block) {
    std::lock_guard lock(store_mtx);

    LOG(DEBUG) << "Storing block " << id;

    auto state = states.find(id);
    if (state == states.end()) {
      blocks.insert({std::move(id), std::move(block)});
    } else {
      json together = {{"id", id}, {"block", std::move(block)}, {"state", std::move(state->second)}};
      joined.emplace_back(std::move(together));
      states.erase(state);
    }

    if (joined.size() >= buffer_size) {
      dump();
    }
  }

  void storeState(std::string id, json state) {
    std::lock_guard lock(store_mtx);

    LOG(DEBUG) << "Storing state " << id;

    auto block = blocks.find(id);
    if (block == blocks.end()) {
      states.insert({std::move(id), std::move(state)});
    } else {
      json together = {{"id", id}, {"block", std::move(block->second)}, {"state", std::move(state)}};
      joined.emplace_back(std::move(together));
      blocks.erase(block);
    }

    if (joined.size() >= buffer_size) {
      dump();
    }
  }

  void forceDump() {
    LOG(INFO) << "Force dump of what is left";
    dump();
    dumpLoners();
    LOG(INFO) << "Finished force dumping";
  }

 public:
  void dump() {
    std::lock_guard lock(dump_mtx);

    const auto dumped_amount = joined.size();

    auto to_dump = json::array();
    for (auto &e : joined) {
      to_dump.emplace_back(std::move(e));
    }
    joined.clear();

    std::ostringstream oss;
    oss << prefix
        << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
               .count()
        << ".json";
    std::ofstream file(oss.str());
    file << to_dump.dump(4);

    LOG(INFO) << "Dumped " << dumped_amount << " block/state pairs";
  }

  void dumpLoners() {
    std::lock_guard lock(dump_mtx);

    const auto lone_blocks_amount = blocks.size();
    const auto lone_states_amount = states.size();

    auto blocks_to_dump = json::array();
    for (auto &e : blocks) {
      json block_json = {{"id", e.first}, {"block", std::move(e.second)}};
      blocks_to_dump.emplace_back(std::move(block_json));
    }
    blocks.clear();

    auto states_to_dump = json::array();
    for (auto &e : states) {
      json state_json = {{"id", e.first}, {"state", std::move(e.second)}};
      states_to_dump.emplace_back(std::move(state_json));
    }
    states.clear();

    json to_dump = {{"blocks", std::move(blocks_to_dump)}, {"states", std::move(states_to_dump)}};

    if (lone_blocks_amount != 0 || lone_states_amount != 0) {
      std::ostringstream oss;
      oss << prefix << "loners_"
          << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
                 .count()
          << ".json";
      std::ofstream file(oss.str());
      file << to_dump.dump(4);

      LOG(WARNING) << "Dumped " << lone_blocks_amount << " blocks without pair";
      LOG(WARNING) << "Dumped " << lone_states_amount << " states without pair";
    }
  }

 private:
  const std::string prefix;
  std::mutex store_mtx;
  std::mutex dump_mtx;
  std::unordered_map<std::string, json> blocks;
  std::unordered_map<std::string, json> states;
  std::vector<json> joined;
  const std::size_t buffer_size;
};

class Indexer : public td::actor::Actor {
 private:
  std::string db_root_ = "/mnt/ton/ton-node/db";
  std::string global_config_ /*; = db_root_ + "/global-config.json"*/;
  BlockSeqno seqno_first_ = 0;
  BlockSeqno seqno_last_ = 0;
  int block_padding_ = 0;
  int state_padding_ = 0;
  td::uint32 chunk_size_ = 20000;
  td::uint32 chunk_count_ = 0;
  td::uint32 chunk_current_ = 0;
  std::mutex display_mtx_;
  Dumper dumper_ = Dumper("dump_", 20000);

  // store timestamps of parsed blocks for speed measuring
  std::queue<std::chrono::time_point<std::chrono::high_resolution_clock>> parsed_blocks_timepoints_;
  std::queue<std::chrono::time_point<std::chrono::high_resolution_clock>> parsed_states_timepoints_;
  td::Ref<ton::validator::ValidatorManagerOptions> opts_;
  td::actor::ActorOwn<ton::validator::ValidatorManagerInterface> validator_manager_;
  bool display_speed_ = false;
  td::Status create_validator_options() {
    TRY_RESULT_PREFIX(conf_data, td::read_file(global_config_), "failed to read: ");
    TRY_RESULT_PREFIX(conf_json, td::json_decode(conf_data.as_slice()), "failed to parse json: ");

    ton::ton_api::config_global conf;
    TRY_STATUS_PREFIX(ton::ton_api::from_json(conf, conf_json.get_object()), "json does not fit TL scheme: ");

    auto zero_state = ton::create_block_id(conf.validator_->zero_state_);
    ton::BlockIdExt init_block;
    if (!conf.validator_->init_block_) {
      LOG(INFO) << "no init block readOnlyin config. using zero state";
      init_block = zero_state;
    } else {
      init_block = ton::create_block_id(conf.validator_->init_block_);
    }

    std::function<bool(ton::ShardIdFull, ton::CatchainSeqno, ton::validator::ValidatorManagerOptions::ShardCheckMode)>
        check_shard = [](ton::ShardIdFull, ton::CatchainSeqno,
                         ton::validator::ValidatorManagerOptions::ShardCheckMode) { return true; };
    bool allow_blockchain_init = false;
    double sync_blocks_before = 86400;
    double block_ttl = 86400 * 7;
    double state_ttl = 3600;
    double archive_ttl = 86400 * 365;
    double key_proof_ttl = 86400 * 3650;
    double max_mempool_num = 999999;
    bool initial_sync_disabled = true;

    opts_ = ton::validator::ValidatorManagerOptions::create(zero_state, init_block, check_shard, allow_blockchain_init,
                                                            sync_blocks_before, block_ttl, state_ttl, archive_ttl,
                                                            key_proof_ttl, max_mempool_num, initial_sync_disabled);

    std::vector<ton::BlockIdExt> h;
    h.reserve(conf.validator_->hardforks_.size());
    for (auto &x : conf.validator_->hardforks_) {
      auto b = ton::create_block_id(x);
      if (!b.is_masterchain()) {
        return td::Status::Error(ton::ErrorCode::error,
                                 "[validator/hardforks] section contains not masterchain block id");
      }
      if (!b.is_valid_full()) {
        return td::Status::Error(ton::ErrorCode::error, "[validator/hardforks] section contains invalid block_id");
      }
      for (auto &y : h) {
        if (y.is_valid() && y.seqno() >= b.seqno()) {
          y.invalidate();
        }
      }
      h.emplace_back(std::move(b));
    }
    opts_.write().set_hardforks(std::move(h));
    return td::Status::OK();
  }
  std::unordered_set<std::string> already_traversed_;
  std::mutex parsed_shards_mtx_;

  std::mutex stored_counter_mtx_;

 public:
  std::size_t stored_blocks_counter_ = 0;
  std::size_t stored_states_counter_ = 0;

 public:
  void set_db_root(std::string db_root) {
    db_root_ = std::move(db_root);
  }
  void set_global_config_path(std::string path) {
    global_config_ = std::move(path);
  }
  void set_seqno_range(BlockSeqno seqno_first, BlockSeqno seqno_last) {
    seqno_first_ = seqno_first;
    seqno_last_ = seqno_last;
  }
  void set_chunk_size(td::uint32 size) {
    chunk_size_ = size;
  }
  void set_display_speed(bool display_speed) {
    display_speed_ = display_speed;
  }

  void run() {
    LOG(DEBUG) << "Use db root: " << db_root_;

    auto Sr = create_validator_options();
    if (Sr.is_error()) {
      LOG(ERROR) << "failed to load global config'" << global_config_ << "': " << Sr;
      std::_Exit(2);
    } else {
      LOG(DEBUG) << "Global config loaded successfully from " << global_config_;
    }

    auto shard = ton::ShardIdFull(ton::masterchainId, ton::shardIdAll);
    auto shard_top =
        ton::BlockIdExt{ton::masterchainId, ton::shardIdAll, 0, ton::RootHash::zero(), ton::FileHash::zero()};

    auto id = PublicKeyHash::zero();

    validator_manager_ =
        ton::validator::ValidatorManagerDiskFactory::create(id, opts_, shard, shard_top, db_root_, true);

    class Callback : public ValidatorManagerInterface::Callback {
     public:
      void initial_read_complete(BlockHandle handle) override {
        LOG(DEBUG) << "Initial read complete";
        td::actor::send_closure(id_, &Indexer::sync_complete, handle);
      }
      void add_shard(ShardIdFull shard) override {
        LOG(DEBUG) << "add_shard";
        //        td::actor::send_closure(id_, &FullNodeImpl::add_shard, shard);
      }
      void del_shard(ShardIdFull shard) override {
        LOG(DEBUG) << "del_shard";
        //        td::actor::send_closure(id_, &FullNodeImpl::del_shard, shard);
      }
      void send_ihr_message(AccountIdPrefixFull dst, td::BufferSlice data) override {
        LOG(DEBUG) << "send_ihr_message";
        //        td::actor::send_closure(id_, &FullNodeImpl::send_ihr_message, dst, std::move(data));
      }
      void send_ext_message(AccountIdPrefixFull dst, td::BufferSlice data) override {
        LOG(DEBUG) << "send_ext_message";
        //        td::actor::send_closure(id_, &FullNodeImpl::send_ext_message, dst, std::move(data));
      }
      void send_shard_block_info(BlockIdExt block_id, CatchainSeqno cc_seqno, td::BufferSlice data) override {
        LOG(DEBUG) << "send_shard_block_info";
        //        td::actor::send_closure(id_, &FullNodeImpl::send_shard_block_info, block_id, cc_seqno, std::move(data));
      }
      void send_broadcast(BlockBroadcast broadcast) override {
        LOG(DEBUG) << "send_broadcast";
        //        td::actor::send_closure(id_, &FullNodeImpl::send_broadcast, std::move(broadcast));
      }
      void download_block(BlockIdExt id, td::uint32 priority, td::Timestamp timeout,
                          td::Promise<ReceivedBlock> promise) override {
        LOG(DEBUG) << "download_block";
        //        td::actor::send_closure(id_, &FullNodeImpl::download_block, id, priority, timeout, std::move(promise));
      }
      void download_zero_state(BlockIdExt id, td::uint32 priority, td::Timestamp timeout,
                               td::Promise<td::BufferSlice> promise) override {
        LOG(DEBUG) << "download_zero_state";
        //        td::actor::send_closure(id_, &FullNodeImpl::download_zero_state, id, priority, timeout, std::move(promise));
      }
      void download_persistent_state(BlockIdExt id, BlockIdExt masterchain_block_id, td::uint32 priority,
                                     td::Timestamp timeout, td::Promise<td::BufferSlice> promise) override {
        LOG(DEBUG) << "download_persistent_state";
        //        td::actor::send_closure(id_, &FullNodeImpl::download_persistent_state, id, masterchain_block_id, priority,
        //                                timeout, std::move(promise));
      }
      void download_block_proof(BlockIdExt block_id, td::uint32 priority, td::Timestamp timeout,
                                td::Promise<td::BufferSlice> promise) override {
        LOG(DEBUG) << "download_block_proof";
        //        td::actor::send_closure(id_, &FullNodeImpl::download_block_proof, block_id, priority, timeout,
        //                                std::move(promise));
      }
      void download_block_proof_link(BlockIdExt block_id, td::uint32 priority, td::Timestamp timeout,
                                     td::Promise<td::BufferSlice> promise) override {
        LOG(DEBUG) << "download_block_proof_link";
        //        td::actor::send_closure(id_, &FullNodeImpl::download_block_proof_link, block_id, priority, timeout,
        //                                std::move(promise));
      }
      void get_next_key_blocks(BlockIdExt block_id, td::Timestamp timeout,
                               td::Promise<std::vector<BlockIdExt>> promise) override {
        LOG(DEBUG) << "get_next_key_blocks";
        //        td::actor::send_closure(id_, &FullNodeImpl::get_next_key_blocks, block_id, timeout, std::move(promise));
      }
      void download_archive(BlockSeqno masterchain_seqno, std::string tmp_dir, td::Timestamp timeout,
                            td::Promise<std::string> promise) override {
        LOG(DEBUG) << "download_archive";
        //        td::actor::send_closure(id_, &FullNodeImpl::download_archive, masterchain_seqno, std::move(tmp_dir), timeout,
        //                                std::move(promise));
      }

      void new_key_block(BlockHandle handle) override {
        LOG(DEBUG) << "new_key_block";
        //        td::actor::send_closure(id_, &FullNodeImpl::new_key_block, std::move(handle));
      }

      Callback(td::actor::ActorId<Indexer> id) : id_(id) {
      }

     private:
      td::actor::ActorId<Indexer> id_;
    };

    auto P_cb = td::PromiseCreator::lambda([](td::Unit R) {});
    td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::install_callback,
                            std::make_unique<Callback>(actor_id(this)), std::move(P_cb));
    LOG(DEBUG) << "Callback installed";

    if (display_speed_) {
      std::unique_lock<std::mutex> lock(display_mtx_);
      display_speed();
    }
  }

  void sync_complete(const BlockHandle &handle) {
    // separate first parse seqno to prevent WC shard seqno leak
    auto P = td::PromiseCreator::lambda(
        [this, SelfId = actor_id(this), seqno_first = seqno_first_](td::Result<ConstBlockHandle> R) {
          if (R.is_error()) {
            td::actor::send_closure(SelfId, &Indexer::decrease_block_padding);
            LOG(ERROR) << R.move_as_error().to_string();
          } else {
            auto handle = R.move_as_ok();
            LOG(DEBUG) << "got data for block " << handle->id().to_str();
            td::actor::send_closure(SelfId, &Indexer::got_block_handle, handle, handle->id().seqno() == seqno_first);
          }
        });

    increase_block_padding();
    ton::AccountIdPrefixFull pfx{-1, 0x8000000000000000};
    td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::get_block_by_seqno_from_db, pfx,
                            seqno_first_, std::move(P));
  }

  void parse_other() {
    if (seqno_last_ > seqno_first_) {
      if (chunk_count_ == 0) {
        auto blocks_size = seqno_last_ - seqno_first_ + 1;
        chunk_count_ = (unsigned int)ceil(blocks_size / chunk_size_);
        LOG(WARNING) << "Total chunks count: " << chunk_count_;
      }

      chunk_current_ += 1;
      auto start = seqno_first_ + 1 + (chunk_size_ * (chunk_current_ - 1));
      auto end = td::min(seqno_last_, seqno_first_ + 1 + (chunk_size_ * chunk_current_));
      LOG(WARNING) << "Process chunk (" << chunk_current_ << ") From: " << start << " To: " << end;

      for (auto seqno = start; seqno <= end; ++seqno) {
        auto P = td::PromiseCreator::lambda(
            [this, SelfId = actor_id(this), seqno_first = seqno_first_](td::Result<ConstBlockHandle> R) {
              if (R.is_error()) {
                LOG(ERROR) << R.move_as_error().to_string();
                td::actor::send_closure(SelfId, &Indexer::decrease_block_padding);
              } else {
                auto handle = R.move_as_ok();
                LOG(DEBUG) << "got block from db " << handle->id().to_str();
                td::actor::send_closure_later(SelfId, &Indexer::got_block_handle, handle, false);
              }
            });

        increase_block_padding();
        ton::AccountIdPrefixFull pfx{-1, 0x8000000000000000};
        td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::get_block_by_seqno_from_db, pfx, seqno,
                                std::move(P));
      }
    }
  }

  void start_parse_shards(BlockSeqno seqno, ShardId shard, WorkchainId workchain, bool is_first = false) {
    auto P = td::PromiseCreator::lambda([this, workchain_shard = workchain, seqno_shard = seqno, shard_shard = shard,
                                         SelfId = actor_id(this), first = is_first](td::Result<ConstBlockHandle> R) {
      if (R.is_error()) {
        LOG(ERROR) << "ERROR IN BLOCK: "
                   << "Seqno: " << seqno_shard << " Shard: " << shard_shard << " Worckchain: " << workchain_shard;

        LOG(ERROR) << R.move_as_error().to_string();
        td::actor::send_closure(SelfId, &Indexer::decrease_state_padding);
        return;
      } else {
        auto handle = R.move_as_ok();
        LOG(DEBUG) << "got block from db " << workchain_shard << ":" << shard_shard << ":" << seqno_shard
                   << " is_first: " << first;
        td::actor::send_closure(SelfId, &Indexer::got_block_handle, handle, first);
      }
    });

    increase_block_padding();
    ton::AccountIdPrefixFull pfx{workchain, shard};
    td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::get_block_by_seqno_from_db, pfx, seqno,
                            std::move(P));
  }

  void got_block_handle(std::shared_ptr<const BlockHandleInterface> handle, bool first = false) {
    {
      std::lock_guard<std::mutex> lock(parsed_shards_mtx_);
      const auto block_id = handle->id().id;
      const auto id = std::to_string(block_id.workchain) + ":" + std::to_string(block_id.shard) + ":" +
                      std::to_string(block_id.seqno);
      if (already_traversed_.find(id) != already_traversed_.end()) {
        LOG(DEBUG) << id << " <- already traversed!";
        decrease_block_padding();
        return;
      }
      already_traversed_.emplace(id);
    }

    auto P = td::PromiseCreator::lambda([this, SelfId = actor_id(this), is_first = first,
                                         block_handle = handle](td::Result<td::Ref<BlockData>> R) {
      if (R.is_error()) {
        LOG(ERROR) << R.move_as_error().to_string();
      } else {
        auto block = R.move_as_ok();
        CHECK(block.not_null());

        auto blkid = block->block_id();

        LOG(INFO) << "Parse block: " << blkid.to_str() << " is_first: " << is_first;

        auto block_root = block->root_cell();
        if (block_root.is_null()) {
          LOG(ERROR) << "block has no valid root cell";
          return;
        }

        //
        // Parsing

        json answer;
        answer["type"] = "block_data";

        auto workchain = blkid.id.workchain;

        answer["BlockIdExt"] = {{"file_hash", blkid.file_hash.to_hex()},
                                {"root_hash", blkid.root_hash.to_hex()},
                                {"id",
                                 {
                                     {"workchain", workchain},
                                     {"seqno", blkid.id.seqno},
                                     {"shard", blkid.id.shard},
                                 }}};

        block::gen::Block::Record blk;
        block::gen::BlockInfo::Record info;
        block::gen::BlockExtra::Record extra;

        if (!(tlb::unpack_cell(block_root, blk) && tlb::unpack_cell(blk.extra, extra) &&
              tlb::unpack_cell(blk.info, info))) {
          LOG(FATAL) << "Error in block: " << blkid.to_str();
          return;
        }

        /* tlb
          block#11ef55aa global_id:int32
          info:^BlockInfo value_flow:^ValueFlow
          state_update:^(MERKLE_UPDATE ShardState)
          extra:^BlockExtra = Block;
        */

        answer["global_id"] = blk.global_id;
        auto now = info.gen_utime;
        auto start_lt = info.start_lt;

        /* tlb
          block_info#9bc7a987 version:uint32
              not_master:(## 1)
              after_merge:(## 1) before_split:(## 1)
              after_split:(## 1)
              want_split:Bool want_merge:Bool
              key_block:Bool vert_seqno_incr:(## 1)
              flags:(## 8) { flags <= 1 }
              seq_no:# vert_seq_no:# { vert_seq_no >= vert_seqno_incr }
              { prev_seq_no:# } { ~prev_seq_no + 1 = seq_no }
              shard:ShardIdent gen_utime:uint32
              start_lt:uint64 end_lt:uint64
              gen_validator_list_hash_short:uint32
              gen_catchain_seqno:uint32
              min_ref_mc_seqno:uint32
              prev_key_block_seqno:uint32
              gen_software:flags . 0?GlobalVersion
              master_ref:not_master?^BlkMasterInfo
              prev_ref:^(BlkPrevInfo after_merge)
              prev_vert_ref:vert_seqno_incr?^(BlkPrevInfo 0)
              = BlockInfo;
        */
        answer["BlockInfo"] = {
            {"version", info.version},
            {"not_master", info.not_master},
            {"after_merge", info.after_merge},
            {"before_split", info.before_split},
            {"after_split", info.after_split},
            {"want_split", info.want_split},
            {"want_merge", info.want_merge},
            {"key_block", info.key_block},
            {"vert_seqno_incr", info.vert_seqno_incr},
            {"flags", info.flags},
            {"seq_no", info.seq_no},
            {"vert_seq_no", info.vert_seq_no},
            {"gen_utime", now},
            {"start_lt", start_lt},
            {"end_lt", info.end_lt},
            {"gen_validator_list_hash_short", info.gen_validator_list_hash_short},
            {"gen_catchain_seqno", info.gen_catchain_seqno},
            {"min_ref_mc_seqno", info.min_ref_mc_seqno},
            {"prev_key_block_seqno", info.prev_key_block_seqno},
        };

        if (info.vert_seqno_incr) {
          block::gen::ExtBlkRef::Record prev_vert_blk{};
          CHECK(tlb::unpack_cell(info.prev_vert_ref, prev_vert_blk));

          answer["BlockInfo"]["prev_vert_ref"] = {
              {"end_lt", prev_vert_blk.end_lt},
              {"seq_no", prev_vert_blk.seq_no},
              {"root_hash", prev_vert_blk.root_hash.to_hex()},
              {"file_hash", prev_vert_blk.file_hash.to_hex()},
          };
        }

        if (info.after_merge) {
          block::gen::ExtBlkRef::Record prev_blk_1{};
          block::gen::ExtBlkRef::Record prev_blk_2{};

          auto c_ref = load_cell_slice(info.prev_ref);
          auto blk1 = c_ref.fetch_ref();
          auto blk2 = c_ref.fetch_ref();

          CHECK(tlb::unpack_cell(blk1, prev_blk_1));
          CHECK(tlb::unpack_cell(blk2, prev_blk_2));

          answer["BlockInfo"]["prev_ref"] = {
              {"type", "1"},
              {"data",
               {
                   {"end_lt", prev_blk_1.end_lt},
                   {"seq_no", prev_blk_1.seq_no},
                   {"root_hash", prev_blk_1.root_hash.to_hex()},
                   {"file_hash", prev_blk_1.file_hash.to_hex()},
               }},
              {"data_2",
               {
                   {"end_lt", prev_blk_2.end_lt},
                   {"seq_no", prev_blk_2.seq_no},
                   {"root_hash", prev_blk_2.root_hash.to_hex()},
                   {"file_hash", prev_blk_2.file_hash.to_hex()},
               }},
          };

          if (info.not_master && !is_first) {
            LOG(DEBUG) << "FOR: " << blkid.to_str() << " first: " << is_first;
            LOG(DEBUG) << "GO: " << blkid.id.workchain << ":" << blkid.id.shard << ":" << prev_blk_1.seq_no;
            LOG(DEBUG) << "GO: " << blkid.id.workchain << ":" << blkid.id.shard << ":" << prev_blk_2.seq_no;

            td::actor::send_closure_later(SelfId, &Indexer::start_parse_shards, prev_blk_1.seq_no, blkid.id.shard,
                                          blkid.id.workchain, false);

            td::actor::send_closure_later(SelfId, &Indexer::start_parse_shards, prev_blk_2.seq_no, blkid.id.shard,
                                          blkid.id.workchain, false);
          }

        } else {
          block::gen::ExtBlkRef::Record prev_blk{};
          CHECK(tlb::unpack_cell(info.prev_ref, prev_blk));

          answer["BlockInfo"]["prev_ref"] = {{"type", "0"},
                                             {"data",
                                              {
                                                  {"end_lt", prev_blk.end_lt},
                                                  {"seq_no", prev_blk.seq_no},
                                                  {"root_hash", prev_blk.root_hash.to_hex()},
                                                  {"file_hash", prev_blk.file_hash.to_hex()},
                                              }}};

          if (info.not_master && !is_first) {
            LOG(DEBUG) << "FOR: " << blkid.to_str();
            LOG(DEBUG) << "GO: " << blkid.id.workchain << ":" << blkid.id.shard << ":" << prev_blk.seq_no;

            td::actor::send_closure(SelfId, &Indexer::start_parse_shards, prev_blk.seq_no, blkid.id.shard,
                                    blkid.id.workchain, false);
          }
        }

        if (info.master_ref.not_null()) {
          block::gen::ExtBlkRef::Record master{};
          auto csr = load_cell_slice(info.master_ref);
          CHECK(tlb::unpack(csr, master));

          answer["BlockInfo"]["master_ref"] = {
              {"end_lt", master.end_lt},
              {"seq_no", master.seq_no},
              {"root_hash", master.root_hash.to_hex()},
              {"file_hash", master.file_hash.to_hex()},
          };
        }

        if (info.gen_software.not_null()) {
          answer["BlockInfo"]["gen_software"] = {
              {"version", info.gen_software->prefetch_ulong(32)},
              {"capabilities", info.gen_software->prefetch_ulong(64)},
          };
        }

        auto value_flow_root = blk.value_flow;
        block::ValueFlow value_flow;
        vm::CellSlice cs{vm::NoVmOrd(), value_flow_root};
        if (!(cs.is_valid() && value_flow.fetch(cs) && cs.empty_ext())) {
          LOG(ERROR) << "cannot unpack ValueFlow of the new block ";
          return;
        }

        /* tlb
          value_flow ^[ from_prev_blk:CurrencyCollection
                        to_next_blk:CurrencyCollection
                        imported:CurrencyCollection
                        exported:CurrencyCollection ]
                        fees_collected:CurrencyCollection
                        ^[
                        fees_imported:CurrencyCollection
                        recovered:CurrencyCollection
                        created:CurrencyCollection
                        minted:CurrencyCollection
                        ] = ValueFlow;
        */

        answer["ValueFlow"] = {};

        answer["ValueFlow"]["from_prev_blk"] = {{"grams", value_flow.from_prev_blk.grams->to_dec_string()},
                                                {"extra", parse_extra_currency(value_flow.from_prev_blk.extra)}};
        answer["ValueFlow"]["to_next_blk"] = {{"grams", value_flow.to_next_blk.grams->to_dec_string()},
                                              {"extra", parse_extra_currency(value_flow.to_next_blk.extra)}};
        answer["ValueFlow"]["imported"] = {{"grams", value_flow.imported.grams->to_dec_string()},
                                           {"extra", parse_extra_currency(value_flow.imported.extra)}};
        answer["ValueFlow"]["exported"] = {{"grams", value_flow.exported.grams->to_dec_string()},
                                           {"extra", parse_extra_currency(value_flow.exported.extra)}};
        answer["ValueFlow"]["fees_collected"] = {{"grams", value_flow.fees_collected.grams->to_dec_string()},
                                                 {"extra", parse_extra_currency(value_flow.fees_collected.extra)}};
        answer["ValueFlow"]["fees_imported"] = {{"grams", value_flow.fees_imported.grams->to_dec_string()},
                                                {"extra", parse_extra_currency(value_flow.fees_imported.extra)}};
        answer["ValueFlow"]["recovered"] = {{"grams", value_flow.recovered.grams->to_dec_string()},
                                            {"extra", parse_extra_currency(value_flow.recovered.extra)}};
        answer["ValueFlow"]["created"] = {{"grams", value_flow.created.grams->to_dec_string()},
                                          {"extra", parse_extra_currency(value_flow.created.extra)}};
        answer["ValueFlow"]["minted"] = {{"grams", value_flow.minted.grams->to_dec_string()},
                                         {"extra", parse_extra_currency(value_flow.minted.extra)}};

        /* tlb
         block_extra in_msg_descr:^InMsgDescr
          out_msg_descr:^OutMsgDescr
          account_blocks:^ShardAccountBlocks
          rand_seed:bits256
          created_by:bits256
          custom:(Maybe ^McBlockExtra) = BlockExtra;
        */

        auto in_msg_dict = std::make_unique<vm::AugmentedDictionary>(vm::load_cell_slice_ref(extra.in_msg_descr), 256,
                                                                     block::tlb::aug_InMsgDescr);

        std::vector<json> in_msgs_json;
        while (!in_msg_dict->is_empty()) {
          td::Bits256 last_key;

          in_msg_dict->get_minmax_key(last_key);
          Ref<vm::CellSlice> data = in_msg_dict->lookup_delete(last_key);

          json parsed = {{"hash", last_key.to_hex()}, {"message", parse_in_msg_descr(data.write(), workchain)}};
          in_msgs_json.emplace_back(std::move(parsed));
        }

        auto out_msg_dict = std::make_unique<vm::AugmentedDictionary>(vm::load_cell_slice_ref(extra.out_msg_descr), 256,
                                                                      block::tlb::aug_OutMsgDescr);

        std::vector<json> out_msgs_json;
        while (!out_msg_dict->is_empty()) {
          td::Bits256 last_key;

          out_msg_dict->get_minmax_key(last_key);
          Ref<vm::CellSlice> data = out_msg_dict->lookup_delete(last_key);

          json parsed = {{"hash", last_key.to_hex()}, {"message", parse_out_msg_descr(data.write(), workchain)}};
          out_msgs_json.emplace_back(std::move(parsed));
        }

        auto account_blocks_dict = std::make_unique<vm::AugmentedDictionary>(
            vm::load_cell_slice_ref(extra.account_blocks), 256, block::tlb::aug_ShardAccountBlocks);

        /* tlb
           acc_trans#5 account_addr:bits256
             transactions:(HashmapAug 64 ^Transaction CurrencyCollection)
             state_update:^(HASH_UPDATE Account)
            = AccountBlock;

          _ (HashmapAugE 256 AccountBlock CurrencyCollection) = ShardAccountBlocks;
         */

        std::vector<json> accounts;
        std::vector<td::Bits256> accounts_keys;
        while (!account_blocks_dict->is_empty()) {
          td::Bits256 last_key;
          Ref<vm::CellSlice> data;

          account_blocks_dict->get_minmax_key(last_key);
          auto hex_addr = last_key.to_hex();
          // todo: fix
          if (hex_addr != "3333333333333333333333333333333333333333333333333333333333333333" &&
              hex_addr != "34517C7BDF5187C55AF4F8B61FDC321588C7AB768DEE24B006DF29106458D7CF" &&
              hex_addr != "5555555555555555555555555555555555555555555555555555555555555555" &&
              hex_addr != "0000000000000000000000000000000000000000000000000000000000000000" &&
              hex_addr != "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEF") {
            accounts_keys.emplace_back(last_key);
          }

          data = account_blocks_dict->lookup_delete(last_key);

          json account_block_parsed;
          account_block_parsed["account_addr"] = {{"address", last_key.to_hex()}, {"workchain", workchain}};

          block::gen::AccountBlock::Record acc_blk;
          CHECK(tlb::csr_unpack(data, acc_blk));
          int count = 0;
          std::vector<json> transactions;

          vm::AugmentedDictionary trans_dict{vm::DictNonEmpty(), std::move(acc_blk.transactions), 64,
                                             block::tlb::aug_AccountTransactions};

          /* tlb
            transaction$0111 account_addr:bits256 lt:uint64
            prev_trans_hash:bits256 prev_trans_lt:uint64 now:uint32
            outmsg_cnt:uint15
            orig_status:AccountStatus end_status:AccountStatus
            ^[ in_msg:(Maybe ^(Message Any)) out_msgs:(HashmapE 15 ^(Message Any)) ]
            total_fees:CurrencyCollection state_update:^(HASH_UPDATE Account)
            description:^TransactionDescr = Transaction;
           */

          while (!trans_dict.is_empty()) {
            td::BitArray<64> last_lt{};
            trans_dict.get_minmax_key(last_lt);

            Ref<vm::CellSlice> tvalue;
            tvalue = trans_dict.lookup_delete(last_lt);

            json transaction = parse_transaction(tvalue, workchain);
            transactions.emplace_back(std::move(transaction));

            ++count;
          };

          account_block_parsed["transactions"] = transactions;
          account_block_parsed["transactions_count"] = count;
          accounts.emplace_back(account_block_parsed);
        }

        if (true /*accounts_keys.size() > 0*/) {
          increase_state_padding();
          td::actor::send_closure(SelfId, &Indexer::got_state_accounts, block_handle, accounts_keys);
        }

        answer["BlockExtra"] = {
            {"accounts", std::move(accounts)},         {"rand_seed", extra.rand_seed.to_hex()},
            {"created_by", extra.created_by.to_hex()}, {"out_msg_descr", std::move(out_msgs_json)},
            {"in_msg_descr", std::move(in_msgs_json)},
        };

        if ((int)extra.custom->prefetch_ulong(1) == 1) {
          auto mc_extra = extra.custom->prefetch_ref();

          block::gen::McBlockExtra::Record extra_mc;
          CHECK(tlb::unpack_cell(mc_extra, extra_mc));

          answer["BlockExtra"]["custom"] = {
              {"key_block", extra_mc.key_block},
          };

          if (extra_mc.key_block) {
            block::gen::ConfigParams::Record cp;
            CHECK(tlb::unpack(extra_mc.config.write(), cp));

            answer["BlockExtra"]["custom"]["config_addr"] = cp.config_addr.to_hex();

            std::map<long long, std::string> configs;

            vm::Dictionary config_dict{cp.config, 32};

            while (!config_dict.is_empty()) {
              td::BitArray<32> key{};
              config_dict.get_minmax_key(key);

              Ref<vm::Cell> tvalue;
              tvalue = config_dict.lookup_delete(key)->prefetch_ref();

              configs[key.to_long()] = dump_as_boc(tvalue);
            };

            answer["BlockExtra"]["custom"]["configs"] = configs;
          };

          //          vm::Dictionary shard_fees_dict{extra_mc.shard_fees->prefetch_ref(), 96};
          //          std::map<std::string, json> shard_fees;
          //
          //          while (!shard_fees_dict.is_empty()) {
          //            td::BitArray<96> key{};
          //            shard_fees_dict.get_minmax_key(key);
          //
          //            Ref<vm::CellSlice> tvalue;
          //            tvalue = shard_fees_dict.lookup_delete(key);
          //
          //            block::gen::ShardFeeCreated::Record sf;
          //            CHECK(tlb::unpack(tvalue.write(), sf));
          //
          //            block::gen::CurrencyCollection::Record fees;
          //            block::gen::CurrencyCollection::Record create;
          //
          //            CHECK(tlb::unpack(sf.fees.write(), fees));
          //            CHECK(tlb::unpack(sf.create.write(), create));
          //
          //            std::vector<std::tuple<int, std::string>> dummy;
          //
          //            json data = {
          //                {"fees",
          //                 {{"grams", block::tlb::t_Grams.as_integer(fees.grams)->to_dec_string()},
          //                  {"extra", fees.other->have_refs() ? parse_extra_currency(fees.other->prefetch_ref()) : dummy}}},
          //
          //                {"create",
          //                 {{"grams", block::tlb::t_Grams.as_integer(create.grams)->to_dec_string()},
          //                  {"extra", create.other->have_refs() ? parse_extra_currency(create.other->prefetch_ref()) : dummy}}}};
          //
          //            shard_fees[key.to_hex()] = data;
          //          };
          //
          //          answer["BlockExtra"]["custom"]["shard_fees"] = shard_fees;

          if (extra_mc.r1.mint_msg->have_refs()) {
            answer["BlockExtra"]["custom"]["mint_msg"] =
                parse_in_msg_descr(load_cell_slice(extra_mc.r1.mint_msg->prefetch_ref()), workchain);
          }

          if (extra_mc.r1.recover_create_msg->have_refs()) {
            answer["BlockExtra"]["custom"]["recover_create_msg"] =
                parse_in_msg_descr(load_cell_slice(extra_mc.r1.recover_create_msg->prefetch_ref()), workchain);
          }

          if (extra_mc.r1.prev_blk_signatures->have_refs()) {
            vm::Dictionary prev_blk_signatures{extra_mc.r1.prev_blk_signatures->prefetch_ref(), 16};
            std::vector<json> prev_blk_signatures_json;

            while (!prev_blk_signatures.is_empty()) {
              td::BitArray<16> key{};
              prev_blk_signatures.get_minmax_key(key);

              Ref<vm::CellSlice> tvalue;
              tvalue = prev_blk_signatures.lookup_delete(key);

              block::gen::CryptoSignaturePair::Record cs_pair;
              block::gen::CryptoSignatureSimple::Record css{};

              CHECK(tlb::unpack(tvalue.write(), cs_pair));

              CHECK(tlb::unpack(cs_pair.sign.write(), css));

              json data = {{"key", key.to_long()},
                           {"node_id_short", cs_pair.node_id_short.to_hex()},
                           {
                               "sign",
                               {"R", css.R.to_hex()},
                               {"s", css.s.to_hex()},
                           }};

              prev_blk_signatures_json.emplace_back(std::move(data));
            };

            answer["BlockExtra"]["custom"]["prev_blk_signatures"] = std::move(prev_blk_signatures_json);
          };

          block::ShardConfig shards;
          shards.unpack(extra_mc.shard_hashes);

          std::vector<json> shards_json;

          auto f = [&shards_json, &SelfId, &blkid, is_first](McShardHash &ms) {
            json data = {{"BlockIdExt",
                          {{"file_hash", ms.top_block_id().file_hash.to_hex()},
                           {"root_hash", ms.top_block_id().root_hash.to_hex()},
                           {"id",
                            {
                                {"workchain", ms.top_block_id().id.workchain},
                                {"seqno", ms.top_block_id().id.seqno},
                                {"shard", ms.top_block_id().id.shard},
                            }}}},
                         {"start_lt", ms.start_lt()},
                         {"end_lt", ms.end_lt()},
                         {"before_split", ms.before_split()},
                         {"before_merge", ms.before_merge()},
                         {"shard",
                          {
                              {"workchain", ms.shard().workchain},
                              {"shard", ms.shard().shard},
                          }},
                         {"fsm_utime", ms.fsm_utime()},
                         {"fsm_state", ms.fsm_state()}};

            shards_json.emplace_back(std::move(data));

            auto shard_seqno = ms.top_block_id().id.seqno;
            auto shard_shard = ms.top_block_id().id.shard;
            auto shard_workchain = ms.shard().workchain;

            LOG(DEBUG) << "FOR: " << blkid.to_str() << " first: " << is_first;
            LOG(DEBUG) << "GO: " << shard_workchain << ":" << shard_shard << ":" << shard_seqno;

            td::actor::send_closure(SelfId, &Indexer::start_parse_shards, shard_seqno, shard_shard, shard_workchain,
                                    is_first);

            return 1;
          };

          shards.process_shard_hashes(f);
          answer["BlockExtra"]["custom"]["shards"] = shards_json;
        }

        vm::CellSlice upd_cs{vm::NoVmSpec(), blk.state_update};
        if (!(upd_cs.is_special() && upd_cs.prefetch_long(8) == 4  // merkle update
              && upd_cs.size_ext() == 0x20228)) {
          LOG(ERROR) << "invalid Merkle update in block";
          return;
        }

        CHECK(upd_cs.have_refs(2));
        auto state_old_hash = upd_cs.prefetch_ref(0)->get_hash(0).to_hex();
        auto state_hash = upd_cs.prefetch_ref(1)->get_hash(0).to_hex();

        answer["ShardState"] = {{"state_old_hash", state_old_hash}, {"state_hash", state_hash}};

        {
          std::lock_guard<std::mutex> lock(stored_counter_mtx_);
          ++stored_blocks_counter_;
        }
        dumper_.storeBlock(
            std::to_string(workchain) + ":" + std::to_string(blkid.id.shard) + ":" + std::to_string(blkid.seqno()),
            std::move(answer));

        td::actor::send_closure(SelfId, &Indexer::decrease_block_padding);

        if (is_first && !info.not_master) {
          td::actor::send_closure(SelfId, &Indexer::parse_other);
        }
      }
    });
    td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::get_block_data_from_db, handle,
                            std::move(P));
  }

  void increase_block_padding() {
    std::unique_lock<std::mutex> lock(display_mtx_);
    ++block_padding_;
    progress_changed();
  }

  void decrease_block_padding() {
    std::unique_lock<std::mutex> lock(display_mtx_);

    if (display_speed_) {
      parsed_blocks_timepoints_.emplace(std::chrono::high_resolution_clock::now());
    }

    if (block_padding_ == 0) {
      LOG(ERROR) << "decreasing seqno padding but it's zero";
    }
    --block_padding_;
    progress_changed();
  }

  void increase_state_padding() {
    std::unique_lock<std::mutex> lock(display_mtx_);

    ++state_padding_;
    progress_changed();
  }

  void decrease_state_padding() {
    std::unique_lock<std::mutex> lock(display_mtx_);

    if (display_speed_) {
      parsed_states_timepoints_.emplace(std::chrono::high_resolution_clock::now());
    }

    if (state_padding_ == 0) {
      LOG(ERROR) << "decreasing state padding but it's zero";
    }
    --state_padding_;
    progress_changed();
  }

  void progress_changed() {
    if (display_speed_) {
      display_speed();
    }

    if (block_padding_ == 0 && state_padding_ == 0) {
      LOG(WARNING) << "Block&State paddings reached 0; Dump json files;";
      dumper_.forceDump();

      if (chunk_current_ != chunk_count_) {
        LOG(WARNING) << "Call parse next chunk";
        delay_action(
            [SelfId = actor_id(this)]() {
              td::actor::send_closure(SelfId, &Indexer::parse_other);
            }, td::Timestamp::in(0.1));
      } else {
        shutdown();
      }
    }
  }

  void display_speed() {
    while (!parsed_blocks_timepoints_.empty()) {
      const auto timepoint = parsed_blocks_timepoints_.front();
      if (std::chrono::high_resolution_clock::now() - timepoint < std::chrono::seconds(1)) {
        break;
      }
      parsed_blocks_timepoints_.pop();
    }
    while (!parsed_states_timepoints_.empty()) {
      const auto timepoint = parsed_states_timepoints_.front();
      if (std::chrono::high_resolution_clock::now() - timepoint < std::chrono::seconds(1)) {
        break;
      }
      parsed_states_timepoints_.pop();
    }

    std::ostringstream oss;

    if (verbosity == 0) {
      oss << '\r';
      for (auto i = 0; i < 112; ++i)
        oss << ' ';
      oss << '\r';
    }

    oss << "speed(blocks/s):\t" << parsed_blocks_timepoints_.size() << "\tpadding:\t" << block_padding_ << '\t'
        << "speed(states/s):\t" << parsed_states_timepoints_.size() << "\tpadding:\t" << state_padding_ << '\t';

    if (verbosity == 0) {
      std::cout << oss.str() << std::flush;
    } else {
      LOG(INFO) << oss.str();
    }
  }

  void shutdown() {
    LOG(INFO) << "Ready to die";
    ///TODO: danger danger
    LOG(WARNING) << "Calling std::exit(0)";
    std::exit(0);
  }

  void got_state_accounts(std::shared_ptr<const BlockHandleInterface> handle, std::vector<td::Bits256> accounts_keys) {
    auto P = td::PromiseCreator::lambda([this, SelfId = actor_id(this), handle,
                                         accounts_keys =
                                             std::move(accounts_keys)](td::Result<td::Ref<vm::DataCell>> R) {
      if (R.is_error()) {
        LOG(ERROR) << R.move_as_error().to_string();
      } else {
        auto root_cell = R.move_as_ok();
        auto block_id = handle->id();

        LOG(INFO) << "Parse state: " << block_id.to_str();

        block::gen::ShardStateUnsplit::Record shard_state;
        CHECK(tlb::unpack_cell(root_cell, shard_state));

        std::vector<std::tuple<int, std::string>> dummy;

        block::gen::CurrencyCollection::Record total_balance_cc;
        block::gen::CurrencyCollection::Record total_validator_fees_cc;

        CHECK(tlb::unpack(shard_state.r1.total_balance.write(), total_balance_cc))
        CHECK(tlb::unpack(shard_state.r1.total_validator_fees.write(), total_validator_fees_cc))

        json total_balance = {
            {"grams", block::tlb::t_Grams.as_integer(total_balance_cc.grams)->to_dec_string()},
            {"extra", total_balance_cc.other->have_refs() ? parse_extra_currency(total_balance_cc.other->prefetch_ref())
                                                          : dummy}};

        json total_validator_fees = {
            {"grams", block::tlb::t_Grams.as_integer(total_validator_fees_cc.grams)->to_dec_string()},
            {"extra", total_balance_cc.other->have_refs()
                          ? parse_extra_currency(total_validator_fees_cc.other->prefetch_ref())
                          : dummy}};

        json answer = {
            {"type", "shard_state"},
            {"id",
             {
                 {"workchain", block_id.id.workchain},
                 {"seqno", block_id.id.seqno},
                 {"shard", block_id.id.shard},
             }},
            {"seq_no", shard_state.seq_no},
            {"vert_seq_no", shard_state.vert_seq_no},
            {"gen_utime", shard_state.gen_utime},
            {"gen_lt", shard_state.gen_lt},
            {"min_ref_mc_seqno", shard_state.min_ref_mc_seqno},
            {"before_split", shard_state.before_split},
            {"overload_history", shard_state.r1.overload_history},
            {"underload_history", shard_state.r1.underload_history},
            {"total_balance", total_balance},
            {"total_validator_fees", total_validator_fees},
        };

        if (shard_state.r1.libraries->have_refs()) {
          auto libraries = vm::Dictionary{shard_state.r1.libraries->prefetch_ref(), 256};

          std::vector<json> libs;
          while (!libraries.is_empty()) {
            td::BitArray<256> key{};
            libraries.get_minmax_key(key);
            auto lib = libraries.lookup_delete(key);

            block::gen::LibDescr::Record libdescr;
            CHECK(tlb::unpack(lib.write(), libdescr));

            std::vector<std::string> publishers;

            auto libs_publishers = libdescr.publishers.write();

            vm::CellBuilder cb;
            Ref<vm::Cell> cool_cell;

            cb.append_cellslice(libs_publishers);
            cb.finalize_to(cool_cell);

            auto publishers_dict = vm::Dictionary{cool_cell, 256};

            while (!publishers_dict.is_empty()) {
              td::BitArray<256> publisher{};
              publishers_dict.get_minmax_key(publisher);
              publishers_dict.lookup_delete(publisher);

              publishers.emplace_back(publisher.to_hex());
            }

            json data = {{"hash", key.to_hex()}, {"lib", dump_as_boc(libdescr.lib)}, {"publishers", publishers}};
            libs.emplace_back(std::move(data));
          }

          answer["libraries"] = std::move(libs);
        }

        vm::AugmentedDictionary accounts{vm::load_cell_slice_ref(shard_state.accounts), 256,
                                         block::tlb::aug_ShardAccounts};

        std::vector<json> accounts_list;
        accounts_list.reserve(accounts_keys.size());

        for (const auto &account : accounts_keys) {
          LOG(DEBUG) << "Parse " << account.to_hex();
          auto result = accounts.lookup(account.cbits(), 256);
          //          auto value = result.first;
          //          auto extra = result.second;
          auto value = result;
          if (value.not_null()) {
            block::gen::ShardAccount::Record sa;
            block::gen::CurrencyCollection::Record dbi_cc;
            CHECK(tlb::unpack(value.write(), sa));
            json data;

            data["account_address"] = {{"workchain", block_id.id.workchain}, {"address", account.to_hex()}};
            data["account"] = {{"last_trans_hash", sa.last_trans_hash.to_hex()}, {"last_trans_lt", sa.last_trans_lt}};

            auto account_cell = load_cell_slice(sa.account);
            auto acc_tag = block::gen::t_Account.get_tag(account_cell);

            if (acc_tag == block::gen::t_Account.account) {
              block::gen::Account::Record_account acc;
              block::gen::StorageInfo::Record si;
              block::gen::AccountStorage::Record as;
              block::gen::StorageUsed::Record su;
              block::gen::CurrencyCollection::Record balance;

              CHECK(tlb::unpack(account_cell, acc));

              CHECK(tlb::unpack(acc.storage.write(), as));
              CHECK(tlb::unpack(acc.storage_stat.write(), si));
              CHECK(tlb::unpack(si.used.write(), su));
              CHECK(tlb::unpack(as.balance.write(), balance));
              data["account"]["addr"] = parse_address(acc.addr.write());
              std::string due_payment;

              if (si.due_payment->prefetch_ulong(1) > 0) {
                auto due = si.due_payment.write();
                due.fetch_bits(1);  // maybe
                due_payment = block::tlb::t_Grams.as_integer(due)->to_dec_string();
              }

              data["account"]["storage_stat"] = {{"last_paid", si.last_paid}, {"due_payment", due_payment}};

              data["account"]["storage_stat"]["used"] = {
                  {"cells", block::tlb::t_VarUInteger_7.as_uint(su.cells.write())},
                  {"bits", block::tlb::t_VarUInteger_7.as_uint(su.bits.write())},
                  {"public_cells", block::tlb::t_VarUInteger_7.as_uint(su.public_cells.write())},
              };

              data["account"]["storage"] = {{"last_trans_lt", as.last_trans_lt}};

              data["account"]["storage"]["balance"] = {
                  {"grams", block::tlb::t_Grams.as_integer(balance.grams)->to_dec_string()},
                  {"extra", balance.other->have_refs() ? parse_extra_currency(balance.other->prefetch_ref()) : dummy}};

              auto tag = block::gen::t_AccountState.get_tag(as.state.write());

              if (tag == block::gen::t_AccountState.account_uninit) {
                data["account"]["state"] = {{"type", "uninit"}};
              }

              else if (tag == block::gen::t_AccountState.account_active) {
                block::gen::AccountState::Record_account_active active_account;
                CHECK(tlb::unpack(as.state.write(), active_account));

                data["account"]["state"] = {{"type", "active"},
                                            {"state_init", parse_state_init(active_account.x.write())}};

              }

              else if (tag == block::gen::t_AccountState.account_frozen) {
                block::gen::AccountState::Record_account_frozen f{};
                CHECK(tlb::unpack(as.state.write(), f))
                data["account"]["state"] = {{"type", "frozen"}, {"state_hash", f.state_hash.to_hex()}};
              }
            }

            accounts_list.emplace_back(std::move(data));
          }
        }

        answer["accounts"] = std::move(accounts_list);

        {
          std::lock_guard<std::mutex> lock(stored_counter_mtx_);
          ++stored_states_counter_;
        }
        dumper_.storeState(std::to_string(block_id.id.workchain) + ":" + std::to_string(block_id.id.shard) + ":" +
                               std::to_string(block_id.id.seqno),
                           std::move(answer));

        LOG(DEBUG) << "received & parsed state from db " << block_id.to_str();
        td::actor::send_closure(SelfId, &Indexer::decrease_state_padding);
      }
    });

    LOG(DEBUG) << "getting state from db " << handle->id().to_str();
    td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::get_shard_state_root_cell_from_db, handle,
                            std::move(P));
  }
};  // namespace validator
}  // namespace validator
}  // namespace ton

td::actor::ActorOwn<ton::validator::Indexer> indexer;

int main(int argc, char **argv) {
  SET_VERBOSITY_LEVEL(verbosity_DEBUG);

  CHECK(vm::init_op_cp0());

  td::OptionParser p;
  p.set_description("blockchain indexer");
  p.add_option('h', "help", "prints_help", [&]() {
    char b[10240];
    td::StringBuilder sb(td::MutableSlice{b, 10000});
    sb << p;
    std::cout << sb.as_cslice().c_str();
    std::exit(2);
  });
  p.add_checked_option('v', "verbosity", "set verbosity level", [&](td::Slice arg) {
    verbosity = td::to_integer<int>(arg);
    SET_VERBOSITY_LEVEL(VERBOSITY_NAME(FATAL) + verbosity);
    return (verbosity >= 0 && verbosity <= 9) ? td::Status::OK() : td::Status::Error("verbosity must be 0..9");
  });
  p.add_checked_option('u', "user", "change user", [&](td::Slice user) { return td::change_user(user.str()); });
  p.add_option('D', "db", "root for dbs", [&](td::Slice fname) {
    td::actor::send_closure(indexer, &ton::validator::Indexer::set_db_root, fname.str());
  });
  p.add_option('C', "config", "global config path", [&](td::Slice fname) {
    td::actor::send_closure(indexer, &ton::validator::Indexer::set_global_config_path, fname.str());
  });
  td::uint32 threads = 24;
  p.add_checked_option(
      't', "threads", PSTRING() << "number of threads (default=" << threads << ")", [&](td::Slice arg) {
        td::uint32 v;
        try {
          v = std::stoi(arg.str());
        } catch (...) {
          return td::Status::Error(ton::ErrorCode::error, "bad value for --threads: not a number");
        }
        if (v < 1 || v > 256) {
          return td::Status::Error(ton::ErrorCode::error, "bad value for --threads: should be in range [1..256]");
        }
        threads = v;
        return td::Status::OK();
      });

  p.add_checked_option('c', "chunk-size", PSTRING() << "number of blocks per chunk (default=20000)",
                       [&](td::Slice arg) {
                         TRY_RESULT(size, td::to_integer_safe<ton::BlockSeqno>(arg.str()));
                         td::actor::send_closure(indexer, &ton::validator::Indexer::set_chunk_size, size);
                         return td::Status::OK();
                       });

  p.add_checked_option('s', "seqno", "seqno_first[:seqno_last]\tseqno range", [&](td::Slice arg) {
    auto pos = std::min(arg.find(':'), arg.size());
    TRY_RESULT(seqno_first, td::to_integer_safe<ton::BlockSeqno>(arg.substr(0, pos)));
    ++pos;
    if (pos >= arg.size()) {
      td::actor::send_closure(indexer, &ton::validator::Indexer::set_seqno_range, seqno_first, seqno_first);
      return td::Status::OK();
    }
    TRY_RESULT(seqno_last, td::to_integer_safe<ton::BlockSeqno>(arg.substr(pos, arg.size())));
    td::actor::send_closure(indexer, &ton::validator::Indexer::set_seqno_range, seqno_first, seqno_last);
    return td::Status::OK();
  });
  p.add_checked_option('S', "speed", "display speed true/false", [&](td::Slice arg) {
    const auto str = arg.str();
    if (str == "true") {
      td::actor::send_closure(indexer, &ton::validator::Indexer::set_display_speed, true);
      return td::Status::OK();
    } else if (str == "false") {
      td::actor::send_closure(indexer, &ton::validator::Indexer::set_display_speed, false);
      return td::Status::OK();
    } else {
      return td::Status::Error(ton::ErrorCode::error, "bad value for --progress: not true or false");
    }
  });

  td::actor::set_debug(true);
  td::actor::Scheduler scheduler({threads});  // contans a bug: threads not initialized by OptionsParser
  scheduler.run_in_context([&] { indexer = td::actor::create_actor<ton::validator::Indexer>("cool"); });
  scheduler.run_in_context([&] { p.run(argc, argv).ensure(); });
  scheduler.run_in_context([&] { td::actor::send_closure(indexer, &ton::validator::Indexer::run); });
  scheduler.run();
  scheduler.stop();
  return 0;
}
