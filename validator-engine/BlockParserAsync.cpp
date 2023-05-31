//
// Created by Andrey Tvorozhkov on 5/25/23.
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
#include "tuple"
#include "crypto/block/mc-config.h"
#include <algorithm>
#include <queue>
#include <chrono>
#include <thread>
#include "BlockParserAsync.hpp"
#include "blockchain-indexer/json.hpp"
#include "blockchain-indexer/json-utils.hpp"

namespace ton::validator {

class AsyncStateIndexer : public td::actor::Actor {
  td::unique_ptr<vm::AugmentedDictionary> accounts;
  std::vector<json> json_accounts;
  std::string block_id_string;
  td::Timer timer;
  BlockIdExt block_id;
  std::mutex accounts_mtx_;
  std::mutex accounts_count_mtx_;
  unsigned long total_accounts;
  json answer;
  td::Promise<std::string> final_promise;
  vm::Ref<vm::Cell> root_cell;
  td::optional<td::Ref<vm::Cell>> prev_root_cell;
  std::vector<std::pair<td::Bits256, int>> accounts_keys;
  bool with_prev_state;
  td::unique_ptr<vm::AugmentedDictionary> prev_accounts;

 public:
  AsyncStateIndexer(std::string block_id_string_, vm::Ref<vm::Cell> root_cell_,
                    td::optional<td::Ref<vm::Cell>> prev_root_cell_,
                    std::vector<std::pair<td::Bits256, int>> accounts_keys_, BlockIdExt block_id_,
                    td::Promise<std::string> final_promise_) {
    block_id = block_id_;
    block_id_string = std::move(block_id_string_);
    total_accounts = accounts_keys_.size();
    root_cell = std::move(root_cell_);
    with_prev_state = true;
    if (!prev_root_cell_) {
      with_prev_state = false;
    }

    prev_root_cell = std::move(prev_root_cell_);
    accounts_keys = std::move(accounts_keys_);
    final_promise = std::move(final_promise_);
  }

