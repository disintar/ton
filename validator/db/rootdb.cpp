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
#include "rootdb.hpp"
#include "validator/fabric.h"
#include "archiver.hpp"

#include "ton/ton-tl.hpp"
#include "common/checksum.h"
#include "validator/stats-merger.h"
#include "td/actor/MultiPromise.h"


namespace ton {

namespace validator {

// TODO: use td/utils/json
// TODO: use tlb auto deserializer to json (PrettyPrintJson)
using json = nlohmann::json;
using td::Ref;


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
  return td::base64_encode(std_boc_serialize(std::move(root_cell), 31).move_as_ok());
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

json parse_libraries(Ref<vm::Cell> lib_cell) {
  auto libraries = vm::Dictionary{std::move(lib_cell), 256};

  std::list<json> libs;

  while (!libraries.is_empty()) {
    td::BitArray<256> key{};
    libraries.get_minmax_key(key);

    auto lib = libraries.lookup_delete(key);
    auto code = lib->prefetch_ref();

    json lib_json = {
        {"hash", key.to_hex()},
        {"public", (bool)lib->prefetch_ulong(1)},
        {"root", dump_as_boc(code)},
    };

    libs.push_back(lib_json);
    //      out_msgs_list.push_back(parse_message(o_msg));
  }

  return libs;
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

    answer["code_hash"] = code->get_hash().to_hex();
    answer["code"] = dump_as_boc(code);
  }

  if ((int)state_init_parsed.data->prefetch_ulong(1) == 1) {
    auto data = state_init_parsed.data->prefetch_ref();

    answer["data"] = dump_as_boc(data);
  }

  if ((int)state_init_parsed.library->prefetch_ulong(1) == 1) {  // if not empty
    answer["libs"] = parse_libraries(state_init_parsed.library->prefetch_ref());
  }

