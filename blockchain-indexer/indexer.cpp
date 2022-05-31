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
#include "td/utils/base64.h"
#include "td/utils/OptionParser.h"
#include "td/utils/port/user.h"
#include <utility>
#include <fstream>
#include "auto/tl/lite_api.h"
#include "adnl/utils.hpp"
#include "shard.hpp"
#include "validator-set.hpp"
#include "json.hpp"
#include "tuple"
#include "vm/boc.h"

// TODO: use td/utils/json
// TODO: use tlb auto deserializer to json
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

std::map<std::string, std::variant<int, std::string>> parse_anycast(vm::CellSlice anycast) {
  block::gen::Anycast::Record anycast_parsed;
  CHECK(tlb::unpack(anycast, anycast_parsed));

  return {{"depth", anycast_parsed.depth}, {"rewrite_pfx", anycast_parsed.rewrite_pfx->to_binary()}};
};

std::string dump_as_boc(Ref<vm::Cell> root_cell) {
  vm::BagOfCells boc;
  boc.set_root(std::move(root_cell));
  boc.import_cells().ensure();
  auto res = boc.serialize_to_string(31);
  LOG(DEBUG) << res;
  return res;
}

json parse_address(vm::CellSlice address) {
  json answer;

  // addr_none$00
  // addr_extern$01
  // addr_std$10
  // addr_var$11
  auto tag = (int)address.prefetch_ulong(2);

  if (tag == 0) {
    answer["type"] = "addr_none";
  } else if (tag == 1) {
    block::gen::MsgAddressExt::Record_addr_extern src_addr;
    CHECK(tlb::unpack(address, src_addr));
    answer["type"] = "addr_extern";
    answer["address"] = {{"len", src_addr.len},
                         {"address", src_addr.external_address->to_binary()},
                         {"address_hex", src_addr.len % 8 == 0 ? src_addr.external_address->to_hex() : ""}};
  } else if (tag == 2) {
    block::gen::MsgAddressInt::Record_addr_std dest_addr;
    CHECK(tlb::unpack(address, dest_addr));

    // TODO: create separated function
    std::map<std::string, std::variant<int, std::string>> anycast_prased;

    auto anycast = dest_addr.anycast.write();
    if ((int)anycast.prefetch_ulong(1) == 1) {  // Maybe Anycast
      anycast_prased = parse_anycast(anycast);
    } else {
      anycast_prased = {
          {"depth", 0},
          {"rewrite_pfx", ""},
      };
    }

    answer["type"] = "addr_std";
    answer["address"] = {
        {"anycast",
         {
             "depth",
             std::get<int>(anycast_prased["depth"]),
             "rewrite_pfx",
             std::get<std::string>(anycast_prased["rewrite_pfx"]),
         }},
        {"workchain_id", dest_addr.workchain_id},
        {"address_hex", dest_addr.address.to_hex()},
    };
  } else if (tag == 3) {
    block::gen::MsgAddressInt::Record_addr_var dest_addr;
    CHECK(tlb::unpack(address, dest_addr));

    // TODO: create separated function
    std::map<std::string, std::variant<int, std::string>> anycast_prased;
    auto anycast = dest_addr.anycast.write();
    if ((int)anycast.prefetch_ulong(1) == 1) {  // Maybe Anycast
      anycast_prased = parse_anycast(anycast);
    } else {
      anycast_prased = {
          {"depth", 0},
          {"rewrite_pfx", ""},
      };
    }

    answer["type"] = "addr_var";
    answer["address"] = {{"anycast",
                          {
                              "depth",
                              std::get<int>(anycast_prased["depth"]),
                              "rewrite_pfx",
                              std::get<std::string>(anycast_prased["rewrite_pfx"]),
                          }},
                         {"workchain_id", dest_addr.workchain_id},
                         {"addr_len", dest_addr.addr_len},
                         {"address", dest_addr.address->to_binary()},
                         {"address_hex", dest_addr.addr_len % 8 == 0 ? dest_addr.address->to_hex() : ""}};
  }

  return answer;
}