  void start_up() override {
    try {
      LOG(WARNING) << "Parse accounts states " << block_id_string << " " << timer;

      block::gen::ShardStateUnsplit::Record shard_state;
      CHECK(tlb::unpack_cell(std::move(root_cell), shard_state));

      std::vector<std::tuple<int, std::string>> dummy;

      block::gen::CurrencyCollection::Record total_balance_cc;
      block::gen::CurrencyCollection::Record total_validator_fees_cc;

      CHECK(tlb::unpack(shard_state.r1.total_balance.write(), total_balance_cc))
      CHECK(tlb::unpack(shard_state.r1.total_validator_fees.write(), total_validator_fees_cc))

      json total_balance = {
          {"grams", block::tlb::t_Grams.as_integer(total_balance_cc.grams)->to_dec_string()},
          {"extra",
           total_balance_cc.other->have_refs() ? parse_extra_currency(total_balance_cc.other->prefetch_ref()) : dummy}};

      json total_validator_fees = {
          {"grams", block::tlb::t_Grams.as_integer(total_validator_fees_cc.grams)->to_dec_string()},
          {"extra", total_balance_cc.other->have_refs()
                        ? parse_extra_currency(total_validator_fees_cc.other->prefetch_ref())
                        : dummy}};

      answer = {
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

      LOG(DEBUG) << "Parsed accounts shard state main info " << block_id_string << " " << timer;

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

      LOG(DEBUG) << "Parse accounts states libs " << block_id_string << " " << timer;

      accounts = td::make_unique<vm::AugmentedDictionary>(vm::load_cell_slice_ref(shard_state.accounts), 256,
                                                          block::tlb::aug_ShardAccounts);

      if (with_prev_state) {
        block::gen::ShardStateUnsplit::Record prev_shard_state;
        CHECK(tlb::unpack_cell(std::move(prev_root_cell.value()), prev_shard_state));

        prev_accounts = td::make_unique<vm::AugmentedDictionary>(vm::load_cell_slice_ref(prev_shard_state.accounts),
                                                                 256, block::tlb::aug_ShardAccounts);
      }
      if (accounts_keys.empty()) {
        td::actor::send_closure(actor_id(this), &AsyncStateIndexer::finalize);
      } else {
        LOG(DEBUG) << "Parse accounts states one by one " << block_id_string << " " << timer;

        for (const auto &account : accounts_keys) {
          td::actor::send_closure(actor_id(this), &AsyncStateIndexer::processAccount, account.first, account.second);
        }
      }
    } catch (std::exception &e) {
      LOG(ERROR) << e.what() << " state error: " << block_id_string;
    } catch (...) {
      LOG(ERROR) << " state error: " << block_id_string;
    }
  }

  void processAccount(td::Bits256 account, int tx_count) {
    try {
      std::vector<std::tuple<int, std::string>> dummy;

      auto value = accounts->lookup(account);
      LOG(DEBUG) << "Parse accounts states got account data " << account.to_hex() << " " << block_id_string << " "
                 << timer;

      if (value.not_null()) {
        block::gen::ShardAccount::Record sa;
        block::gen::CurrencyCollection::Record dbi_cc;
        CHECK(tlb::unpack(value.write(), sa));
        LOG(DEBUG) << "Parse accounts states account data parsed " << account.to_hex() << " " << block_id_string << " "
                   << timer;

        json data;

        if (with_prev_state && tx_count > 1) {
          auto prev_acc = prev_accounts->lookup(account.cbits(), 256);

          if (prev_acc.not_null() && (prev_acc->have_refs() || !prev_acc->empty())) {
            vm::CellBuilder b;
            b.append_cellslice(prev_acc);
            const auto prev_state_acc = td::base64_encode(std_boc_serialize(b.finalize(), 31).move_as_ok());
            data["prev_state"] = prev_state_acc;

            LOG(DEBUG) << "In account " << account.to_hex()
                       << " several transactions in one block save prev state for emulation";
          }
        }

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
          LOG(DEBUG) << "Parse accounts states account main info parsed " << account.to_hex() << " " << block_id_string
                     << " " << timer;

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

          LOG(DEBUG) << "Parse accounts states account storage parsed" << account.to_hex() << " " << block_id_string
                     << " " << timer;

          auto tag = block::gen::t_AccountState.get_tag(as.state.write());

          if (tag == block::gen::t_AccountState.account_uninit) {
            data["account"]["state"] = {{"type", "uninit"}};
          }

          else if (tag == block::gen::t_AccountState.account_active) {
            block::gen::AccountState::Record_account_active active_account;
            CHECK(tlb::unpack(as.state.write(), active_account));

            data["account"]["state"] = {{"type", "active"}, {"state_init", parse_state_init(active_account.x.write())}};

          }

          else if (tag == block::gen::t_AccountState.account_frozen) {
            block::gen::AccountState::Record_account_frozen f{};
            CHECK(tlb::unpack(as.state.write(), f))
            data["account"]["state"] = {{"type", "frozen"}, {"state_hash", f.state_hash.to_hex()}};
          }

          {
            std::lock_guard<std::mutex> lock(accounts_mtx_);
            json_accounts.emplace_back(data);
          }
        }
      }

      LOG(DEBUG) << "Parse accounts states account finally parsed " << account.to_hex() << " " << block_id_string << " "
                 << timer;
    } catch (std::exception &e) {
      LOG(ERROR) << e.what() << "account error " << account.to_hex();
    } catch (...) {
      LOG(ERROR) << "account error " << account.to_hex();
    }

    bool is_end;

    {
      std::lock_guard<std::mutex> lock(accounts_count_mtx_);
      total_accounts -= 1;
      LOG(DEBUG) << "Parse accounts for " << block_id_string << " left " << total_accounts;
      is_end = total_accounts == 0;
    }

    if (is_end) {
      td::actor::send_closure(actor_id(this), &AsyncStateIndexer::finalize);
    }
  }