  return answer;
}

json parse_message(Ref<vm::Cell> message_any) {
  // int_msg_info$0
  // ext_in_msg_info$10
  // ext_out_msg_info$11

  json answer;
  answer["hash"] = message_any->get_hash().to_hex();

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

json parse_storage_ph(vm::CellSlice item) {
  json answer;

  block::gen::TrStoragePhase::Record ts;
  CHECK(tlb::unpack(item, ts));

  answer = {{"storage_fees_collected", block::tlb::t_Grams.as_integer(ts.storage_fees_collected)->to_dec_string()},
            {"status_change", parse_status_change(ts.status_change)}};

  if ((int)ts.storage_fees_due->prefetch_ulong(1) == 1) {
    auto cs = ts.storage_fees_due.write();
    cs.skip_first(1);

    answer["storage_fees_due"] = block::tlb::t_Grams.as_integer(cs)->to_dec_string();
  }

  return answer;
}

json parse_credit_ph(vm::CellSlice item) {
  json answer;
  block::gen::TrCreditPhase::Record credit_ph;
  CHECK(tlb::unpack(item, credit_ph));

  block::gen::CurrencyCollection::Record cc;
  CHECK(tlb::unpack(credit_ph.credit.write(), cc));

  std::list<std::tuple<int, std::string>> dummy;
  answer = {{"credit",
             {{"grams", block::tlb::t_Grams.as_integer(cc.grams)->to_dec_string()},
              {"extra", cc.other->have_refs() ? parse_extra_currency(cc.other->prefetch_ref()) : dummy}}}};

  if ((int)credit_ph.due_fees_collected->prefetch_ulong(1) == 1) {
    auto cs = credit_ph.due_fees_collected.write();
    cs.skip_first(1);

    answer["due_fees_collected"] = block::tlb::t_Grams.as_integer(cs)->to_dec_string();
  }

  return answer;
}

json parse_action_ph(const vm::CellSlice &item) {
  json answer;
  block::gen::TrActionPhase::Record action_ph;

  CHECK(tlb::unpack_cell(item.prefetch_ref(), action_ph));

  answer = {
      {"success", action_ph.success},
      {"valid", action_ph.valid},
      {"no_funds", action_ph.no_funds},
      {"status_change", parse_status_change(action_ph.status_change)},
      {"result_code", action_ph.result_code},
      {"tot_actions", action_ph.tot_actions},
      {"spec_actions", action_ph.spec_actions},
      {"skipped_actions", action_ph.skipped_actions},
      {"msgs_created", action_ph.msgs_created},
      {"action_list_hash", action_ph.action_list_hash.to_hex()},
      {"result_code", action_ph.result_code},
      {"tot_msg_size", parse_storage_used_short(action_ph.tot_msg_size.write())},
  };

  if ((int)action_ph.total_fwd_fees->prefetch_ulong(1) == 1) {
    auto tf = action_ph.total_fwd_fees.write();
    tf.skip_first(1);

    answer["total_fwd_fees"] = block::tlb::t_Grams.as_integer(tf)->to_dec_string();
  }

  if ((int)action_ph.total_action_fees->prefetch_ulong(1) == 1) {
    auto tf = action_ph.total_action_fees.write();
    tf.skip_first(1);

    answer["total_action_fees"] = block::tlb::t_Grams.as_integer(tf)->to_dec_string();
  }

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

json parse_compute_ph(vm::CellSlice item) {
  json answer;
  if (item.prefetch_ulong(1) == 0) {  // tr_phase_compute_skipped
    block::gen::TrComputePhase::Record_tr_phase_compute_skipped t{};
    CHECK(tlb::unpack(item, t));

    if (t.reason == block::gen::t_ComputeSkipReason.cskip_no_state) {
      answer = {{"type", "skipped"}, {"reason", "cskip_no_state"}};
    } else if (t.reason == block::gen::t_ComputeSkipReason.cskip_bad_state) {
      answer = {{"type", "skipped"}, {"reason", "cskip_bad_state"}};
    } else if (t.reason == block::gen::t_ComputeSkipReason.cskip_no_gas) {
      answer = {{"type", "skipped"}, {"reason", "cskip_no_gas"}};
    }

  } else {
    block::gen::TrComputePhase::Record_tr_phase_compute_vm t;
    CHECK(tlb::unpack(item, t));

    answer = {
        {"type", "computed"},
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
      answer["gas_credit"] = block::tlb::t_VarUInteger_3.as_uint(cs);
    }

    if ((int)t.r1.exit_arg->prefetch_ulong(1) == 1) {
      auto cs = t.r1.exit_arg.write();
      cs.skip_first(1);
      answer["exit_arg"] = (int)cs.prefetch_ulong(32);
    }
  }

  return answer;
}

json parse_split_prepare(vm::CellSlice item) {
  json answer;
  block::gen::SplitMergeInfo::Record splmi{};
  CHECK(tlb::unpack(item, splmi));

  answer = {
      {"cur_shard_pfx_len", splmi.cur_shard_pfx_len},
      {"acc_split_depth", splmi.acc_split_depth},
      {"this_addr", splmi.this_addr.to_hex()},
      {"sibling_addr", splmi.sibling_addr.to_hex()},
  };

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
    answer["compute_ph"] = parse_compute_ph(parsed.compute_ph.write());

    if ((int)parsed.storage_ph->prefetch_ulong(1) == 1) {  // Maybe TrStoragePhase
      auto storage_ph = parsed.storage_ph.write();
      storage_ph.skip_first(1);

      answer["storage_ph"] = parse_storage_ph(storage_ph);
    }

    if ((int)parsed.credit_ph->prefetch_ulong(1) == 1) {  // Maybe TrCreditPhase
      auto credit_ph_root = parsed.credit_ph.write();
      credit_ph_root.skip_first(1);
      answer["credit_ph"] = parse_credit_ph(credit_ph_root);
    }

    if ((int)parsed.action->prefetch_ulong(1) == 1) {  // Maybe ^TrActionPhase
      answer["action"] = parse_action_ph(parsed.action.write());
    }

    if ((int)parsed.bounce->prefetch_ulong(1) == 1) {  // Maybe TrBouncePhase
      auto bounce = parsed.bounce.write();
      bounce.skip_first(1);

      answer["bounce"] = parse_bounce_phase(bounce);
    }
  }

  else if (tag == block::gen::t_TransactionDescr.trans_storage) {
    block::gen::TransactionDescr::Record_trans_storage parsed;
    CHECK(tlb::unpack_cell(transaction_descr, parsed));

    answer["type"] = "trans_storage";
    answer["storage_ph"] = parse_storage_ph(parsed.storage_ph.write());
  }

  else if (tag == block::gen::t_TransactionDescr.trans_tick_tock) {
    block::gen::TransactionDescr::Record_trans_tick_tock parsed;
    CHECK(tlb::unpack_cell(transaction_descr, parsed));

    answer["type"] = "trans_tick_tock";
    answer["is_tock"] = parsed.is_tock;
    answer["aborted"] = parsed.aborted;
    answer["destroyed"] = parsed.destroyed;
    answer["compute_ph"] = parse_compute_ph(parsed.compute_ph.write());
    answer["storage_ph"] = parse_storage_ph(parsed.storage_ph.write());

    if ((int)parsed.action->prefetch_ulong(1) == 1) {  // Maybe ^TrActionPhase
      answer["action"] = parse_action_ph(parsed.action.write());
    }
  }

  else if (tag == block::gen::t_TransactionDescr.trans_split_prepare) {
    block::gen::TransactionDescr::Record_trans_split_prepare parsed;
    CHECK(tlb::unpack_cell(transaction_descr, parsed));
    answer["type"] = "trans_split_prepare";
    answer["aborted"] = parsed.aborted;
    answer["destroyed"] = parsed.destroyed;
    answer["compute_ph"] = parse_compute_ph(parsed.compute_ph.write());
    answer["split_info"] = parse_split_prepare(parsed.split_info.write());

    if ((int)parsed.storage_ph->prefetch_ulong(1) == 1) {  // Maybe TrStoragePhase
      auto storage_ph = parsed.storage_ph.write();
      storage_ph.skip_first(1);

      answer["storage_ph"] = parse_storage_ph(storage_ph);
    }

    if ((int)parsed.action->prefetch_ulong(1) == 1) {  // Maybe ^TrActionPhase
      answer["action"] = parse_action_ph(parsed.action.write());
    }
  }

  else if (tag == block::gen::t_TransactionDescr.trans_split_install) {
    block::gen::TransactionDescr::Record_trans_split_install parsed;
    CHECK(tlb::unpack_cell(transaction_descr, parsed));

    answer["type"] = "trans_split_install";
    answer["installed"] = parsed.installed;
    answer["split_info"] = parse_split_prepare(parsed.split_info.write());
    // todo: parse
    // answer["prepare_transaction"] = "need_to_be_parsed"

  }

  else if (tag == block::gen::t_TransactionDescr.trans_merge_prepare) {
    block::gen::TransactionDescr::Record_trans_merge_prepare parsed;
    CHECK(tlb::unpack_cell(transaction_descr, parsed));

    answer["type"] = "trans_merge_prepare";
    answer["aborted"] = parsed.aborted;
    answer["split_info"] = parse_split_prepare(parsed.split_info.write());
    answer["storage_ph"] = parse_storage_ph(parsed.storage_ph.write());

  }

  else if (tag == block::gen::t_TransactionDescr.trans_merge_install) {
    block::gen::TransactionDescr::Record_trans_merge_install parsed;
    CHECK(tlb::unpack_cell(transaction_descr, parsed));
    answer["type"] = "trans_merge_install";
    answer["aborted"] = parsed.aborted;
    answer["destroyed"] = parsed.destroyed;
    answer["split_info"] = parse_split_prepare(parsed.split_info.write());
    answer["compute_ph"] = parse_compute_ph(parsed.compute_ph.write());

    // todo: parse
    // answer["prepare_transaction"] = "need_to_be_parsed"

    if ((int)parsed.storage_ph->prefetch_ulong(1) == 1) {  // Maybe TrStoragePhase
      auto storage_ph = parsed.storage_ph.write();
      storage_ph.skip_first(1);

      answer["storage_ph"] = parse_storage_ph(storage_ph);
    }

    if ((int)parsed.credit_ph->prefetch_ulong(1) == 1) {  // Maybe TrCreditPhase
      auto credit_ph_root = parsed.credit_ph.write();
      credit_ph_root.skip_first(1);
      answer["credit_ph"] = parse_credit_ph(credit_ph_root);
    }

    if ((int)parsed.action->prefetch_ulong(1) == 1) {  // Maybe ^TrActionPhase
      answer["action"] = parse_action_ph(parsed.action.write());
    }
  }

  return answer;
}

json parse_transaction(const Ref<vm::CellSlice> &tvalue, int workchain) {
  json transaction;
  block::gen::Transaction::Record trans;
  block::gen::HASH_UPDATE::Record hash_upd{};
  block::gen::CurrencyCollection::Record trans_total_fees_cc;

  CHECK(tvalue->have_refs());
  auto trans_root = tvalue->prefetch_ref();
  CHECK(tlb::unpack_cell(trans_root, trans));
  CHECK(tlb::type_unpack_cell(std::move(trans.state_update), block::gen::t_HASH_UPDATE_Account, hash_upd));
  CHECK(tlb::unpack(trans.total_fees.write(), trans_total_fees_cc));

  std::list<std::tuple<int, std::string>> dummy;
  transaction["total_fees"] = {
      {"grams", block::tlb::t_Grams.as_integer(trans_total_fees_cc.grams)->to_dec_string()},
      {"extra", trans_total_fees_cc.other->have_refs() ? parse_extra_currency(trans_total_fees_cc.other->prefetch_ref())
                                                       : dummy}};

  transaction["account_addr"] = {{"workchain", workchain}, {"address", trans.account_addr.to_hex()}};
  transaction["lt"] = trans.lt;

  transaction["hash"] = trans_root->get_hash().to_hex();
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

json parse_in_msg_descr(vm::CellSlice in_msg, int workchain) {
  //  //
  //  msg_import_ext$000 msg:^(Message Any) transaction:^Transaction
  //                                                       = InMsg;
  //  msg_import_ihr$010 msg:^(Message Any) transaction:^Transaction
  //                                                           ihr_fee:Grams proof_created:^Cell = InMsg;
  //  msg_import_imm$011 in_msg:^MsgEnvelope
  //                                  transaction:^Transaction fwd_fee:Grams = InMsg;
  //  msg_import_fin$100 in_msg:^MsgEnvelope
  //                                  transaction:^Transaction fwd_fee:Grams = InMsg;
  //  msg_import_tr$101  in_msg:^MsgEnvelope out_msg:^MsgEnvelope
  //                                                        transit_fee:Grams = InMsg;
  //  msg_discard_fin$110 in_msg:^MsgEnvelope transaction_id:uint64
  //                                                                 fwd_fee:Grams = InMsg;
  //  msg_discard_tr$111 in_msg:^MsgEnvelope transaction_id:uint64
  //                                                                fwd_fee:Grams proof_delivered:^Cell = InMsg;
  //  //

  json answer;

  auto tag = block::gen::t_InMsg.check_tag(in_msg);

  if (tag == block::gen::t_InMsg.msg_import_ext) {
    answer["type"] = "msg_import_ext";
  }

  else if (tag == block::gen::t_InMsg.msg_import_ihr) {
    answer["type"] = "msg_import_ihr";
  }

  else if (tag == block::gen::t_InMsg.msg_import_imm) {
    answer["type"] = "msg_import_imm";
  }

  else if (tag == block::gen::t_InMsg.msg_import_fin) {
    answer["type"] = "msg_import_fin";
  }

  else if (tag == block::gen::t_InMsg.msg_import_tr) {
    answer["type"] = "msg_import_tr";
  }

  else if (tag == block::gen::t_InMsg.msg_discard_fin) {
    answer["type"] = "msg_discard_fin";
  }

  else if (tag == block::gen::t_InMsg.msg_discard_tr) {
    answer["type"] = "msg_discard_tr";
  }

  else {
    answer["type"] = "undefined";
  }

  return answer;
}

json parse_out_msg_descr(vm::CellSlice out_msg, int workchain) {  // TODO: parse

  //
  //  msg_import_ext$000 msg:^(Message Any) transaction:^Transaction
  //                                                       = InMsg;
  //  msg_import_ihr$010 msg:^(Message Any) transaction:^Transaction
  //                                                           ihr_fee:Grams proof_created:^Cell = InMsg;
  //  msg_import_imm$011 in_msg:^MsgEnvelope
  //                                  transaction:^Transaction fwd_fee:Grams = InMsg;
  //  msg_import_fin$100 in_msg:^MsgEnvelope
  //                                  transaction:^Transaction fwd_fee:Grams = InMsg;
  //  msg_import_tr$101  in_msg:^MsgEnvelope out_msg:^MsgEnvelope
  //                                                        transit_fee:Grams = InMsg;
  //  msg_discard_fin$110 in_msg:^MsgEnvelope transaction_id:uint64
  //                                                                 fwd_fee:Grams = InMsg;
  //  msg_discard_tr$111 in_msg:^MsgEnvelope transaction_id:uint64
  //                                                                fwd_fee:Grams proof_delivered:^Cell = InMsg;
  //

  json answer;

  auto tag = block::gen::t_OutMsg.check_tag(out_msg);

  if (tag == block::gen::t_OutMsg.msg_export_ext) {
    answer["type"] = "msg_export_ext";

    block::gen::OutMsg::Record_msg_export_ext data;
    CHECK(tlb::unpack(out_msg, data));

    vm::CellBuilder cb;  // TODO: fixme
    cb.store_ref(data.transaction);
    auto body_cell = cb.finalize();
    auto csr = load_cell_slice_ref(body_cell);

    answer["transaction"] = parse_transaction(csr, workchain);
  }

  else if (tag == block::gen::t_OutMsg.msg_export_imm) {
    answer["type"] = "msg_export_imm";

    block::gen::OutMsg::Record_msg_export_imm data;
    CHECK(tlb::unpack(out_msg, data));

    auto t = load_cell_slice_ref(data.transaction);

    vm::CellBuilder cb;  // TODO: fixme
    cb.store_ref(data.transaction);
    auto body_cell = cb.finalize();
    auto csr = load_cell_slice_ref(body_cell);

    answer["transaction"] = parse_transaction(csr, workchain);
  }

  else if (tag == block::gen::t_OutMsg.msg_export_new) {
    answer["type"] = "msg_export_new";

    block::gen::OutMsg::Record_msg_export_new data;
    CHECK(tlb::unpack(out_msg, data));

    vm::CellBuilder cb;  // TODO: fixme
    cb.store_ref(data.transaction);
    auto body_cell = cb.finalize();
    auto csr = load_cell_slice_ref(body_cell);

    answer["transaction"] = parse_transaction(csr, workchain);
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

void BlockPublisher::storeBlockData(BlockHandle handle, td::Ref<BlockData> block) {
  LOG(WARNING) << "Store block: " << block->block_id().to_str();
  CHECK(block.not_null());

  auto blkid = block->block_id();
  //  LOG(DEBUG) << "Parse: " << blkid.to_str() << " is_first: " << is_first;

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

    //    if (info.not_master && !is_first) {
    //      LOG(DEBUG) << "FOR: " << blkid.to_str() << " first: " << is_first;
    //      LOG(DEBUG) << "GO: " << blkid.id.workchain << ":" << blkid.id.shard << ":" << prev_blk_1.seq_no;
    //      LOG(DEBUG) << "GO: " << blkid.id.workchain << ":" << blkid.id.shard << ":" << prev_blk_2.seq_no;
    //
    //      td::actor::send_closure(SelfId, &Indexer::start_parse_shards, prev_blk_1.seq_no, blkid.id.shard,
    //                              blkid.id.workchain, false);
    //
    //      td::actor::send_closure(SelfId, &Indexer::start_parse_shards, prev_blk_2.seq_no, blkid.id.shard,
    //                              blkid.id.workchain, false);
    //    }

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

    //    if (info.not_master && !is_first) {
    //      LOG(DEBUG) << "FOR: " << blkid.to_str();
    //      LOG(DEBUG) << "GO: " << blkid.id.workchain << ":" << blkid.id.shard << ":" << prev_blk.seq_no;
    //
    //      td::actor::send_closure(SelfId, &Indexer::start_parse_shards, prev_blk.seq_no, blkid.id.shard,
    //                              blkid.id.workchain, false);
    //    }
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

  std::list<json> in_msgs_json;
  while (!in_msg_dict->is_empty()) {
    td::Bits256 last_key;

    in_msg_dict->get_minmax_key(last_key);
    Ref<vm::CellSlice> data = in_msg_dict->lookup_delete(last_key);

    json parsed = {{"hash", last_key.to_hex()}, {"message", parse_in_msg_descr(data.write(), workchain)}};
    in_msgs_json.push_back(parsed);
  }

  auto out_msg_dict = std::make_unique<vm::AugmentedDictionary>(vm::load_cell_slice_ref(extra.out_msg_descr), 256,
                                                                block::tlb::aug_OutMsgDescr);

  std::list<json> out_msgs_json;
  while (!out_msg_dict->is_empty()) {
    td::Bits256 last_key;

    out_msg_dict->get_minmax_key(last_key);
    Ref<vm::CellSlice> data = out_msg_dict->lookup_delete(last_key);

    json parsed = {{"hash", last_key.to_hex()}, {"message", parse_out_msg_descr(data.write(), workchain)}};
    out_msgs_json.push_back(parsed);
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

  std::list<json> accounts;
  std::list<td::Bits256> accounts_keys;

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
      accounts_keys.push_back(last_key);
    }

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

  if (true/*accounts_keys.size() > 0*/) {
    std::lock_guard<std::mutex> lock(maps_mtx);
    const std::string key = std::to_string(workchain) + ":" + std::to_string(blkid.id.shard) + ":" +
                            std::to_string(blkid.seqno());
    auto state_iter = states_.find(key);
    if (state_iter == states_.end()) {
      accounts_keys_.insert({key, accounts_keys});
    } else {
      gotState(state_iter->second.first, state_iter->second.second, accounts_keys);
      states_.erase(state_iter);
    }
  }

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

    vm::Dictionary shard_fees_dict{extra_mc.shard_fees->prefetch_ref(), 96};
    std::map<std::string, json> shard_fees;

    while (!shard_fees_dict.is_empty()) {
      td::BitArray<96> key{};
      shard_fees_dict.get_minmax_key(key);

      Ref<vm::CellSlice> tvalue;
      tvalue = shard_fees_dict.lookup_delete(key);

      block::gen::ShardFeeCreated::Record sf;
      CHECK(tlb::unpack(tvalue.write(), sf));

      block::gen::CurrencyCollection::Record fees;
      block::gen::CurrencyCollection::Record create;

      CHECK(tlb::unpack(sf.fees.write(), fees));
      CHECK(tlb::unpack(sf.create.write(), create));

      std::list<std::tuple<int, std::string>> dummy;

      json data = {
          {"fees",
           {{"grams", block::tlb::t_Grams.as_integer(fees.grams)->to_dec_string()},
            {"extra", fees.other->have_refs() ? parse_extra_currency(fees.other->prefetch_ref()) : dummy}}},

          {"create",
           {{"grams", block::tlb::t_Grams.as_integer(create.grams)->to_dec_string()},
            {"extra", create.other->have_refs() ? parse_extra_currency(create.other->prefetch_ref()) : dummy}}}};

      shard_fees[key.to_hex()] = data;
    };

    answer["BlockExtra"]["custom"]["shard_fees"] = shard_fees;

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
      std::list<json> prev_blk_signatures_json;

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

    block::ShardConfig shards;
    shards.unpack(extra_mc.shard_hashes);

    std::list<json> shards_json;

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

      auto shard_seqno = ms.top_block_id().id.seqno;
      auto shard_shard = ms.top_block_id().id.shard;
      auto shard_workchain = ms.shard().workchain;

      //      LOG(DEBUG) << "FOR: " << blkid.to_str() << " first: " << is_first;
      //      LOG(DEBUG) << "GO: " << shard_workchain << ":" << shard_shard << ":" << shard_seqno;

      //      td::actor::send_closure(SelfId, &Indexer::start_parse_shards, shard_seqno, shard_shard, shard_workchain,
      //                              is_first);

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

  answer["filename"] = std::string("block_") + std::to_string(workchain) + ":" + std::to_string(blkid.id.shard) + ":" +
                       std::to_string(blkid.seqno());

  publishBlockData(answer.dump());
}

void BlockPublisher::gotState(BlockHandle handle, td::Ref<ShardState> state, std::list<td::Bits256> accounts_keys) {
  auto block_id = state->get_block_id();
  LOG(WARNING) << "Parse state: " << block_id.to_str();
  CHECK(state.not_null());

  auto root_cell = state->root_cell();

  block::gen::ShardStateUnsplit::Record shard_state;
  CHECK(tlb::unpack_cell(root_cell, shard_state));

  std::list<std::tuple<int, std::string>> dummy;

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

    std::list<json> libs;

    while (!libraries.is_empty()) {
      td::BitArray<256> key{};
      libraries.get_minmax_key(key);
      auto lib = libraries.lookup_delete(key);

      block::gen::LibDescr::Record libdescr;
      CHECK(tlb::unpack(lib.write(), libdescr));

      std::list<std::string> publishers;

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

        publishers.push_back(publisher.to_hex());
      }

      json data = {{"hash", key.to_hex()}, {"lib", dump_as_boc(libdescr.lib)}, {"publishers", publishers}};
      libs.push_back(data);
    }

    answer["libraries"] = libs;
  }

  vm::AugmentedDictionary accounts{vm::load_cell_slice_ref(shard_state.accounts), 256,
                                   block::tlb::aug_ShardAccounts};

  std::list<json> accounts_list;

  for (const auto &account : accounts_keys) {
    LOG(DEBUG) << "Parse " << account.to_hex();
    auto result = accounts.lookup_extra(account.cbits(), 256);
    auto value = result.first;
    auto extra = result.second;
    if (value.not_null()) {
      block::gen::ShardAccount::Record sa;
      block::gen::DepthBalanceInfo::Record dbi;
      block::gen::CurrencyCollection::Record dbi_cc;
      CHECK(tlb::unpack(value.write(), sa));
      CHECK(tlb::unpack(extra.write(), dbi));
      CHECK(tlb::unpack(dbi.balance.write(), dbi_cc));

      json data;
      data["balance"] = {
          {"split_depth", dbi.split_depth},
          {"grams", block::tlb::t_Grams.as_integer(dbi_cc.grams)->to_dec_string()},
          {"extra", dbi_cc.other->have_refs() ? parse_extra_currency(dbi_cc.other->prefetch_ref()) : dummy}};
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

      accounts_list.push_back(data);
    }
  }

  answer["accounts"] = accounts_list;

  answer["filename"] = std::string("state_") + std::to_string(block_id.id.workchain) + ":" + std::to_string(block_id.id.shard) +
                       ":" + std::to_string(block_id.id.seqno);
  publishBlockState(answer.dump());
}


void RootDb::store_block_data(BlockHandle handle, td::Ref<BlockData> block, td::Promise<td::Unit> promise) {
  publisher_.storeBlockData(handle, block);

  if (handle->received()) {
    promise.set_value(td::Unit());
    return;
  }

  auto P = td::PromiseCreator::lambda(
      [id = archive_db_.get(), handle, promise = std::move(promise)](td::Result<td::Unit> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
        } else {
          handle->set_received();
          td::actor::send_closure(id, &ArchiveManager::update_handle, std::move(handle), std::move(promise));
        }
      });

  td::actor::send_closure(archive_db_, &ArchiveManager::add_file, handle, fileref::Block{handle->id()}, block->data(),
                          std::move(P));
}

void RootDb::get_block_data(ConstBlockHandle handle, td::Promise<td::Ref<BlockData>> promise) {
  if (!handle->received()) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "not in db"));
  } else {
    auto P = td::PromiseCreator::lambda(
        [id = handle->id(), promise = std::move(promise)](td::Result<td::BufferSlice> R) mutable {
          if (R.is_error()) {
            promise.set_error(R.move_as_error());
          } else {
            promise.set_result(create_block(id, R.move_as_ok()));
          }
        });

    td::actor::send_closure(archive_db_, &ArchiveManager::get_file, handle, fileref::Block{handle->id()}, std::move(P));
  }
}