json parse_out_msg_descr(const vm::CellSlice &out_msg) {
  json answer;

  auto tag = block::gen::t_OutMsg.check_tag(out_msg);
  LOG(DEBUG) << "Tag: " << tag;

  if (tag == block::gen::t_OutMsg.msg_export_ext) {
    answer["type"] = "msg_export_ext";
  }

  else if (tag == block::gen::t_OutMsg.msg_export_imm) {
    answer["type"] = "msg_export_imm";
  }

  else if (tag == block::gen::t_OutMsg.msg_export_new) {
    answer["type"] = "msg_export_new";
  }

  else if (tag == block::gen::t_OutMsg.msg_export_tr) {
    answer["type"] = "msg_export_tr";
  }

  else if (tag == block::gen::t_OutMsg.msg_export_deq) {
    answer["type"] = "msg_export_deq";
  }

  else if (tag == block::gen::t_OutMsg.msg_export_deq_short) {
    answer["type"] = "msg_export_deq_short";
  }

  else if (tag == block::gen::t_OutMsg.msg_export_tr_req) {
    answer["type"] = "msg_export_tr_req";
  }

  else if (tag == block::gen::t_OutMsg.msg_export_deq_imm) {
    answer["type"] = "msg_export_deq_imm";
  }

  else {
    answer["type"] = "undefined";
  }

  return answer;
}

json parse_state_init(vm::CellSlice state_init) {
  json answer;

  block::gen::StateInit::Record state_init_parsed;
  CHECK(tlb::unpack(state_init, state_init_parsed));

  if ((int)state_init_parsed.split_depth->prefetch_ulong(1) == 1) {
    auto sd = state_init_parsed.split_depth.write();
    sd.skip_first(1);
    answer["split_depth"] = (int)sd.prefetch_ulong(5);
  }

  if ((int)state_init_parsed.special->prefetch_ulong(1) == 1) {
    auto s = state_init_parsed.special.write();
    s.skip_first(1);

    block::gen::TickTock::Record tiktok{};
    CHECK(tlb::unpack(s, tiktok));

    answer["special"] = {
        {"tick", tiktok.tick},
        {"tock", tiktok.tock},
    };
  }

  if ((int)state_init_parsed.code->prefetch_ulong(1) == 1) {
    auto code = state_init_parsed.code->prefetch_ref();

    answer["code"] = dump_as_boc(code);
  }

  if ((int)state_init_parsed.data->prefetch_ulong(1) == 1) {
    auto data = state_init_parsed.data->prefetch_ref();

    answer["data"] = dump_as_boc(data);
  }

  if ((int)state_init_parsed.library->prefetch_ulong(1) == 1) {  // if not empty
    auto libraries = vm::Dictionary{state_init_parsed.library->prefetch_ref(), 256};

    std::list<json> libs;

    while (!libraries.is_empty()) {
      td::BitArray<256> key{};
      libraries.get_minmax_key(key);

      auto lib = load_cell_slice(libraries.lookup_delete_ref(key));
      auto code = lib.prefetch_ref();

      json lib_json = {
          {"hash", key.to_hex()},
          {"public", (bool)lib.prefetch_ulong(1)},
          {"root", dump_as_boc(code)},
      };

      libs.push_back(lib_json);
      //      out_msgs_list.push_back(parse_message(o_msg));
    }

    answer["libs"] = libs;
  }

  return answer;
}