  bool finalize() {
    answer["accounts"] = json_accounts;
    LOG(WARNING) << "Parse accounts states all accounts parsed " << block_id_string << " " << timer;

    std::string final_json;
    std::string final_id = std::to_string(block_id.id.workchain) + ":" + std::to_string(block_id.id.shard) + ":" +
                           std::to_string(block_id.id.seqno);

    try {
      LOG(DEBUG) << "received & parsed state from db " << block_id.to_str();

      json to_dump = {{"id", std::to_string(block_id.id.workchain) + ":" + std::to_string(block_id.id.shard) + ":" +
                                 std::to_string(block_id.id.seqno)},
                      {"data", answer}};

      final_json = to_dump.dump(-1);
      final_promise.set_value(std::move(final_json));
    } catch (...) {
      LOG(ERROR) << "Cant dump state: " << final_id;

      LOG(WARNING) << "Calling std::exit(0)";
      std::exit(0);
    }

    stop();
    return true;
  }
};

void BlockParserAsync::parseBlockData() {
  LOG(WARNING) << "Parse block data" << id.to_str();

  auto blkid = data->block_id();
  LOG(DEBUG) << "Parse: " << blkid.to_str();

  auto block_root = data->root_cell();
  if (block_root.is_null()) {
    LOG(ERROR) << "block has no valid root cell";
    std::abort();
  }

  //
  // Parsing

  json answer;
  answer["type"] = "block_data";
  answer["is_applied"] = handle->is_applied();

  auto workchain = blkid.id.workchain;

  answer["BlockIdExt"] = {{"file_hash", blkid.file_hash.to_hex()},
                          {"root_hash", blkid.root_hash.to_hex()},
                          {"id",
                           {
                               {"workchain", workchain},
                               {"seqno", blkid.id.seqno},
                               {"shard", blkid.id.shard},
                           }}};
  LOG(DEBUG) << "Parse: " << blkid.to_str() << " BlockIdExt success";
  block::gen::Block::Record blk;
  block::gen::BlockInfo::Record info;
  block::gen::BlockExtra::Record extra;

  if (!(tlb::unpack_cell(block_root, blk) && tlb::unpack_cell(blk.extra, extra) && tlb::unpack_cell(blk.info, info))) {
    LOG(FATAL) << "Error in block: " << blkid.to_str();
    std::abort();  // TODO:
                   //    return;
  }

  answer["global_id"] = blk.global_id;
  auto now = info.gen_utime;
  auto start_lt = info.start_lt;

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

  LOG(DEBUG) << "Parse: " << blkid.to_str() << " BlockInfo success";

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
  }