void RootDb::store_block_signatures(BlockHandle handle, td::Ref<BlockSignatureSet> data,
                                    td::Promise<td::Unit> promise) {
  if (handle->inited_signatures() || handle->moved_to_archive()) {
    promise.set_value(td::Unit());
    return;
  }

  auto P = td::PromiseCreator::lambda(
      [id = archive_db_.get(), handle, promise = std::move(promise)](td::Result<td::Unit> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
        } else {
          handle->set_signatures();
          td::actor::send_closure(id, &ArchiveManager::update_handle, std::move(handle), std::move(promise));
        }
      });
  td::actor::send_closure(archive_db_, &ArchiveManager::add_temp_file_short, fileref::Signatures{handle->id()},
                          data->serialize(), std::move(P));
}

void RootDb::get_block_signatures(ConstBlockHandle handle, td::Promise<td::Ref<BlockSignatureSet>> promise) {
  if (!handle->inited_signatures() || handle->moved_to_archive()) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "not in db"));
  } else {
    auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<td::BufferSlice> R) mutable {
      if (R.is_error()) {
        promise.set_error(R.move_as_error());
      } else {
        promise.set_result(create_signature_set(R.move_as_ok()));
      }
    });
    td::actor::send_closure(archive_db_, &ArchiveManager::get_temp_file_short, fileref::Signatures{handle->id()},
                            std::move(P));
  }
}