json parse_message(Ref<vm::Cell> message_any) {
  // int_msg_info$0
  // ext_in_msg_info$10
  // ext_out_msg_info$11

  json answer;

  block::gen::Message::Record in_message;
  block::gen::CommonMsgInfo::Record_int_msg_info info;

  CHECK(tlb::type_unpack_cell(std::move(message_any), block::gen::t_Message_Any, in_message));

  auto in_msg_info = in_message.info.write();
  // Get in msg type
  int tag = block::gen::t_CommonMsgInfo.get_tag(in_msg_info);

  if (tag == block::gen::CommonMsgInfo::int_msg_info) {
    block::gen::CommonMsgInfo::Record_int_msg_info msg;
    CHECK(tlb::unpack(in_msg_info, msg));
    answer["type"] = "int_msg_info";
    // Get dest
    auto dest = parse_address(msg.dest.write());
    answer["dest"] = dest;

    // Get src
    auto src = msg.src.write();
    answer["src"] = parse_address(src);

    answer["ihr_disabled"] = msg.ihr_disabled;
    answer["bounce"] = msg.bounce;
    answer["bounced"] = msg.bounced;

    block::gen::CurrencyCollection::Record value_cc;
    // TODO: separate function
    CHECK(tlb::unpack(msg.value.write(), value_cc))

    std::list<std::tuple<int, std::string>> dummy;
    answer["value"] = {
        {"grams", block::tlb::t_Grams.as_integer(value_cc.grams)->to_dec_string()},
        {"extra", value_cc.other->have_refs() ? parse_extra_currency(value_cc.other->prefetch_ref()) : dummy}};

    answer["ihr_fee"] = block::tlb::t_Grams.as_integer(msg.ihr_fee.write())->to_dec_string();
    answer["fwd_fee"] = block::tlb::t_Grams.as_integer(msg.fwd_fee.write())->to_dec_string();
    answer["created_lt"] = msg.created_lt;
    answer["created_at"] = msg.created_at;
  } else if (tag == block::gen::CommonMsgInfo::ext_in_msg_info) {
    answer["type"] = "ext_in_msg_info";
    block::gen::CommonMsgInfo::Record_ext_in_msg_info msg;
    CHECK(tlb::unpack(in_msg_info, msg));

    // Get src
    auto src = msg.src.write();
    answer["src"] = parse_address(src);

    // Get dest
    auto dest = msg.dest.write();
    answer["dest"] = parse_address(dest);
    answer["import_fee"] = block::tlb::t_Grams.as_integer(msg.import_fee.write())->to_dec_string();
  } else if (tag == block::gen::CommonMsgInfo::ext_out_msg_info) {
    answer["type"] = "ext_out_msg_info";

    block::gen::CommonMsgInfo::Record_ext_out_msg_info msg;
    CHECK(tlb::unpack(in_msg_info, msg));

    answer["src"] = parse_address(msg.src.write());
    answer["dest"] = parse_address(msg.dest.write());
    answer["created_lt"] = msg.created_lt;
    answer["created_at"] = msg.created_at;
  } else {
    LOG(ERROR) << "Not covered";
    answer = {{"type", "Unknown"}};
  }

  // Parse init
  auto init = in_message.init.write();

  if ((int)init.prefetch_ulong(1) == 1) {
    init.skip_first(1);

    if (init.have_refs()) {
      auto init_root = init.prefetch_ref();
      answer["init"] = parse_state_init(load_cell_slice(init_root));
    } else {
      answer["init"] = parse_state_init(init);
    }
  }

  auto body = in_message.body.write();
  if ((int)body.prefetch_ulong(1) == 1) {  // Either
    answer["body"] = dump_as_boc(body.prefetch_ref());
  } else {
    vm::CellBuilder cb;
    cb.append_cellslice(body);
    auto body_cell = cb.finalize();

    answer["body"] = dump_as_boc(body_cell);
  }

  return answer;
}

std::string parse_type(char type) {
  if (type == block::gen::t_AccountStatus.acc_state_active) {
    return "active";
  } else if (type == block::gen::t_AccountStatus.acc_state_uninit) {
    return "uninit";
  } else if (type == block::gen::t_AccountStatus.acc_state_frozen) {
    return "frozen";
  } else if (type == block::gen::t_AccountStatus.acc_state_nonexist) {
    return "nonexist";
  } else {
    return "undefined";
  }
}