  LOG(DEBUG) << "Parse: " << blkid.to_str() << " BlockInfo prev_ref success";

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
    LOG(DEBUG) << "Parse: " << blkid.to_str() << " BlockInfo master_ref success";
  }

  if (info.gen_software.not_null()) {
    answer["BlockInfo"]["gen_software"] = {
        {"version", info.gen_software->prefetch_ulong(32)},
        {"capabilities", info.gen_software->prefetch_ulong(64)},
    };
    LOG(DEBUG) << "Parse: " << blkid.to_str() << " BlockInfo gen_software success";
  }

  auto value_flow_root = blk.value_flow;
  block::ValueFlow value_flow;
  vm::CellSlice cs{vm::NoVmOrd(), value_flow_root};
  if (!(cs.is_valid() && value_flow.fetch(cs) && cs.empty_ext())) {
    LOG(ERROR) << "cannot unpack ValueFlow of the new block ";
    std::abort();  // TODO:
                   //    return;
  }

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

  LOG(DEBUG) << "Parse: " << blkid.to_str() << " ValueFlow success";

  auto in_msg_dict = std::make_unique<vm::AugmentedDictionary>(vm::load_cell_slice_ref(extra.in_msg_descr), 256,
                                                               block::tlb::aug_InMsgDescr);

  std::vector<json> in_msgs_json;
  while (!in_msg_dict->is_empty()) {
    td::Bits256 last_key;

    in_msg_dict->get_minmax_key(last_key);
    Ref<vm::CellSlice> data = in_msg_dict->lookup_delete(last_key);

    json parsed = {{"hash", last_key.to_hex()}, {"message", parse_in_msg(data.write(), workchain)}};
    in_msgs_json.push_back(parsed);
  }

  LOG(DEBUG) << "Parse: " << blkid.to_str() << " in_msg_dict success";

  auto out_msg_dict = std::make_unique<vm::AugmentedDictionary>(vm::load_cell_slice_ref(extra.out_msg_descr), 256,
                                                                block::tlb::aug_OutMsgDescr);

  std::vector<json> out_msgs_json;
  while (!out_msg_dict->is_empty()) {
    td::Bits256 last_key;

    out_msg_dict->get_minmax_key(last_key);
    Ref<vm::CellSlice> data = out_msg_dict->lookup_delete(last_key);

    json parsed = {{"hash", last_key.to_hex()}, {"message", parse_out_msg(data.write(), workchain)}};
    out_msgs_json.push_back(parsed);
  }

  LOG(DEBUG) << "Parse: " << blkid.to_str() << " out_msg_dict success";

  auto account_blocks_dict = std::make_unique<vm::AugmentedDictionary>(vm::load_cell_slice_ref(extra.account_blocks),
                                                                       256, block::tlb::aug_ShardAccountBlocks);

  /* tlb
           acc_trans#5 account_addr:bits256
             transactions:(HashmapAug 64 ^Transaction CurrencyCollection)
             state_update:^(HASH_UPDATE Account)
            = AccountBlock;

          _ (HashmapAugE 256 AccountBlock CurrencyCollection) = ShardAccountBlocks;
         */

  std::vector<json> accounts;
  std::vector<std::pair<td::Bits256, int>> accounts_keys;

  while (!account_blocks_dict->is_empty()) {
    td::Bits256 last_key;
    Ref<vm::CellSlice> data;

    account_blocks_dict->get_minmax_key(last_key);
    auto hex_addr = last_key.to_hex();
    LOG(DEBUG) << "Parse: " << blkid.to_str() << " at " << hex_addr;

    data = account_blocks_dict->lookup_delete(last_key);

    json account_block_parsed;
    account_block_parsed["account_addr"] = {{"address", last_key.to_hex()}, {"workchain", workchain}};

    block::gen::AccountBlock::Record acc_blk;
    CHECK(tlb::csr_unpack(data, acc_blk));
    int count = 0;
    std::vector<json> transactions;

    vm::AugmentedDictionary trans_dict{vm::DictNonEmpty(), std::move(acc_blk.transactions), 64,
                                       block::tlb::aug_AccountTransactions};

    while (!trans_dict.is_empty()) {
      td::BitArray<64> last_lt{};
      trans_dict.get_minmax_key(last_lt);

      Ref<vm::CellSlice> tvalue;
      tvalue = trans_dict.lookup_delete(last_lt);

      json transaction = parse_transaction(tvalue, workchain);
      transactions.push_back(transaction);

      ++count;
    };

    accounts_keys.emplace_back(last_key, count);
    LOG(DEBUG) << "Parse: " << blkid.to_str() << " at " << hex_addr << " transactions success";

    account_block_parsed["transactions"] = transactions;
    account_block_parsed["transactions_count"] = count;
    accounts.push_back(account_block_parsed);
  }

  LOG(DEBUG) << "Parse: " << blkid.to_str() << " account_blocks_dict success";

  answer["BlockExtra"] = {
      {"accounts", accounts},
      {"rand_seed", extra.rand_seed.to_hex()},
      {"created_by", extra.created_by.to_hex()},
      {"out_msg_descr", out_msgs_json},
      {"in_msg_descr", in_msgs_json},
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

      answer["BlockExtra"]["custom"]["config_cell_hash"] = cp.config->get_hash().to_hex();
      answer["BlockExtra"]["custom"]["config_cell"] = dump_as_boc(cp.config);
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

    LOG(DEBUG) << "Parse: " << blkid.to_str() << " BlockExtra success";

    auto shard_fees_dict =
        std::make_unique<vm::AugmentedDictionary>(extra_mc.shard_fees, 96, block::tlb::aug_ShardFees);

    std::map<std::string, json> shard_fees;

    while (!shard_fees_dict->is_empty()) {
      td::BitArray<96> key{};
      shard_fees_dict->get_minmax_key(key);
      LOG(DEBUG) << "Parse: " << blkid.to_str() << " shard_fees_dict at " << key.to_hex();

      Ref<vm::CellSlice> tvalue;
      tvalue = shard_fees_dict->lookup_delete(key);

      block::gen::ShardFeeCreated::Record sf;
      CHECK(tlb::unpack(tvalue.write(), sf));

      block::gen::CurrencyCollection::Record fees;
      block::gen::CurrencyCollection::Record create;

      CHECK(tlb::unpack(sf.fees.write(), fees));
      CHECK(tlb::unpack(sf.create.write(), create));

      std::vector<std::tuple<int, std::string>> dummy;

      json data_fees = {
          {"fees",
           {{"grams", block::tlb::t_Grams.as_integer(fees.grams)->to_dec_string()},
            {"extra", fees.other->have_refs() ? parse_extra_currency(fees.other->prefetch_ref()) : dummy}}},

          {"create",
           {{"grams", block::tlb::t_Grams.as_integer(create.grams)->to_dec_string()},
            {"extra", create.other->have_refs() ? parse_extra_currency(create.other->prefetch_ref()) : dummy}}}};

      shard_fees[key.to_hex()] = data_fees;
      LOG(DEBUG) << "Parse: " << blkid.to_str() << " shard_fees_dict at " << key.to_hex() << " success";
    };

    answer["BlockExtra"]["custom"]["shard_fees"] = shard_fees;
    LOG(DEBUG) << "Parse: " << blkid.to_str() << " BlockExtra shard_fees success";

    if (extra_mc.r1.mint_msg->have_refs()) {
      answer["BlockExtra"]["custom"]["mint_msg"] =
          parse_in_msg(load_cell_slice(extra_mc.r1.mint_msg->prefetch_ref()), workchain);
    }
    LOG(DEBUG) << "Parse: " << blkid.to_str() << " BlockExtra mint_msg success";

    if (extra_mc.r1.recover_create_msg->have_refs()) {
      answer["BlockExtra"]["custom"]["recover_create_msg"] =
          parse_in_msg(load_cell_slice(extra_mc.r1.recover_create_msg->prefetch_ref()), workchain);
    }
    LOG(DEBUG) << "Parse: " << blkid.to_str() << " BlockExtra recover_create_msg success";

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

        prev_blk_signatures_json.push_back(data);
      };

      answer["BlockExtra"]["custom"]["prev_blk_signatures"] = prev_blk_signatures_json;
    };
    LOG(DEBUG) << "Parse: " << blkid.to_str() << " BlockExtra prev_blk_signatures success";

    block::ShardConfig shards;
    shards.unpack(extra_mc.shard_hashes);

    std::vector<json> shards_json;

    auto f = [&shards_json, &blkid](McShardHash &ms) {
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

      shards_json.push_back(data);
      return 1;
    };

    shards.process_shard_hashes(f);
    LOG(DEBUG) << "Parse: " << blkid.to_str() << " BlockExtra shards success";
    answer["BlockExtra"]["custom"]["shards"] = shards_json;
  }

  vm::CellSlice upd_cs{vm::NoVmSpec(), blk.state_update};
  if (!(upd_cs.is_special() && upd_cs.prefetch_long(8) == 4  // merkle update
        && upd_cs.size_ext() == 0x20228)) {
    LOG(ERROR) << "invalid Merkle update in block";
    std::abort();
  }

  CHECK(upd_cs.have_refs(2));
  auto state_old_hash = upd_cs.prefetch_ref(0)->get_hash(0).to_hex();
  auto state_hash = upd_cs.prefetch_ref(1)->get_hash(0).to_hex();

  answer["ShardState"] = {{"state_old_hash", state_old_hash}, {"state_hash", state_hash}};
  LOG(DEBUG) << "Parse: " << blkid.to_str() << " ShardState success";

  json to_dump = {
      {"id", std::to_string(workchain) + ":" + std::to_string(blkid.id.shard) + ":" + std::to_string(blkid.seqno())},
      {"data", answer}};

  parsed_data = to_dump.dump(-1);

  auto Pfinal = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<std::string> potential_state) {
    if (potential_state.is_ok()) {
      td::actor::send_closure(SelfId, &BlockParserAsync::saveStateData, potential_state.move_as_ok());
    }
    return 1;
  });

  const auto block_id_string =
      std::to_string(id.id.workchain) + ":" + std::to_string(id.id.shard) + ":" + std::to_string(id.id.seqno);

  //  AsyncStateIndexer(std::string block_id_string_, vm::Ref<vm::Cell> root_cell_,
  //                    td::optional<td::Ref<vm::Cell>> prev_root_cell_, std::vector<td::Bits256> accounts_keys_,
  //                    BlockIdExt block_id_, td::Promise<std::string> final_promise_) {

  td::actor::create_actor<AsyncStateIndexer>("AsyncStateIndexer", block_id_string, state, prev_state, accounts_keys, id,
                                             std::move(Pfinal))
      .release();

  LOG(WARNING) << "Parsed: " << blkid.to_str() << " success";
};