void RootDb::store_block_proof(BlockHandle handle, td::Ref<Proof> proof, td::Promise<td::Unit> promise) {
  if (handle->inited_proof()) {
    promise.set_value(td::Unit());
    return;
  }

  auto P = td::PromiseCreator::lambda(
      [id = archive_db_.get(), handle, promise = std::move(promise)](td::Result<td::Unit> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
        } else {
          handle->set_proof();
          td::actor::send_closure(id, &ArchiveManager::update_handle, std::move(handle), std::move(promise));
        }
      });

  td::actor::send_closure(archive_db_, &ArchiveManager::add_file, handle, fileref::Proof{handle->id()}, proof->data(),
                          std::move(P));
}

void RootDb::get_block_proof(ConstBlockHandle handle, td::Promise<td::Ref<Proof>> promise) {
  if (!handle->inited_proof()) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "not in db"));
  } else {
    auto P = td::PromiseCreator::lambda(
        [id = handle->id(), promise = std::move(promise)](td::Result<td::BufferSlice> R) mutable {
          if (R.is_error()) {
            promise.set_error(R.move_as_error());
          } else {
            promise.set_result(create_proof(id, R.move_as_ok()));
          }
        });
    td::actor::send_closure(archive_db_, &ArchiveManager::get_file, handle, fileref::Proof{handle->id()}, std::move(P));
  }
}