std::string parse_status_change(char type) {
  if (type == block::gen::t_AccStatusChange.acst_unchanged) {
    return "acst_unchanged";
  } else if (type == block::gen::t_AccStatusChange.acst_frozen) {
    return "acst_frozen";
  } else if (type == block::gen::t_AccStatusChange.acst_deleted) {
    return "acst_deleted";
  } else {
    return "undefined";
  }
}

json parse_storage_used_short(vm::CellSlice storage_used_short) {
  json answer;

  block::gen::StorageUsedShort::Record ss;
  CHECK(tlb::unpack(storage_used_short, ss));

  answer = {
      {"cells", block::tlb::t_VarUInteger_7.as_uint(ss.cells.write())},
      {"bits", block::tlb::t_VarUInteger_7.as_uint(ss.bits.write())},
  };

  return answer;
}

json parse_bounce_phase(vm::CellSlice bp) {
  json answer;
  auto tag = block::gen::t_TrBouncePhase.check_tag(bp);

  if (tag == block::gen::t_TrBouncePhase.tr_phase_bounce_negfunds) {
    answer["type"] = "negfunds";
  } else if (tag == block::gen::t_TrBouncePhase.tr_phase_bounce_nofunds) {
    block::gen::TrBouncePhase::Record_tr_phase_bounce_nofunds bn;
    CHECK(tlb::unpack(bp, bn));

    answer = {
        {"type", "nofunds"},
        {"msg_size", parse_storage_used_short(bn.msg_size.write())},
        {"req_fwd_fees", block::tlb::t_Grams.as_integer(bn.req_fwd_fees)->to_dec_string()},
    };
  } else if (tag == block::gen::t_TrBouncePhase.tr_phase_bounce_ok) {
    block::gen::TrBouncePhase::Record_tr_phase_bounce_ok bo;
    CHECK(tlb::unpack(bp, bo));

    answer = {{"type", "ok"},
              {"msg_size", parse_storage_used_short(bo.msg_size.write())},
              {"msg_fees", block::tlb::t_Grams.as_integer(bo.msg_fees)->to_dec_string()},
              {"fwd_fees", block::tlb::t_Grams.as_integer(bo.fwd_fees)->to_dec_string()}};
  }

  return answer;
}