void BlockParserAsync::saveStateData(std::string tmp_state) {
  parsed_state = std::move(tmp_state);
  td::actor::send_closure(actor_id(this), &BlockParserAsync::finalize);
}

void BlockParserAsync::finalize() {
  LOG(WARNING) << "Send: " << id.to_str() << " success";
  P.set_value(std::make_tuple(parsed_data, parsed_state));
}

void StartupBlockParser::end_with_error(td::Status err) {
  P_final.set_error(std::move(err));
  stop();
}

void StartupBlockParser::receive_first_handle(std::shared_ptr<const BlockHandleInterface> handle) {
  LOG(WARNING) << "Receive first block for initial last blocks parse: " << handle->id().to_str();

  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), parsed_shards = &parsed_shards, handle](td::Result<td::Ref<BlockData>> R) {
        if (R.is_error()) {
          auto err = R.move_as_error();
          LOG(ERROR) << "failed query: " << err << " block: " << handle->id().to_str();
          td::actor::send_closure(SelfId, &StartupBlockParser::end_with_error, std::move(err));
        } else {
          auto block = R.move_as_ok();

          block::gen::Block::Record blk;
          block::gen::BlockExtra::Record extra;
          block::gen::McBlockExtra::Record mc_extra;
          if (!tlb::unpack_cell(block->root_cell(), blk) || !tlb::unpack_cell(blk.extra, extra) ||
              !extra.custom->have_refs() || !tlb::unpack_cell(extra.custom->prefetch_ref(), mc_extra)) {
            td::actor::send_closure(SelfId, &StartupBlockParser::end_with_error,
                                    td::Status::Error(-1, "cannot unpack header of block " + handle->id().to_str()));
          }
          block::ShardConfig shards(mc_extra.shard_hashes->prefetch_ref());

          auto parseShards = [parsed_shards = parsed_shards](McShardHash &ms) {
            parsed_shards->emplace_back(ms.top_block_id().to_str());
            return 1;
          };

          shards.process_shard_hashes(parseShards);

          td::actor::send_closure(SelfId, &StartupBlockParser::parse_other);
        }
      });

  td::actor::send_closure(manager, &ValidatorManagerInterface::get_block_data_from_db, handle, std::move(P));
}