void RootDb::store_block_proof_link(BlockHandle handle, td::Ref<ProofLink> proof, td::Promise<td::Unit> promise) {
  if (handle->inited_proof_link()) {
    promise.set_value(td::Unit());
    return;
  }

  auto P = td::PromiseCreator::lambda(
      [id = archive_db_.get(), handle, promise = std::move(promise)](td::Result<td::Unit> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
        } else {
          handle->set_proof_link();
          td::actor::send_closure(id, &ArchiveManager::update_handle, std::move(handle), std::move(promise));
        }
      });
  td::actor::send_closure(archive_db_, &ArchiveManager::add_file, handle, fileref::ProofLink{handle->id()},
                          proof->data(), std::move(P));
}

void RootDb::get_block_proof_link(ConstBlockHandle handle, td::Promise<td::Ref<ProofLink>> promise) {
  if (!handle->inited_proof_link()) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "not in db"));
  } else {
    auto P = td::PromiseCreator::lambda(
        [id = handle->id(), promise = std::move(promise)](td::Result<td::BufferSlice> R) mutable {
          if (R.is_error()) {
            promise.set_error(R.move_as_error());
          } else {
            promise.set_result(create_proof_link(id, R.move_as_ok()));
          }
        });
    td::actor::send_closure(archive_db_, &ArchiveManager::get_file, handle, fileref::ProofLink{handle->id()},
                            std::move(P));
  }
}

