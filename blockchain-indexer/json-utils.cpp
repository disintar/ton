#include "json-utils.hpp"

std::vector<std::tuple<int, std::string>> parse_extra_currency(const Ref<vm::Cell> &extra) {
  std::vector<std::tuple<int, std::string>> c_list;

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

std::unordered_map<vm::CellHash, std::string> cache;
std::mutex cache_mtx;

std::string dump_as_boc(Ref<vm::Cell> root_cell) {
  auto hash = root_cell->get_hash();

  {
    std::lock_guard<std::mutex> lock(cache_mtx);
    auto it = cache.find(hash);

    if (it != cache.end()) {
      return it->second;
    }
  }

  auto s = td::base64_encode(std_boc_serialize(std::move(root_cell), 31).move_as_ok());

  {
    std::lock_guard<std::mutex> lock(cache_mtx);
    cache.insert({hash, s});
  }
  return s;
}

bool clear_cache() {
  std::lock_guard<std::mutex> lock(cache_mtx);

  cache.clear();
  return true;
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

  std::vector<json> libs;

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

    libs.emplace_back(std::move(lib_json));
    //      out_msgs_list.emplace_back(parse_message(o_msg));
  }

  return libs;
}

json parse_state_init(vm::CellSlice state_init) {
  json answer;

  block::gen::StateInit::Record state_init_parsed;
  auto is_good = tlb::unpack(state_init, state_init_parsed);

  if (is_good) {
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

    answer["type"] = "success";

    return answer;
  } else {
    answer["type"] = "unsuccess";
    return answer;
  }
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

    std::vector<std::tuple<int, std::string>> dummy;
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

json parse_intermediate_address(vm::CellSlice intermediate_address) {
  //  interm_addr_regular$0 use_dest_bits:(#<= 96)
  //  = IntermediateAddress;
  //  interm_addr_simple$10 workchain_id:int8 addr_pfx:uint64
  //                                                       = IntermediateAddress;
  //  interm_addr_ext$11 workchain_id:int32 addr_pfx:uint64
  //                                                     = IntermediateAddress;

  json answer;

  const auto tag = block::gen::t_IntermediateAddress.get_tag(intermediate_address);

  if (tag == block::gen::IntermediateAddress::interm_addr_regular) {
    answer["type"] = "interm_addr_regular";

    block::gen::IntermediateAddress::Record_interm_addr_regular interm_addr_regular;
    CHECK(tlb::unpack(intermediate_address, interm_addr_regular));
    answer["use_dest_bits"] = interm_addr_regular.use_dest_bits; // WARNING: isn't int too small?
  }

  else if (tag == block::gen::IntermediateAddress::interm_addr_simple) {
    answer["type"] = "interm_addr_simple";

    block::gen::IntermediateAddress::Record_interm_addr_simple interm_addr_simple;
    CHECK(tlb::unpack(intermediate_address, interm_addr_simple));
    answer["workchain_id"] = interm_addr_simple.workchain_id;
    answer["addr_pfx"] = interm_addr_simple.addr_pfx;
  }

  else if (tag == block::gen::IntermediateAddress::interm_addr_ext) {
    answer["type"] = "interm_addr_ext";

    block::gen::IntermediateAddress::Record_interm_addr_ext interm_addr_ext;
    CHECK(tlb::unpack(intermediate_address, interm_addr_ext));
    answer["workchain_id"] = interm_addr_ext.workchain_id;
    answer["addr_pfx"] = interm_addr_ext.addr_pfx;
  }

  else {
    answer["type"] = "undefined";
  }

  return answer;
}

json parse_msg_envelope(Ref<vm::Cell> message_envelope) {
  /*
  msg_envelope#4 cur_addr:IntermediateAddress
  next_addr:IntermediateAddress fwd_fee_remaining:Grams
  msg:^(Message Any) = MsgEnvelope;
*/

  json answer;

  block::gen::MsgEnvelope::Record msgEnvelope;
  CHECK(tlb::type_unpack_cell(std::move(message_envelope), block::gen::t_MsgEnvelope, msgEnvelope));

  answer["cur_addr"] = parse_intermediate_address(msgEnvelope.cur_addr.write());
  answer["next_addr"] = parse_intermediate_address(msgEnvelope.next_addr.write());
  answer["fwd_fee_remaining"] = block::tlb::t_Grams.as_integer(msgEnvelope.fwd_fee_remaining.write())->to_dec_string();
  answer["msg"] = parse_message(msgEnvelope.msg);

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

  std::vector<std::tuple<int, std::string>> dummy;
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
    // TODO: parse
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

    // TODO: parse
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

  std::vector<std::tuple<int, std::string>> dummy;
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
    transaction["in_msg_cell"] = dump_as_boc(message);
    transaction["in_msg"] = parse_message(message);
  }

  auto out_msgs = vm::Dictionary{trans.r1.out_msgs, 15};

  std::vector<json> out_msgs_list;

  while (!out_msgs.is_empty()) {
    td::BitArray<15> key{};
    out_msgs.get_minmax_key(key);

    auto o_msg = out_msgs.lookup_delete_ref(key);
    out_msgs_list.emplace_back(parse_message(o_msg));
  }

  transaction["out_msgs"] = out_msgs_list;
  transaction["orig_status"] = parse_type(trans.orig_status);
  transaction["end_status"] = parse_type(trans.end_status);
  transaction["description"] = parse_transaction_descr(trans.description);

  return transaction;
}