void StartupBlockParser::receive_handle(std::shared_ptr<const BlockHandleInterface> handle) {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), handle](td::Result<td::Ref<BlockData>> R) {
    if (R.is_error()) {
      auto err = R.move_as_error();
      LOG(ERROR) << "failed query: " << err << " block: " << handle->id().to_str();
      td::actor::send_closure(SelfId, &StartupBlockParser::end_with_error, std::move(err));
    } else {
      auto block = R.move_as_ok();

      block::gen::Block::Record blk;
      block::gen::BlockExtra::Record extra;
      block::gen::McBlockExtra::Record mc_extra;
      if (!tlb::unpack_cell(block->root_cell(), blk) || !tlb::unpack_cell(blk.extra, extra) ||
          !extra.custom->have_refs() || !tlb::unpack_cell(extra.custom->prefetch_ref(), mc_extra)) {
        td::actor::send_closure(SelfId, &StartupBlockParser::end_with_error,
                                td::Status::Error(-1, "cannot unpack header of block " + handle->id().to_str()));
      }
      block::ShardConfig shards(mc_extra.shard_hashes->prefetch_ref());

      auto parseShards = [SelfId](McShardHash &ms) {
        const auto _id = ms.top_block_id().to_str();
        td::actor::send_closure_later(SelfId, &StartupBlockParser::parse_shard, ms.top_block_id());
        return 1;
      };

      shards.process_shard_hashes(parseShards);

      td::actor::send_closure(SelfId, &StartupBlockParser::receive_block, handle, std::move(block));
    }
  });

  block_handles.push_back(handle);
  td::actor::send_closure(manager, &ValidatorManagerInterface::get_block_data_from_db, handle, std::move(P));
}

