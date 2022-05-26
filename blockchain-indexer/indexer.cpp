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
#include "crypto/block/transaction.h"

#include <utility>
#include "auto/tl/lite_api.h"
#include "adnl/utils.hpp"
#include "shard.hpp"
#include "validator-set.hpp"
#include "json.hpp"

// TODO: use td/utils/json
using json = nlohmann::json;

using td::Ref;

namespace ton {

namespace validator {

std::list<std::tuple<int, std::string>> parse_extra_currency(const Ref<vm::Cell> &extra) {
  std::list<std::tuple<int, std::string>> c_list;

  if (extra.not_null()) {
    vm::Dictionary dict{extra, 32};
    !dict.check_for_each([&c_list](td::Ref<vm::CellSlice> csr, td::ConstBitPtr key, int n) {
      CHECK(n == 32);
      int x = (int)key.get_int(n);
      auto val = block::tlb::t_VarUIntegerPos_32.as_integer_skip(csr.write());
      if (val.is_null() || !csr->empty_ext()) {
        return false;
      }

      c_list.emplace_back(x, val->to_dec_string());

      return true;
    });
  }

  return c_list;
}

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

    LOG(DEBUG) << "Callback";
    auto P_cb = td::PromiseCreator::lambda([](td::Unit R) {});
    td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::install_callback,
                            std::make_unique<Callback>(actor_id(this)), std::move(P_cb));
    LOG(DEBUG) << "Callback installed";
  }

  void sync_complete(const BlockHandle &handle) {
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

    ton::AccountIdPrefixFull pfx{ton::masterchainId, 0x8000000000000000};
    td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::get_block_by_seqno_from_db, pfx, 20000000,
                            std::move(P));
  }

  void got_block_handle(std::shared_ptr<const BlockHandleInterface> handle) {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<BlockData>> R) {
      if (R.is_error()) {
        LOG(ERROR) << R.move_as_error().to_string();
      } else {
        auto block = R.move_as_ok();
        CHECK(block.not_null());

        auto blkid = block->block_id();
        auto block_root = block->root_cell();
        if (block_root.is_null()) {
          LOG(ERROR) << "block has no valid root cell";
          return;
        }

        //
        // Parsing
        //

        json answer;
        auto workchain = blkid.id.workchain;

        answer["BlockIdExt"] = {{"file_hash", blkid.file_hash.to_hex()},
                                {"root_hash", blkid.root_hash.to_hex()},
                                {"id",
                                 {
                                     {"workchain", workchain},
                                     {"seqno", blkid.id.seqno},
                                     {"shard", blkid.id.shard},
                                 }}};
        LOG(DEBUG) << "BlockIdExt: " << to_string(answer["BlockIdExt"]);

        block::gen::Block::Record blk;
        block::gen::BlockInfo::Record info;
        block::gen::BlockExtra::Record extra;

        if (!(tlb::unpack_cell(block_root, blk) && tlb::unpack_cell(blk.extra, extra))) {
          LOG(ERROR) << "cannot unpack Block header";
          return;
        }

        answer["global_id"] = blk.global_id;
        auto now = info.gen_utime;

        // todo: master_ref, prev_ref, prev_vert_ref, gen_software
        answer["BlockInfo"] = {{"version", info.version},
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
                               {"start_lt", info.start_lt},
                               {"end_lt", info.end_lt},
                               {"gen_validator_list_hash_short", info.gen_validator_list_hash_short},
                               {"gen_catchain_seqno", info.gen_catchain_seqno},
                               {"min_ref_mc_seqno", info.min_ref_mc_seqno},
                               {"prev_key_block_seqno", info.prev_key_block_seqno}};

        LOG(DEBUG) << "BlockInfo: " << to_string(answer["BlockInfo"]);

        auto value_flow_root = blk.value_flow;
        block::ValueFlow value_flow;
        vm::CellSlice cs{vm::NoVmOrd(), value_flow_root};
        if (!(cs.is_valid() && value_flow.fetch(cs) && cs.empty_ext())) {
          LOG(ERROR) << "cannot unpack ValueFlow of the new block ";
          return;
        }

        answer["ValueFlow"] = {
            {"from_prev_blk",
             {"grams", value_flow.from_prev_blk.grams->to_dec_string()},
             {"extra", parse_extra_currency(value_flow.from_prev_blk.extra)}},

            {"to_next_blk",
             {"grams", value_flow.to_next_blk.grams->to_dec_string()},
             {"extra", parse_extra_currency(value_flow.to_next_blk.extra)}},

            {"imported",
             {"grams", value_flow.imported.grams->to_dec_string()},
             {"extra", parse_extra_currency(value_flow.imported.extra)}},

            {"exported",
             {"grams", value_flow.exported.grams->to_dec_string()},
             {"extra", parse_extra_currency(value_flow.exported.extra)}},

            {"fees_collected",
             {"grams", value_flow.fees_collected.grams->to_dec_string()},
             {"extra", parse_extra_currency(value_flow.fees_collected.extra)}},

            {"fees_collected",
             {"grams", value_flow.fees_imported.grams->to_dec_string()},
             {"extra", parse_extra_currency(value_flow.fees_imported.extra)}},

            {"recovered",
             {"grams", value_flow.recovered.grams->to_dec_string()},
             {"extra", parse_extra_currency(value_flow.recovered.extra)}},

            {"created",
             {"grams", value_flow.created.grams->to_dec_string()},
             {"extra", parse_extra_currency(value_flow.created.extra)}},

            {"minted",
             {"grams", value_flow.minted.grams->to_dec_string()},
             {"extra", parse_extra_currency(value_flow.minted.extra)}},
        };

        LOG(DEBUG) << "ValueFlow: " << to_string(answer["ValueFlow"]);

        auto inmsg_cs = vm::load_cell_slice_ref(extra.in_msg_descr);
        auto outmsg_cs = vm::load_cell_slice_ref(extra.out_msg_descr);

        auto in_msg_dict =
            std::make_unique<vm::AugmentedDictionary>(std::move(inmsg_cs), 256, block::tlb::aug_InMsgDescr);
        auto out_msg_dict =
            std::make_unique<vm::AugmentedDictionary>(std::move(outmsg_cs), 256, block::tlb::aug_OutMsgDescr);
        auto account_blocks_dict = std::make_unique<vm::AugmentedDictionary>(
            vm::load_cell_slice_ref(extra.account_blocks), 256, block::tlb::aug_ShardAccountBlocks);

        account_blocks_dict->check_for_each_extra(
            [&account_blocks_dict, &workchain, &now](const Ref<vm::CellSlice> &value, Ref<vm::CellSlice> extra,
                                                     td::ConstBitPtr key, int key_len) {
              CHECK(key_len == 256);
              const StdSmcAddress &acc_addr = key;
              LOG(DEBUG) << "Account " << acc_addr;
              auto dict_entry = account_blocks_dict->lookup_extra(key, 256);

              auto account = std::move(dict_entry.first);
              block::gen::CurrencyCollection::Record account_cc;
              CHECK(tlb::unpack(extra.write(), account_cc))
              LOG(DEBUG) << "Grams: " << account_cc.grams;

              json j_list(parse_extra_currency(account_cc.other->get_base_cell()));
              LOG(DEBUG) << "Other: " << to_string(j_list);

              block::gen::AccountBlock::Record acc_blk;
              CHECK(tlb::csr_unpack(value, acc_blk) && acc_blk.account_addr == acc_addr);
              vm::AugmentedDictionary trans_dict{vm::DictNonEmpty(), std::move(acc_blk.transactions), 64,
                                                 block::tlb::aug_AccountTransactions};

              td::BitArray<64> min_trans, max_trans;
              CHECK(trans_dict.get_minmax_key(min_trans).not_null() &&
                    trans_dict.get_minmax_key(max_trans, true).not_null());
              ton::LogicalTime min_trans_lt = min_trans.to_ulong(), max_trans_lt = max_trans.to_ulong();

              LOG(DEBUG) << "min_trans_lt " << min_trans_lt << " max_trans_lt " << max_trans_lt;

              //              auto trans_parser = [min_trans_lt, max_trans_lt](Ref<vm::CellSlice> value, Ref<vm::CellSlice> extra,
              //                                                                     td::ConstBitPtr key, int key_len) {
              //                CHECK(key_len == 64);
              //                ton::LogicalTime lt = key.get_uint(64);
              //                extra.clear();
              //                return;
              //              };
              //              trans_dict.check_for_each_extra(trans_parser);

              return false;
            });

        answer["BlockExtra"] = {
            {"rand_seed", extra.rand_seed.to_hex()},
            {"created_by", extra.created_by.to_hex()},
        };

        LOG(DEBUG) << "BlockExtra: " << to_string(answer["BlockExtra"]);

        vm::CellSlice upd_cs{vm::NoVmSpec(), blk.state_update};
        if (!(upd_cs.is_special() && upd_cs.prefetch_long(8) == 4  // merkle update
              && upd_cs.size_ext() == 0x20228)) {
          LOG(ERROR) << "invalid Merkle update in block";
          return;
        }

        auto state_old_hash = upd_cs.prefetch_ref(0)->get_hash(0).to_hex();
        auto state_hash = upd_cs.prefetch_ref(1)->get_hash(0).to_hex();

        answer["ShardState"] = {{"state_old_hash", state_old_hash}, {"state_hash", state_hash}};

        LOG(DEBUG) << "ShardState: " << to_string(answer["ShardState"]);
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