json parse_in_msg(vm::CellSlice in_msg, int workchain) {
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

  const auto insert_parsed_transaction
      = [](const Ref<vm::Cell>& transaction, const auto workchain) -> json {
    vm::CellBuilder cb;
    cb.store_ref(transaction);
    const auto body_cell = cb.finalize();
    const auto csr = load_cell_slice_ref(body_cell);

    return parse_transaction(csr, workchain);
  };

  auto tag = block::gen::t_InMsg.check_tag(in_msg);

  if (tag == block::gen::t_InMsg.msg_import_ext) {
    answer["type"] = "msg_import_ext";

    block::gen::InMsg::Record_msg_import_ext msg_import_ext;
    CHECK(tlb::unpack(in_msg, msg_import_ext))

    answer["transaction"] = insert_parsed_transaction(msg_import_ext.transaction, workchain);
    answer["msg"] = parse_message(msg_import_ext.msg);
  }

  else if (tag == block::gen::t_InMsg.msg_import_ihr) {
    answer["type"] = "msg_import_ihr";

    block::gen::InMsg::Record_msg_import_ihr msg_import_ihr;
    CHECK(tlb::unpack(in_msg, msg_import_ihr))

    answer["transaction"] = insert_parsed_transaction(msg_import_ihr.transaction, workchain);
    answer["ihr_fee"] = block::tlb::t_Grams.as_integer(msg_import_ihr.ihr_fee.write())->to_dec_string();
    answer["msg"] = parse_message(msg_import_ihr.msg);

    // TODO:
    //    msg_import_ihr.proof_created - proof_created:^Cell
  }

  else if (tag == block::gen::t_InMsg.msg_import_imm) {
    answer["type"] = "msg_import_imm";

    block::gen::InMsg::Record_msg_import_imm msg_import_imm;
    CHECK(tlb::unpack(in_msg, msg_import_imm))

    answer["transaction"] = insert_parsed_transaction(msg_import_imm.transaction, workchain);
    answer["fwd_fee"] = block::tlb::t_Grams.as_integer(msg_import_imm.fwd_fee.write())->to_dec_string();
    answer["in_msg"] = parse_msg_envelope(msg_import_imm.in_msg);
  }

  else if (tag == block::gen::t_InMsg.msg_import_fin) {
    answer["type"] = "msg_import_fin";

    block::gen::InMsg::Record_msg_import_fin msg_import_fin;
    CHECK(tlb::unpack(in_msg, msg_import_fin))

    answer["transaction"] = insert_parsed_transaction(msg_import_fin.transaction, workchain);
    answer["fwd_fee"] = block::tlb::t_Grams.as_integer(msg_import_fin.fwd_fee.write())->to_dec_string();
    answer["in_msg"] = parse_msg_envelope(msg_import_fin.in_msg);
  }

  else if (tag == block::gen::t_InMsg.msg_import_tr) {
    answer["type"] = "msg_import_tr";

    block::gen::InMsg::Record_msg_import_tr msg_import_tr;
    CHECK(tlb::unpack(in_msg, msg_import_tr))

    answer["transit_fee"] = block::tlb::t_Grams.as_integer(msg_import_tr.transit_fee.write())->to_dec_string();
    answer["in_msg"] = parse_msg_envelope(msg_import_tr.in_msg);
    answer["out_msg"] = parse_msg_envelope(msg_import_tr.out_msg);
  }

  else if (tag == block::gen::t_InMsg.msg_discard_fin) {
    answer["type"] = "msg_discard_fin";

    block::gen::InMsg::Record_msg_discard_fin msg_discard_fin;
    CHECK(tlb::unpack(in_msg, msg_discard_fin))

    answer["transaction_id"] = msg_discard_fin.transaction_id;
    answer["fwd_fee"] = block::tlb::t_Grams.as_integer(msg_discard_fin.fwd_fee.write())->to_dec_string();
    answer["in_msg"] = parse_msg_envelope(msg_discard_fin.in_msg);
  }

  else if (tag == block::gen::t_InMsg.msg_discard_tr) {
    answer["type"] = "msg_discard_tr";

    block::gen::InMsg::Record_msg_discard_tr msg_discard_tr;
    CHECK(tlb::unpack(in_msg, msg_discard_tr))

    answer["transaction_id"] = msg_discard_tr.transaction_id;
    answer["fwd_fee"] = block::tlb::t_Grams.as_integer(msg_discard_tr.fwd_fee.write())->to_dec_string();
    answer["in_msg"] = parse_msg_envelope(msg_discard_tr.in_msg);

    // TODO:
    //    msg_discard_tr.proof_delivered - proof_delivered:^Cell
  }

  else {
    answer["type"] = "undefined";
  }

  return answer;
}