void StartupBlockParser::receive_shard_handle(std::shared_ptr<const BlockHandleInterface> handle) {
  LOG(WARNING) << "Send get_block_data_from_db for shard";
  td::actor::send_closure(
      manager, &ValidatorManagerInterface::get_block_data_from_db, handle,
      td::PromiseCreator::lambda([SelfId = actor_id(this), handle](td::Result<td::Ref<BlockData>> R) {
        if (R.is_error()) {
          auto err = R.move_as_error();
          LOG(ERROR) << "failed query: " << err << " block: " << handle->id().to_str();
          td::actor::send_closure(SelfId, &StartupBlockParser::end_with_error, std::move(err));
        } else {
          auto block = R.move_as_ok();
          for (auto i : handle->prev()) {
            LOG(WARNING) << "Send find prev shard: " << i.to_str();
            td::actor::send_closure_later(SelfId, &StartupBlockParser::parse_shard, i);
          }

          td::actor::send_closure(SelfId, &StartupBlockParser::receive_block, handle, std::move(block));
        }
      }));
}

void StartupBlockParser::parse_shard(ton::BlockIdExt shard_id) {
  if (std::find(parsed_shards.begin(), parsed_shards.end(), shard_id.to_str()) == parsed_shards.end()) {
    td::actor::send_closure(actor_id(this), &StartupBlockParser::pad);

    LOG(WARNING) << "Receive parse shards: " << shard_id.to_str();
    ton::AccountIdPrefixFull pfx{shard_id.id.workchain, shard_id.id.shard};
    td::actor::send_closure(
        manager, &ValidatorManagerInterface::get_block_by_seqno_from_db, pfx, shard_id.seqno(),
        td::PromiseCreator::lambda([SelfId = actor_id(this), shard_id](td::Result<ConstBlockHandle> R) {
          if (R.is_error()) {
            auto err = R.move_as_error();
            LOG(ERROR) << "failed query: " << err << " block: " << shard_id.to_str();
            td::actor::send_closure(SelfId, &StartupBlockParser::end_with_error, std::move(err));
          } else {
            auto handle = R.move_as_ok();
            LOG(WARNING) << "Send receive_shard_handle";
            td::actor::send_closure(SelfId, &StartupBlockParser::receive_shard_handle, handle);
          }
        }));
  }
}

void StartupBlockParser::receive_block(std::shared_ptr<const BlockHandleInterface> handle, td::Ref<BlockData> block) {
  LOG(WARNING) << " send get shard state query for " << handle->id().to_str();
  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), handle, block = std::move(block)](td::Result<td::Ref<vm::DataCell>> R) mutable {
        if (R.is_error()) {
          auto err = R.move_as_error();
          LOG(ERROR) << err.to_string() << " state error";
          td::actor::send_closure(SelfId, &StartupBlockParser::end_with_error, std::move(err));
        } else {
          auto root_cell = R.move_as_ok();

          LOG(WARNING) << " send receive_states";
          td::actor::send_closure(SelfId, &StartupBlockParser::receive_states, handle, std::move(block),
                                  std::move(root_cell));
        }
      });

  td::actor::send_closure(manager, &ValidatorManagerInterface::get_shard_state_root_cell_from_db, handle, std::move(P));
  LOG(WARNING) << " sendEDEDEDE get shard state query for " << handle->id().to_str();
}