json parse_transaction_descr(const Ref<vm::Cell> &transaction_descr) {
  json answer;
  auto trans_descr_cs = load_cell_slice(transaction_descr);
  auto tag = block::gen::t_TransactionDescr.get_tag(trans_descr_cs);

  if (tag == block::gen::t_TransactionDescr.trans_ord) {
    block::gen::TransactionDescr::Record_trans_ord parsed;
    CHECK(tlb::unpack_cell(transaction_descr, parsed));

    answer["type"] = "trans_ord";
    answer["credit_first"] = parsed.credit_first;
    answer["aborted"] = parsed.aborted;
    answer["destroyed"] = parsed.destroyed;

    if ((int)parsed.compute_ph->prefetch_ulong(1) == 0) {  // tr_phase_compute_skipped
      block::gen::TrComputePhase::Record_tr_phase_compute_skipped t;
      CHECK(tlb::unpack(parsed.compute_ph.write(), t));

      if (t.reason == block::gen::t_ComputeSkipReason.cskip_no_state) {
        answer["compute_ph"] = {{"type", "skipped"}, {"reason", "cskip_no_state"}};
      } else if (t.reason == block::gen::t_ComputeSkipReason.cskip_bad_state) {
        answer["compute_ph"] = {{"type", "skipped"}, {"reason", "cskip_bad_state"}};
      } else if (t.reason == block::gen::t_ComputeSkipReason.cskip_no_gas) {
        answer["compute_ph"] = {{"type", "skipped"}, {"reason", "cskip_no_gas"}};
      }

    } else {
      block::gen::TrComputePhase::Record_tr_phase_compute_vm t;
      CHECK(tlb::unpack(parsed.compute_ph.write(), t));

      answer["compute_ph"] = {
          {"success", t.success},
          {"msg_state_used", t.msg_state_used},
          {"account_activated", t.account_activated},
          {"gas_fees", block::tlb::t_Grams.as_integer(t.gas_fees)->to_dec_string()},
          {"gas_used", block::tlb::t_VarUInteger_7.as_uint(t.r1.gas_used.write())},
          {"gas_limit", block::tlb::t_VarUInteger_7.as_uint(t.r1.gas_limit.write())},
          {"mode", t.r1.mode},
          {"exit_code", t.r1.exit_code},
          {"vm_steps", t.r1.vm_steps},
          {"vm_init_state_hash", t.r1.vm_init_state_hash.to_hex()},
          {"vm_final_state_hash", t.r1.vm_final_state_hash.to_hex()},
      };

      if ((int)t.r1.gas_credit->prefetch_ulong(1) == 1) {
        auto cs = t.r1.gas_credit.write();
        cs.skip_first(1);
        answer["compute_ph"]["gas_credit"] = block::tlb::t_VarUInteger_3.as_uint(cs);
      }

      if ((int)t.r1.exit_arg->prefetch_ulong(1) == 1) {
        auto cs = t.r1.exit_arg.write();
        cs.skip_first(1);
        answer["compute_ph"]["exit_arg"] = (int)cs.prefetch_ulong(32);
      }
    }

    if ((int)parsed.storage_ph->prefetch_ulong(1) == 1) {  // Maybe TrStoragePhase
      auto storage_ph = parsed.storage_ph.write();
      storage_ph.skip_first(1);

      block::gen::TrStoragePhase::Record ts;
      CHECK(tlb::unpack(storage_ph, ts));

      answer["storage_ph"] = {
          {"storage_fees_collected", block::tlb::t_Grams.as_integer(ts.storage_fees_collected)->to_dec_string()},
          {"status_change", parse_status_change(ts.status_change)}};

      if ((int)ts.storage_fees_due->prefetch_ulong(1) == 1) {
        auto cs = ts.storage_fees_due.write();
        cs.skip_first(1);

        answer["storage_ph"]["storage_fees_due"] = block::tlb::t_Grams.as_integer(cs)->to_dec_string();
      }
    }

    if ((int)parsed.credit_ph->prefetch_ulong(1) == 1) {  // Maybe TrCreditPhase
      auto credit_ph_root = parsed.credit_ph.write();
      credit_ph_root.skip_first(1);

      block::gen::TrCreditPhase::Record credit_ph;
      CHECK(tlb::unpack(credit_ph_root, credit_ph));

      block::gen::CurrencyCollection::Record cc;
      CHECK(tlb::unpack(credit_ph.credit.write(), cc));

      std::list<std::tuple<int, std::string>> dummy;
      answer["credit_ph"] = {
          {"credit",
           {{"grams", block::tlb::t_Grams.as_integer(cc.grams)->to_dec_string()},
            {"extra", cc.other->have_refs() ? parse_extra_currency(cc.other->prefetch_ref()) : dummy}}}};

      if ((int)credit_ph.due_fees_collected->prefetch_ulong(1) == 1) {
        auto cs = credit_ph.due_fees_collected.write();
        cs.skip_first(1);

        answer["credit_ph"]["due_fees_collected"] = block::tlb::t_Grams.as_integer(cs)->to_dec_string();
      }
    }

    if ((int)parsed.action->prefetch_ulong(1) == 1) {  // Maybe ^TrActionPhase
      block::gen::TrActionPhase::Record action_ph;
      CHECK(parsed.action->have_refs());
      CHECK(tlb::unpack_cell(parsed.action->prefetch_ref(), action_ph));

      answer["action"] = {
          {"success", action_ph.success},
          {"valid", action_ph.valid},
          {"no_funds", action_ph.no_funds},
          {"no_funds", action_ph.no_funds},
          {"status_change", parse_status_change(action_ph.status_change)},
          {"result_code", action_ph.result_code},
          {"tot_actions", action_ph.tot_actions},
          {"spec_actions", action_ph.spec_actions},
          {"skipped_actions", action_ph.skipped_actions},
          {"msgs_created", action_ph.msgs_created},
          {"action_list_hash", action_ph.action_list_hash.to_hex()},
          {"tot_msg_size", parse_storage_used_short(action_ph.tot_msg_size.write())},
      };

      if ((int)action_ph.total_fwd_fees->prefetch_ulong(1) == 1) {
        auto tf = action_ph.total_fwd_fees.write();
        tf.skip_first(1);

        answer["action"]["total_fwd_fees"] = block::tlb::t_Grams.as_integer(tf)->to_dec_string();
      }

      if ((int)action_ph.total_action_fees->prefetch_ulong(1) == 1) {
        auto tf = action_ph.total_action_fees.write();
        tf.skip_first(1);

        answer["action"]["total_action_fees"] = block::tlb::t_Grams.as_integer(tf)->to_dec_string();
      }
      if (action_ph.result_code) {
        answer["action"]["result_code"] = action_ph.result_code;
      }
    }

    if ((int)parsed.bounce->prefetch_ulong(1) == 1) {  // Maybe TrBouncePhase
      auto bounce = parsed.bounce.write();
      bounce.skip_first(1);

      answer["bounce"] = parse_bounce_phase(bounce);
    }

  } else if (tag == block::gen::t_TransactionDescr.trans_storage) {
    answer["type"] = "trans_storage";
  } else if (tag == block::gen::t_TransactionDescr.trans_tick_tock) {
    answer["type"] = "trans_tick_tock";
  } else if (tag == block::gen::t_TransactionDescr.trans_split_prepare) {
    answer["type"] = "trans_split_prepare";
  } else if (tag == block::gen::t_TransactionDescr.trans_split_install) {
    answer["type"] = "trans_split_install";
  } else if (tag == block::gen::t_TransactionDescr.trans_merge_prepare) {
    answer["type"] = "trans_merge_prepare";
  } else if (tag == block::gen::t_TransactionDescr.trans_merge_install) {
    answer["type"] = "trans_merge_install";
  }

  return answer;
}