json parse_out_msg(vm::CellSlice out_msg, int workchain) {

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

  const auto insert_parsed_transaction
      = [](const Ref<vm::Cell>& transaction, const auto workchain) -> json {
    vm::CellBuilder cb;
    cb.store_ref(transaction);
    const auto body_cell = cb.finalize();
    const auto csr = load_cell_slice_ref(body_cell);

    return parse_transaction(csr, workchain);
  };

  const auto insert_parsed_in_msg
      = [](const Ref<vm::Cell>& in_msg, const auto workchain) -> json {
    vm::CellBuilder cb;
    cb.store_ref(in_msg);
    const auto body_cell = cb.finalize();
    const auto cs = load_cell_slice(body_cell);

    return parse_in_msg(cs, workchain);
  };

  json answer;

  auto tag = block::gen::t_OutMsg.check_tag(out_msg);

  if (tag == block::gen::t_OutMsg.msg_export_ext) {
    answer["type"] = "msg_export_ext";

    block::gen::OutMsg::Record_msg_export_ext data;
    CHECK(tlb::unpack(out_msg, data))

    answer["transaction"] = insert_parsed_transaction(data.transaction, workchain);
    answer["msg"] = parse_message(data.msg);
  }

  else if (tag == block::gen::t_OutMsg.msg_export_imm) {
    answer["type"] = "msg_export_imm";

    block::gen::OutMsg::Record_msg_export_imm data;
    CHECK(tlb::unpack(out_msg, data))

    answer["transaction"] = insert_parsed_transaction(data.transaction, workchain);
    answer["out_msg"] = parse_msg_envelope(data.out_msg);
    answer["reimport"] = insert_parsed_in_msg(data.reimport, workchain);  // TODO: undefined
  }

  else if (tag == block::gen::t_OutMsg.msg_export_new) {
    answer["type"] = "msg_export_new";

    block::gen::OutMsg::Record_msg_export_new data;
    CHECK(tlb::unpack(out_msg, data))

    answer["transaction"] = insert_parsed_transaction(data.transaction, workchain);
    answer["out_msg"] = parse_msg_envelope(data.out_msg);
  }

  else if (tag == block::gen::t_OutMsg.msg_export_tr) {
    answer["type"] = "msg_export_tr";

    block::gen::OutMsg::Record_msg_export_tr data;
    CHECK(tlb::unpack(out_msg, data))

    answer["out_msg"] = parse_msg_envelope(data.out_msg);
    answer["imported"] = insert_parsed_in_msg(data.imported, workchain);  // TODO: undefined
  }

  else if (tag == block::gen::t_OutMsg.msg_export_deq) {
    answer["type"] = "msg_export_deq";

    block::gen::OutMsg::Record_msg_export_deq data;
    CHECK(tlb::unpack(out_msg, data))

    answer["out_msg"] = parse_msg_envelope(data.out_msg);
    answer["import_block_lt"] = data.import_block_lt;
  }

  else if (tag == block::gen::t_OutMsg.msg_export_deq_short) {
    answer["type"] = "msg_export_deq_short";

    block::gen::OutMsg::Record_msg_export_deq_short data;
    CHECK(tlb::unpack(out_msg, data))

    answer["msg_env_hash"] = data.msg_env_hash.to_hex();
    answer["next_workchain"] = data.next_workchain;
    answer["next_addr_pfx"] = data.next_addr_pfx;
    answer["import_block_lt"] = data.import_block_lt;
  }

  else if (tag == block::gen::t_OutMsg.msg_export_tr_req) {
    answer["type"] = "msg_export_tr_req";

    block::gen::OutMsg::Record_msg_export_tr_req data;
    CHECK(tlb::unpack(out_msg, data))

    answer["out_msg"] = parse_msg_envelope(data.out_msg);
    answer["imported"] = insert_parsed_in_msg(data.imported, workchain);  // TODO: undefined
  }

  else if (tag == block::gen::t_OutMsg.msg_export_deq_imm) {
    answer["type"] = "msg_export_deq_imm";

    block::gen::OutMsg::Record_msg_export_deq_imm data;
    CHECK(tlb::unpack(out_msg, data))

    answer["out_msg"] = parse_msg_envelope(data.out_msg);
    answer["reimport"] = insert_parsed_in_msg(data.reimport, workchain);  // TODO: undefined
  }

  else {
    answer["type"] = "undefined";
  }

  return answer;
}