void RootDb::store_block_candidate(BlockCandidate candidate, td::Promise<td::Unit> promise) {
  auto obj = create_serialize_tl_object<ton_api::db_candidate>(
      PublicKey{pubkeys::Ed25519{candidate.pubkey.as_bits256()}}.tl(), create_tl_block_id(candidate.id),
      std::move(candidate.data), std::move(candidate.collated_data));

  auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<td::Unit> R) mutable {
    if (R.is_error()) {
      promise.set_error(R.move_as_error());
    } else {
      promise.set_value(td::Unit());
    }
  });
  td::actor::send_closure(archive_db_, &ArchiveManager::add_temp_file_short,
                          fileref::Candidate{PublicKey{pubkeys::Ed25519{candidate.pubkey.as_bits256()}}, candidate.id,
                                             candidate.collated_file_hash},
                          std::move(obj), std::move(P));
}

void RootDb::get_block_candidate(PublicKey source, BlockIdExt id, FileHash collated_data_file_hash,
                                 td::Promise<BlockCandidate> promise) {
  auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<td::BufferSlice> R) mutable {
    if (R.is_error()) {
      promise.set_error(R.move_as_error());
    } else {
      auto f = fetch_tl_object<ton_api::db_candidate>(R.move_as_ok(), true);
      f.ensure();
      auto val = f.move_as_ok();
      auto hash = sha256_bits256(val->collated_data_);

      auto key = ton::PublicKey{val->source_};
      auto e_key = Ed25519_PublicKey{key.ed25519_value().raw()};
      promise.set_value(BlockCandidate{e_key, create_block_id(val->id_), hash, std::move(val->data_),
                                       std::move(val->collated_data_)});
    }
  });
  td::actor::send_closure(archive_db_, &ArchiveManager::get_temp_file_short,
                          fileref::Candidate{source, id, collated_data_file_hash}, std::move(P));
}

void RootDb::store_block_state(BlockHandle handle, td::Ref<ShardState> state,
                               td::Promise<td::Ref<ShardState>> promise) {
  publisher_.storeBlockState(handle, state);
  if (handle->moved_to_archive()) {
    promise.set_value(std::move(state));
    return;
  }
  if (!handle->inited_state_boc()) {
    auto P = td::PromiseCreator::lambda([b = archive_db_.get(), root_hash = state->root_hash(), handle,
                                         promise = std::move(promise)](td::Result<td::Ref<vm::DataCell>> R) mutable {
      if (R.is_error()) {
        promise.set_error(R.move_as_error());
      } else {
        handle->set_state_root_hash(root_hash);
        handle->set_state_boc();
        auto S = create_shard_state(handle->id(), R.move_as_ok());
        S.ensure();
        auto P = td::PromiseCreator::lambda(
            [promise = std::move(promise), state = S.move_as_ok()](td::Result<td::Unit> R) mutable {
              R.ensure();
              promise.set_value(std::move(state));
            });
        td::actor::send_closure(b, &ArchiveManager::update_handle, std::move(handle), std::move(P));
      }
    });
    td::actor::send_closure(cell_db_, &CellDb::store_cell, handle->id(), state->root_cell(), std::move(P));
  } else {
    get_block_state(handle, std::move(promise));
  }
}

void RootDb::get_block_state(ConstBlockHandle handle, td::Promise<td::Ref<ShardState>> promise) {
  if (handle->inited_state_boc()) {
    if (handle->deleted_state_boc()) {
      promise.set_error(td::Status::Error(ErrorCode::error, "state already gc'd"));
      return;
    }
    auto P =
        td::PromiseCreator::lambda([handle, promise = std::move(promise)](td::Result<td::Ref<vm::DataCell>> R) mutable {
          if (R.is_error()) {
            promise.set_error(R.move_as_error());
          } else {
            auto S = create_shard_state(handle->id(), R.move_as_ok());
            S.ensure();
            promise.set_value(S.move_as_ok());
          }
        });
    td::actor::send_closure(cell_db_, &CellDb::load_cell, handle->state(), std::move(P));
  } else {
    promise.set_error(td::Status::Error(ErrorCode::notready, "state not in db"));
  }
}

void RootDb::store_persistent_state_file(BlockIdExt block_id, BlockIdExt masterchain_block_id, td::BufferSlice state,
                                         td::Promise<td::Unit> promise) {
  td::actor::send_closure(archive_db_, &ArchiveManager::add_persistent_state, block_id, masterchain_block_id,
                          std::move(state), std::move(promise));
}

