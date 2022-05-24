//
// Created by tvorogme on 5/21/22.
//

#include "td/utils/logging.h"
#include "td/actor/actor.h"
#include "td/utils/Time.h"
#include "td/utils/Random.h"
#include "td/utils/filesystem.h"
#include "td/utils/JsonBuilder.h"
#include "auto/tl/ton_api_json.h"
#include "crypto/vm/cp0.h"
#include "validator/validator.h"
#include "validator/manager-disk.h"
#include "ton/ton-types.h"
#include "ton/ton-tl.hpp"
#include "ton/ton-io.hpp"
#include "vm/boc.h"
#include "tl/tlblib.hpp"
#include "block/block.h"
#include "block/block-parse.h"
#include "block/block-auto.h"
#include "block/check-proof.h"
#include "vm/dict.h"
#include "vm/cells/MerkleProof.h"
#include "vm/vm.h"
#include "liteserver.hpp"
#include "td/utils/Slice.h"
#include "td/utils/common.h"
#include "td/utils/crypto.h"
#include "td/utils/overloaded.h"
#include "auto/tl/lite_api.h"
#include "auto/tl/lite_api.hpp"
#include "adnl/utils.hpp"
#include "ton/lite-tl.hpp"
#include "tl-utils/lite-utils.hpp"
#include "td/utils/Random.h"
#include "vm/boc.h"
#include "tl/tlblib.hpp"
#include "block/block.h"
#include "block/block-parse.h"
#include "block/block-auto.h"
#include "block/check-proof.h"
#include "vm/dict.h"
#include "vm/cells/MerkleProof.h"
#include "vm/vm.h"
#include "vm/memo.h"
#include "shard.hpp"
#include "validator-set.hpp"
using td::Ref;
static bool visit(Ref<vm::Cell> cell);

static bool visit(const vm::CellSlice &cs) {
  auto cnt = cs.size_refs();
  bool res = true;
  for (unsigned i = 0; i < cnt; i++) {
    res &= visit(cs.prefetch_ref(i));
  }
  return res;
}

static bool visit(Ref<vm::Cell> cell) {
  if (cell.is_null()) {
    return true;
  }
  vm::CellSlice cs{vm::NoVm{}, std::move(cell)};
  return visit(cs);
}

static bool visit(Ref<vm::CellSlice> cs_ref) {
  return cs_ref.is_null() || visit(*cs_ref);
}

namespace ton {

namespace validator {
class Indexer : public td::actor::Actor {
 private:
  std::string db_root_ = "/mnt/ton/ton-node/db";
  std::string config_path_ = db_root_ + "/global-config.json";
  td::Ref<ton::validator::ValidatorManagerOptions> opts_;
  td::actor::ActorOwn<ton::validator::ValidatorManagerInterface> validator_manager_;

  td::Status create_validator_options() {
    TRY_RESULT_PREFIX(conf_data, td::read_file(config_path_), "failed to read: ");
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
      h.push_back(b);
    }
    opts_.write().set_hardforks(std::move(h));
    return td::Status::OK();
  }

 public:
  void run() {
    LOG(DEBUG) << "Use db root: " << db_root_;

    auto Sr = create_validator_options();
    if (Sr.is_error()) {
      LOG(ERROR) << "failed to load global config'" << config_path_ << "': " << Sr;
      std::_Exit(2);
    } else {
      LOG(DEBUG) << "Global config loaded successfully from " << config_path_;
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
        LOG(DEBUG) << "INITIAL READ COMPLETE";
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

    LOG(DEBUG) << "Callback";
    auto P_cb = td::PromiseCreator::lambda([](td::Unit R) {});
    td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::install_callback,
                            std::make_unique<Callback>(actor_id(this)), std::move(P_cb));
    LOG(DEBUG) << "Callback installed";
  }

  void sync_complete(const BlockHandle &handle) {
    LOG(DEBUG) << "Sync complete: " << handle->id().to_str();

    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<ConstBlockHandle> R) {
      LOG(DEBUG) << "Got Answer!";

      if (R.is_error()) {
        LOG(ERROR) << R.move_as_error().to_string();
      } else {
        auto handle = R.move_as_ok();
        LOG(DEBUG) << "requesting data for block " << handle->id().to_str();
        td::actor::send_closure(SelfId, &Indexer::got_block_handle, handle);
      }
    });