void StartupBlockParser::parse_other() {
  auto end = last_masterchain_block_handle->id().seqno();

  for (auto seqno = last_masterchain_block_handle->id().seqno() - k + 1; seqno != end + 1; ++seqno) {
    LOG(WARNING) << "Receive other MC blocks for initial last blocks parse: " << seqno;

    td::actor::send_closure(actor_id(this), &StartupBlockParser::pad);
    ton::AccountIdPrefixFull pfx{-1, 0x8000000000000000};
    td::actor::send_closure(
        manager, &ValidatorManagerInterface::get_block_by_seqno_from_db, pfx, seqno,
        td::PromiseCreator::lambda([SelfId = actor_id(this), seqno](td::Result<ConstBlockHandle> R) {
          if (R.is_error()) {
            auto err = R.move_as_error();
            LOG(ERROR) << "failed query: " << err << " MC seqno: " << seqno;
            td::actor::send_closure(SelfId, &StartupBlockParser::end_with_error, std::move(err));
          } else {
            auto handle = R.move_as_ok();
            td::actor::send_closure(SelfId, &StartupBlockParser::receive_handle, handle);
          }
        }));
  }
}

void StartupBlockParser::pad() {
  padding++;
}

void StartupBlockParser::ipad() {
  padding--;
  LOG(WARNING) << "To load: " << padding;

  if (padding == 0) {
    // TODO: fix
    LOG(WARNING) << "GOT: H: " << block_handles.size() << " B: " << blocks.size() << " S: " << states.size()
                 << " PS: " << block_handles.size();
  }
}

void StartupBlockParser::receive_states(ConstBlockHandle handle, td::Ref<BlockData> block, td::Ref<vm::Cell> state) {
  LOG(WARNING) << "Request prev state: " << handle->id().seqno();

  td::actor::send_closure(actor_id(this), &StartupBlockParser::pad);
  ton::AccountIdPrefixFull pfx{handle->id().id.workchain, handle->id().id.shard};
  td::actor::send_closure(
      manager, &ValidatorManagerInterface::get_block_by_seqno_from_db, pfx, handle->id().seqno() - 1,
      td::PromiseCreator::lambda([SelfId = actor_id(this), handle, block = std::move(block),
                                  state = std::move(state)](td::Result<ConstBlockHandle> R) mutable {
        if (R.is_error()) {
          auto err = R.move_as_error();
          LOG(ERROR) << "failed query with prev state";
          td::actor::send_closure(SelfId, &StartupBlockParser::end_with_error, std::move(err));
        } else {
          auto prev_handle = R.move_as_ok();

          td::actor::send_closure(SelfId, &StartupBlockParser::request_prev_state, handle, std::move(block),
                                  std::move(state), std::move(prev_handle));
        }
      }));
}

void StartupBlockParser::request_prev_state(ConstBlockHandle handle, td::Ref<BlockData> block, td::Ref<vm::Cell> state,
                                            std::shared_ptr<const BlockHandleInterface> prev_handle) {
  td::actor::send_closure(
      manager, &ValidatorManagerInterface::get_shard_state_root_cell_from_db, prev_handle,
      td::PromiseCreator::lambda([SelfId = actor_id(this), handle, block = std::move(block),
                                  state = std::move(state)](td::Result<td::Ref<vm::DataCell>> R) mutable {
        if (R.is_error()) {
          auto err = R.move_as_error();
          LOG(ERROR) << "failed to get prev shard state: ";
          td::actor::send_closure(SelfId, &StartupBlockParser::end_with_error, std::move(err));
        } else {
          auto prev_state = R.move_as_ok();
          td::actor::send_closure(SelfId, &StartupBlockParser::request_prev_state_final, handle, block,
                                  std::move(state), std::move(prev_state));
        }
      }));
}

void StartupBlockParser::request_prev_state_final(ConstBlockHandle handle, td::Ref<BlockData> block,
                                                  td::Ref<vm::Cell> state, td::Ref<vm::Cell> prev_state) {
  block_handles.push_back(std::move(handle));
  blocks.push_back(std::move(block));
  states.push_back(std::move(state));
  prev_states.push_back(std::move(prev_state));

  td::actor::send_closure(actor_id(this), &StartupBlockParser::ipad);
}

}  // namespace ton::validator