void RootDb::store_persistent_state_file_gen(BlockIdExt block_id, BlockIdExt masterchain_block_id,
                                             std::function<td::Status(td::FileFd&)> write_data,
                                             td::Promise<td::Unit> promise) {
  td::actor::send_closure(archive_db_, &ArchiveManager::add_persistent_state_gen, block_id, masterchain_block_id,
                          std::move(write_data), std::move(promise));
}

void RootDb::get_persistent_state_file(BlockIdExt block_id, BlockIdExt masterchain_block_id,
                                       td::Promise<td::BufferSlice> promise) {
  td::actor::send_closure(archive_db_, &ArchiveManager::get_persistent_state, block_id, masterchain_block_id,
                          std::move(promise));
}

void RootDb::get_persistent_state_file_slice(BlockIdExt block_id, BlockIdExt masterchain_block_id, td::int64 offset,
                                             td::int64 max_size, td::Promise<td::BufferSlice> promise) {
  td::actor::send_closure(archive_db_, &ArchiveManager::get_persistent_state_slice, block_id, masterchain_block_id,
                          offset, max_size, std::move(promise));
}

void RootDb::check_persistent_state_file_exists(BlockIdExt block_id, BlockIdExt masterchain_block_id,
                                                td::Promise<bool> promise) {
  td::actor::send_closure(archive_db_, &ArchiveManager::check_persistent_state, block_id, masterchain_block_id,
                          std::move(promise));
}

void RootDb::store_zero_state_file(BlockIdExt block_id, td::BufferSlice state, td::Promise<td::Unit> promise) {
  td::actor::send_closure(archive_db_, &ArchiveManager::add_zero_state, block_id, std::move(state), std::move(promise));
}

void RootDb::get_zero_state_file(BlockIdExt block_id, td::Promise<td::BufferSlice> promise) {
  td::actor::send_closure(archive_db_, &ArchiveManager::get_zero_state, block_id, std::move(promise));
}

void RootDb::check_zero_state_file_exists(BlockIdExt block_id, td::Promise<bool> promise) {
  td::actor::send_closure(archive_db_, &ArchiveManager::check_zero_state, block_id, std::move(promise));
}

void RootDb::store_block_handle(BlockHandle handle, td::Promise<td::Unit> promise) {
  td::actor::send_closure(archive_db_, &ArchiveManager::update_handle, std::move(handle), std::move(promise));
}

void RootDb::get_block_handle(BlockIdExt id, td::Promise<BlockHandle> promise) {
  td::actor::send_closure(archive_db_, &ArchiveManager::get_handle, id, std::move(promise));
}

void RootDb::try_get_static_file(FileHash file_hash, td::Promise<td::BufferSlice> promise) {
  td::actor::send_closure(static_files_db_, &StaticFilesDb::load_file, file_hash, std::move(promise));
}

void RootDb::apply_block(BlockHandle handle, td::Promise<td::Unit> promise) {
  td::actor::create_actor<BlockArchiver>("archiver", std::move(handle), archive_db_.get(), std::move(promise))
      .release();
}

void RootDb::get_block_by_lt(AccountIdPrefixFull account, LogicalTime lt, td::Promise<ConstBlockHandle> promise) {
  td::actor::send_closure(archive_db_, &ArchiveManager::get_block_by_lt, account, lt, std::move(promise));
}

void RootDb::get_block_by_unix_time(AccountIdPrefixFull account, UnixTime ts, td::Promise<ConstBlockHandle> promise) {
  td::actor::send_closure(archive_db_, &ArchiveManager::get_block_by_unix_time, account, ts, std::move(promise));
}

void RootDb::get_block_by_seqno(AccountIdPrefixFull account, BlockSeqno seqno, td::Promise<ConstBlockHandle> promise) {
  td::actor::send_closure(archive_db_, &ArchiveManager::get_block_by_seqno, account, seqno, std::move(promise));
}

void RootDb::update_init_masterchain_block(BlockIdExt block, td::Promise<td::Unit> promise) {
  td::actor::send_closure(state_db_, &StateDb::update_init_masterchain_block, block, std::move(promise));
}

void RootDb::get_init_masterchain_block(td::Promise<BlockIdExt> promise) {
  td::actor::send_closure(state_db_, &StateDb::get_init_masterchain_block, std::move(promise));
}

void RootDb::update_gc_masterchain_block(BlockIdExt block, td::Promise<td::Unit> promise) {
  td::actor::send_closure(state_db_, &StateDb::update_gc_masterchain_block, block, std::move(promise));
}

void RootDb::get_gc_masterchain_block(td::Promise<BlockIdExt> promise) {
  td::actor::send_closure(state_db_, &StateDb::get_gc_masterchain_block, std::move(promise));
}

void RootDb::update_shard_client_state(BlockIdExt masterchain_block_id, td::Promise<td::Unit> promise) {
  td::actor::send_closure(state_db_, &StateDb::update_shard_client_state, masterchain_block_id, std::move(promise));
}

void RootDb::get_shard_client_state(td::Promise<BlockIdExt> promise) {
  td::actor::send_closure(state_db_, &StateDb::get_shard_client_state, std::move(promise));
}

void RootDb::update_destroyed_validator_sessions(std::vector<ValidatorSessionId> sessions,
                                                 td::Promise<td::Unit> promise) {
  td::actor::send_closure(state_db_, &StateDb::update_destroyed_validator_sessions, std::move(sessions),
                          std::move(promise));
}

void RootDb::get_destroyed_validator_sessions(td::Promise<std::vector<ValidatorSessionId>> promise) {
  td::actor::send_closure(state_db_, &StateDb::get_destroyed_validator_sessions, std::move(promise));
}

void RootDb::update_async_serializer_state(AsyncSerializerState state, td::Promise<td::Unit> promise) {
  td::actor::send_closure(state_db_, &StateDb::update_async_serializer_state, std::move(state), std::move(promise));
}

void RootDb::get_async_serializer_state(td::Promise<AsyncSerializerState> promise) {
  td::actor::send_closure(state_db_, &StateDb::get_async_serializer_state, std::move(promise));
}

void RootDb::update_hardforks(std::vector<BlockIdExt> blocks, td::Promise<td::Unit> promise) {
  td::actor::send_closure(state_db_, &StateDb::update_hardforks, std::move(blocks), std::move(promise));
}