json parse_transaction(const Ref<vm::CellSlice> &tvalue, int workchain) {
  json transaction;
  block::gen::Transaction::Record trans;
  block::gen::HASH_UPDATE::Record hash_upd{};
  block::gen::CurrencyCollection::Record trans_total_fees_cc;

  CHECK(tvalue->have_refs());
  CHECK(tlb::unpack_cell(tvalue->prefetch_ref(), trans) &&
        tlb::type_unpack_cell(std::move(trans.state_update), block::gen::t_HASH_UPDATE_Account, hash_upd) &&
        tlb::unpack(trans.total_fees.write(), trans_total_fees_cc));

  std::list<std::tuple<int, std::string>> dummy;
  transaction["total_fees"] = {
      {"grams", block::tlb::t_Grams.as_integer(trans_total_fees_cc.grams)->to_dec_string()},
      {"extra", trans_total_fees_cc.other->have_refs() ? parse_extra_currency(trans_total_fees_cc.other->prefetch_ref())
                                                       : dummy}};

  transaction["account_addr"] = {{"workchain", workchain}, {"address", trans.account_addr.to_hex()}};
  transaction["lt"] = trans.lt;
  transaction["prev_trans_hash"] = trans.prev_trans_hash.to_hex();
  transaction["prev_trans_lt"] = trans.prev_trans_lt;
  transaction["now"] = trans.now;
  transaction["outmsg_cnt"] = trans.outmsg_cnt;
  transaction["state_update"] = {{"old_hash", hash_upd.old_hash.to_hex()}, {"new_hash", hash_upd.old_hash.to_hex()}};

  // Parse in msg
  if (trans.r1.in_msg->prefetch_ulong(1) == 1) {
    CHECK(trans.r1.in_msg->have_refs());

    auto message = trans.r1.in_msg->prefetch_ref();
    transaction["in_msg"] = parse_message(message);
  }

  auto out_msgs = vm::Dictionary{trans.r1.out_msgs, 15};

  std::list<json> out_msgs_list;

  while (!out_msgs.is_empty()) {
    td::BitArray<15> key{};
    out_msgs.get_minmax_key(key);

    auto o_msg = out_msgs.lookup_delete_ref(key);
    out_msgs_list.push_back(parse_message(o_msg));
  }

  transaction["out_msgs"] = out_msgs_list;
  transaction["orig_status"] = parse_type(trans.orig_status);
  transaction["end_status"] = parse_type(trans.end_status);
  transaction["description"] = parse_transaction_descr(trans.description);

  return transaction;
}