    LOG(DEBUG) << "sending get_block_by_seqno_from_db request";
    ton::AccountIdPrefixFull pfx{ton::masterchainId, 0x8000000000000000};
    td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::get_block_by_seqno_from_db, pfx, 20000000,
                            std::move(P));
  }

  void got_block_handle(std::shared_ptr<const BlockHandleInterface> handle) {
    LOG(DEBUG) << "Returned to Indexer";

    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<BlockData>> R) {
      LOG(DEBUG) << "GOT!";

      if (R.is_error()) {
        LOG(ERROR) << R.move_as_error().to_string();
      } else {
        auto block = R.move_as_ok();
        LOG(DEBUG) << "data was received!";
        LOG(DEBUG) << block->block_id();
        auto blkid = block->block_id();

        CHECK(block.not_null());

        auto block_root = block->root_cell();

        if (block_root.is_null()) {
          LOG(ERROR) << "block has no valid root cell";
          return;
        }
        vm::MerkleProofBuilder mpb{block_root};
        block::gen::Block::Record blk;
        block::gen::BlockInfo::Record info;

        LOG(DEBUG) << " ------------ PARSED BLOCK HEADER ------------";
        LOG(DEBUG) << "Field: block | Value: " << blkid.to_str();
        LOG(DEBUG) << "Field: roothash | Value: " << blkid.root_hash.to_hex();
        LOG(DEBUG) << "Field: filehash | Value: " << blkid.file_hash.to_hex();
        LOG(DEBUG) << "Field: time | Value: " << info.gen_utime;
        LOG(DEBUG) << "Field: Start LT | Value: " << info.start_lt;
        LOG(DEBUG) << "Field: End LT | Value: " << info.end_lt;
        LOG(DEBUG) << "Field: Global ID | Value: " << blk.global_id;
        LOG(DEBUG) << "Field: Version | Value: " << info.version;
        LOG(DEBUG) << "Field: Flags | Value: " << info.flags;
        LOG(DEBUG) << "Field: Key block | Value: " << info.key_block;
        LOG(DEBUG) << "Field: Not master | Value: " << info.not_master;
        LOG(DEBUG) << "Field: After merge | Value: " << info.after_merge;
        LOG(DEBUG) << "Field: After split | Value: " << info.after_split;
        LOG(DEBUG) << "Field: Before split | Value: " << info.before_split;
        LOG(DEBUG) << "Field: Want merge | Value: " << info.want_merge;
        LOG(DEBUG) << "Field: Want split | Value: " << info.want_split;
        LOG(DEBUG) << "Field: validator_list_hash_short | Value: " << info.gen_validator_list_hash_short;
        LOG(DEBUG) << "Field: catchain_seqno | Value: " << info.gen_catchain_seqno;
        LOG(DEBUG) << "Field: min_ref_mc_seqno | Value: " << info.min_ref_mc_seqno;
        LOG(DEBUG) << "Field: vert_seqno | Value: " << info.vert_seq_no;
        LOG(DEBUG) << "Field: vert_seqno_incr | Value: " << info.vert_seqno_incr;
        LOG(DEBUG) << "Field: prev_key_block_seqno | Value: "
                   << ton::BlockId{ton::masterchainId, ton::shardIdAll, info.prev_key_block_seqno};
        LOG(DEBUG) << " ------------ PARSED BLOCK HEADER ------------";


        block::gen::BlockExtra::Record extra;
        if (!(tlb::unpack_cell(std::move(blk.extra), extra))) {
          LOG(ERROR) << "Can't unpack extra";
          return;
        }

      }
    });

    td::actor::send_closure_later(validator_manager_, &ValidatorManagerInterface::get_block_data_from_db, handle,
                                  std::move(P));
  }
};
}  // namespace validator
}  // namespace ton

int main(int argc, char **argv) {
  SET_VERBOSITY_LEVEL(verbosity_DEBUG);

  LOG(DEBUG) << "Let's rock!";

  CHECK(vm::init_op_cp0());

  td::actor::ActorOwn<ton::validator::Indexer> main;

  //td::actor::send_closure(main, &Indexer::run);
  td::actor::set_debug(true);
  td::actor::Scheduler scheduler({24});
  scheduler.run_in_context([&] { main = td::actor::create_actor<ton::validator::Indexer>("cool"); });
  scheduler.run_in_context([&] { td::actor::send_closure(main, &ton::validator::Indexer::run); });
  scheduler.run();
  return 0;
}