void RootDb::get_hardforks(td::Promise<std::vector<BlockIdExt>> promise) {
  td::actor::send_closure(state_db_, &StateDb::get_hardforks, std::move(promise));
}

void RootDb::start_up() {
  LOG(DEBUG) << "Start cell_db";
  cell_db_ = td::actor::create_actor<CellDb>("celldb", actor_id(this), root_path_ + "/celldb/", read_only_);

  LOG(DEBUG) << "Start state_db";
  state_db_ = td::actor::create_actor<StateDb>("statedb", actor_id(this), root_path_ + "/state/", read_only_);

  LOG(DEBUG) << "Start static_files_db";
  static_files_db_ =
      td::actor::create_actor<StaticFilesDb>("staticfilesdb", actor_id(this), root_path_ + "/static/", read_only_);

  LOG(DEBUG) << "Start archive_db";
  archive_db_ = td::actor::create_actor<ArchiveManager>("archive", actor_id(this), root_path_, read_only_);

  LOG(DEBUG) << "All dbs loaded";
}

void RootDb::archive(BlockHandle handle, td::Promise<td::Unit> promise) {
  td::actor::create_actor<BlockArchiver>("archiveblock", std::move(handle), archive_db_.get(), std::move(promise))
      .release();
}

void RootDb::allow_state_gc(BlockIdExt block_id, td::Promise<bool> promise) {
  td::actor::send_closure(validator_manager_, &ValidatorManager::allow_block_state_gc, block_id, std::move(promise));
}

void RootDb::allow_block_gc(BlockIdExt block_id, td::Promise<bool> promise) {
  td::actor::send_closure(validator_manager_, &ValidatorManager::allow_block_info_gc, block_id, std::move(promise));
}

void RootDb::prepare_stats(td::Promise<std::vector<std::pair<std::string, std::string>>> promise) {
  auto merger = StatsMerger::create(std::move(promise));
}

void RootDb::truncate(BlockSeqno seqno, ConstBlockHandle handle, td::Promise<td::Unit> promise) {
  td::MultiPromise mp;
  auto ig = mp.init_guard();
  ig.add_promise(std::move(promise));

  td::actor::send_closure(archive_db_, &ArchiveManager::truncate, seqno, handle, ig.get_promise());
  td::actor::send_closure(state_db_, &StateDb::truncate, seqno, handle, ig.get_promise());
}

void RootDb::add_key_block_proof(td::Ref<Proof> proof, td::Promise<td::Unit> promise) {
  auto i = proof->get_basic_header_info().move_as_ok();
  td::actor::send_closure(archive_db_, &ArchiveManager::add_key_block_proof, i.utime, proof->block_id().seqno(),
                          i.end_lt, fileref::Proof{proof->block_id()}, proof->data(), std::move(promise));
}

void RootDb::add_key_block_proof_link(td::Ref<ProofLink> proof, td::Promise<td::Unit> promise) {
  auto i = proof->get_basic_header_info().move_as_ok();
  td::actor::send_closure(archive_db_, &ArchiveManager::add_key_block_proof, i.utime, proof->block_id().seqno(),
                          i.end_lt, fileref::ProofLink{proof->block_id()}, proof->data(), std::move(promise));
}
void RootDb::get_key_block_proof(BlockIdExt block_id, td::Promise<td::Ref<Proof>> promise) {
  auto P = td::PromiseCreator::lambda([block_id, promise = std::move(promise)](td::Result<td::BufferSlice> R) mutable {
    if (R.is_error()) {
      promise.set_error(R.move_as_error());
    } else {
      promise.set_result(create_proof(block_id, R.move_as_ok()));
    }
  });
  td::actor::send_closure(archive_db_, &ArchiveManager::get_key_block_proof, fileref::Proof{block_id}, std::move(P));
}
void RootDb::get_key_block_proof_link(BlockIdExt block_id, td::Promise<td::Ref<ProofLink>> promise) {
  auto P = td::PromiseCreator::lambda([block_id, promise = std::move(promise)](td::Result<td::BufferSlice> R) mutable {
    if (R.is_error()) {
      promise.set_error(R.move_as_error());
    } else {
      promise.set_result(create_proof_link(block_id, R.move_as_ok()));
    }
  });
  td::actor::send_closure(archive_db_, &ArchiveManager::get_key_block_proof, fileref::Proof{block_id}, std::move(P));
}

void RootDb::check_key_block_proof_exists(BlockIdExt block_id, td::Promise<bool> promise) {
  auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<td::BufferSlice> R) mutable {
    if (R.is_error()) {
      promise.set_result(false);
    } else {
      promise.set_result(true);
    }
  });
  td::actor::send_closure(archive_db_, &ArchiveManager::get_key_block_proof, fileref::Proof{block_id}, std::move(P));
}
void RootDb::check_key_block_proof_link_exists(BlockIdExt block_id, td::Promise<bool> promise) {
  auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<td::BufferSlice> R) mutable {
    if (R.is_error()) {
      promise.set_result(false);
    } else {
      promise.set_result(true);
    }
  });
  td::actor::send_closure(archive_db_, &ArchiveManager::get_key_block_proof, fileref::ProofLink{block_id},
                          std::move(P));
}

void RootDb::get_archive_id(BlockSeqno masterchain_seqno, td::Promise<td::uint64> promise) {
  td::actor::send_closure(archive_db_, &ArchiveManager::get_archive_id, masterchain_seqno, std::move(promise));
}

void RootDb::get_archive_slice(td::uint64 archive_id, td::uint64 offset, td::uint32 limit,
                               td::Promise<td::BufferSlice> promise) {
  td::actor::send_closure(archive_db_, &ArchiveManager::get_archive_slice, archive_id, offset, limit,
                          std::move(promise));
}

void RootDb::set_async_mode(bool mode, td::Promise<td::Unit> promise) {
  td::actor::send_closure(archive_db_, &ArchiveManager::set_async_mode, mode, std::move(promise));
}

void RootDb::run_gc(UnixTime ts, UnixTime archive_ttl) {
  td::actor::send_closure(archive_db_, &ArchiveManager::run_gc, ts, archive_ttl);
}

}  // namespace validator

}  // namespace ton