class Indexer : public td::actor::Actor {
 private:
  std::string db_root_ = "/mnt/ton/ton-node/db";
  std::string global_config_ /*; = db_root_ + "/global-config.json"*/;
  BlockSeqno seqno_first_ = 0;
  BlockSeqno seqno_last_ = 0;
  td::Ref<ton::validator::ValidatorManagerOptions> opts_;
  td::actor::ActorOwn<ton::validator::ValidatorManagerInterface> validator_manager_;

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

    LOG(DEBUG) << "Callback";
    auto P_cb = td::PromiseCreator::lambda([](td::Unit R) {});
    td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::install_callback,
                            std::make_unique<Callback>(actor_id(this)), std::move(P_cb));
    LOG(DEBUG) << "Callback installed";
  }

  void sync_complete(const BlockHandle &handle) {
    // i in [seqno_first_; seqno_last_]
    for (auto seqno = seqno_first_; seqno <= seqno_last_; ++seqno) {
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

      ton::AccountIdPrefixFull pfx{0, 0x8000000000000000};
      td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::get_block_by_seqno_from_db, pfx, seqno,
                              std::move(P));
    }
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

        block::gen::Block::Record blk;
        block::gen::BlockInfo::Record info;
        block::gen::BlockExtra::Record extra;

        CHECK(tlb::unpack_cell(block_root, blk) && tlb::unpack_cell(blk.extra, extra));
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
                               {"start_lt", start_lt},
                               {"end_lt", info.end_lt},
                               {"gen_validator_list_hash_short", info.gen_validator_list_hash_short},
                               {"gen_catchain_seqno", info.gen_catchain_seqno},
                               {"min_ref_mc_seqno", info.min_ref_mc_seqno},
                               {"prev_key_block_seqno", info.prev_key_block_seqno}};

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

            {"fees_imported",
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

        /* tlb
         block_extra in_msg_descr:^InMsgDescr
          out_msg_descr:^OutMsgDescr
          account_blocks:^ShardAccountBlocks
          rand_seed:bits256
          created_by:bits256
          custom:(Maybe ^McBlockExtra) = BlockExtra;
        */

        auto inmsg_cs = vm::load_cell_slice_ref(extra.in_msg_descr);
        auto outmsg_cs = vm::load_cell_slice_ref(extra.out_msg_descr);

        auto in_msg_dict =
            std::make_unique<vm::AugmentedDictionary>(std::move(inmsg_cs), 256, block::tlb::aug_InMsgDescr);
        auto out_msg_dict =
            std::make_unique<vm::AugmentedDictionary>(std::move(outmsg_cs), 256, block::tlb::aug_OutMsgDescr);
        auto account_blocks_dict = std::make_unique<vm::AugmentedDictionary>(
            vm::load_cell_slice_ref(extra.account_blocks), 256, block::tlb::aug_ShardAccountBlocks);

        //        std::list<json> out_msgs_json;
        //        while (!out_msg_dict->is_empty()) {
        //          td::Bits256 last_key;
        //
        //          account_blocks_dict->get_minmax_key(last_key);
        //          LOG(DEBUG) << "Parse out message " << last_key.to_hex();
        //          Ref<vm::CellSlice> data = account_blocks_dict->lookup_delete(last_key);
        //
        //          if (data.not_null()) {
        //            json parsed = {{"hash", last_key.to_hex()}, {"message", parse_out_msg_descr(data.write())}};
        //            out_msgs_json.push_back(parsed);
        //          }
        //        }

        LOG(DEBUG) << "Finish parse out msg descr";

        //        answer["out_msg_descr"] = out_msgs_json;

        /* tlb
           acc_trans#5 account_addr:bits256
             transactions:(HashmapAug 64 ^Transaction CurrencyCollection)
             state_update:^(HASH_UPDATE Account)
            = AccountBlock;

          _ (HashmapAugE 256 AccountBlock CurrencyCollection) = ShardAccountBlocks;
         */

        std::list<json> accounts;

        while (!account_blocks_dict->is_empty()) {
          td::Bits256 last_key;
          Ref<vm::CellSlice> data;

          account_blocks_dict->get_minmax_key(last_key);
          LOG(DEBUG) << "Parse account " << last_key.to_hex();
          data = account_blocks_dict->lookup_delete(last_key);

          json account_block_parsed;
          account_block_parsed["account_addr"] = {{"address", last_key.to_hex()}, {"workchain", workchain}};

          block::gen::AccountBlock::Record acc_blk;
          CHECK(tlb::csr_unpack(data, acc_blk));
          int count = 0;
          std::list<json> transactions;

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
            transactions.push_back(transaction);

            ++count;
          };

          account_block_parsed["transactions"] = transactions;
          account_block_parsed["transactions_count"] = count;
          accounts.push_back(account_block_parsed);
        }

        answer["BlockExtra"] = {
            {"accounts", accounts},
            {"rand_seed", extra.rand_seed.to_hex()},
            {"created_by", extra.created_by.to_hex()},
        };

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

        std::ofstream block_file;
        block_file.open("block_" + std::to_string(workchain) + ":" + std::to_string(blkid.seqno()) + ":" +
                        std::to_string(blkid.id.shard) + ".json");
        block_file << answer.dump(4);
        block_file.close();
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

  td::OptionParser p;
  p.set_description("blockchain indexer");
  p.add_option('h', "help", "prints_help", [&]() {
    char b[10240];
    td::StringBuilder sb(td::MutableSlice{b, 10000});
    sb << p;
    std::cout << sb.as_cslice().c_str();
    std::exit(2);
  });
  p.add_checked_option('u', "user", "change user", [&](td::Slice user) { return td::change_user(user.str()); });
  p.add_option('D', "db", "root for dbs", [&](td::Slice fname) {
    td::actor::send_closure(main, &ton::validator::Indexer::set_db_root, fname.str());
  });
  p.add_option('C', "config", "global config path", [&](td::Slice fname) {
    td::actor::send_closure(main, &ton::validator::Indexer::set_global_config_path, fname.str());
  });
  td::uint32 threads = 7;
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
  p.add_checked_option('s', "seqno", "seqno_first[:seqno_last]\tseqno range", [&](td::Slice arg) {
    auto pos = std::min(arg.find(':'), arg.size());
    TRY_RESULT(seqno_first, td::to_integer_safe<ton::BlockSeqno>(arg.substr(0, pos)));
    ++pos;
    if (pos >= arg.size()) {
      td::actor::send_closure(main, &ton::validator::Indexer::set_seqno_range, seqno_first, seqno_first);
      return td::Status::OK();
    }
    TRY_RESULT(seqno_last, td::to_integer_safe<ton::BlockSeqno>(arg.substr(pos, arg.size())));
    td::actor::send_closure(main, &ton::validator::Indexer::set_seqno_range, seqno_first, seqno_last);
    return td::Status::OK();
  });

  td::actor::set_debug(true);
  td::actor::Scheduler scheduler({threads});
  scheduler.run_in_context([&] { main = td::actor::create_actor<ton::validator::Indexer>("cool"); });
  scheduler.run_in_context([&] { p.run(argc, argv).ensure(); });
  scheduler.run_in_context([&] { td::actor::send_closure(main, &ton::validator::Indexer::run); });
  scheduler.run();
  return 0;
}
