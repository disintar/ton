#include "IBlockParser.hpp"
#include "blockchain-indexer/json-utils.hpp"

namespace ton::validator {

BlockParser::BlockParser(std::unique_ptr<IBLockPublisher> publisher)
    : publisher_(std::move(publisher))
    , publish_applied_thread_(&BlockParser::publish_applied_worker, this)
    , publish_blocks_thread_(&BlockParser::publish_blocks_worker, this)
    , publish_states_thread_(&BlockParser::publish_states_worker, this) {
}

BlockParser::~BlockParser() {
  running_ = false;
  publish_blocks_cv_.notify_all();
  publish_states_cv_.notify_all();
  publish_blocks_thread_.join();
  publish_states_thread_.join();
}

void BlockParser::storeBlockApplied(BlockIdExt id) {
  std::lock_guard<std::mutex> lock(maps_mtx_);
  LOG(WARNING) << "Store applied: " << id.to_str();
  const std::string key =
      std::to_string(id.id.workchain) + ":" + std::to_string(id.id.shard) + ":" + std::to_string(id.id.seqno);
  stored_applied_.insert({key, id});
  handleBlockProgress(id);
}

void BlockParser::storeBlockData(BlockHandle handle, td::Ref<BlockData> block) {
  std::lock_guard<std::mutex> lock(maps_mtx_);
  LOG(WARNING) << "Store block: " << block->block_id().to_str();
  const std::string key = std::to_string(handle->id().id.workchain) + ":" + std::to_string(handle->id().id.shard) +
                          ":" + std::to_string(handle->id().id.seqno);
  auto blocks_vec = stored_blocks_.find(key);
  if (blocks_vec == stored_blocks_.end()) {
    std::vector<std::pair<BlockHandle, td::Ref<BlockData>>> vec;
    vec.emplace_back(std::pair{handle, block});
    stored_blocks_.insert({key, vec});
  } else {
    blocks_vec->second.emplace_back(std::pair{handle, block});
  }

  handleBlockProgress(handle->id());
}

void BlockParser::storeBlockState(BlockHandle handle, td::Ref<ShardState> state) {
  std::lock_guard<std::mutex> lock(maps_mtx_);
  LOG(WARNING) << "Store state: " << state->get_block_id().to_str();
  const std::string key = std::to_string(handle->id().id.workchain) + ":" + std::to_string(handle->id().id.shard) +
                          ":" + std::to_string(handle->id().id.seqno);
  auto states_vec = stored_states_.find(key);
  if (states_vec == stored_states_.end()) {
    std::vector<std::pair<BlockHandle, td::Ref<ShardState>>> vec;
    vec.emplace_back(std::pair{handle, state});
    stored_states_.insert({key, vec});
  } else {
    states_vec->second.emplace_back(std::pair{handle, state});
  }

  handleBlockProgress(handle->id());
}

void BlockParser::handleBlockProgress(BlockIdExt id) {
  const std::string key =
      std::to_string(id.id.workchain) + ":" + std::to_string(id.id.shard) + ":" + std::to_string(id.id.seqno);

  auto applied_found = stored_applied_.find(key);
  if (applied_found == stored_applied_.end()) {
    return;
  }
  const auto applied = applied_found->second;

  auto blocks_vec_found = stored_blocks_.find(key);
  if (blocks_vec_found == stored_blocks_.end()) {
    return;
  }
  const auto blocks_vec = blocks_vec_found->second;
  auto block_found_iter =
      std::find_if(blocks_vec.begin(), blocks_vec.end(), [&id](const auto& b) { return b.first->id() == id; });
  if (block_found_iter == blocks_vec.end()) {
    return;
  }

  auto states_vec_found = stored_states_.find(key);
  if (states_vec_found == stored_states_.end()) {
    return;
  }
  const auto states_vec = states_vec_found->second;
  auto state_found_iter =
      std::find_if(states_vec.begin(), states_vec.end(), [&id](const auto& s) { return s.first->id() == id; });
  if (state_found_iter == states_vec.end()) {
    return;
  }

  const auto applied_parsed = parseBlockApplied(id);
  enqueuePublishBlockApplied(applied_parsed);

  try {
    const auto block_parsed = parseBlockData(id, block_found_iter->first, block_found_iter->second);
    enqueuePublishBlockData(block_parsed.first);

    try {
      const auto state_parsed =
          parseBlockState(id, state_found_iter->first, state_found_iter->second, block_parsed.second);
      enqueuePublishBlockState(state_parsed);
    } catch (vm::VmError& e) {
      LOG(ERROR) << "VM ERROR: state " << e.get_msg();
    }

  } catch (vm::VmError& e) {
    LOG(ERROR) << "VM ERROR: block " << e.get_msg();
  }

  stored_applied_.erase(stored_applied_.find(key));
  stored_blocks_.erase(stored_blocks_.find(key));
  stored_states_.erase(stored_states_.find(key));
}

std::string BlockParser::parseBlockApplied(BlockIdExt id) {
  LOG(DEBUG) << "Parse Applied" << id.to_str();

  json to_dump = {{"file_hash", id.file_hash.to_hex()},
                  {"root_hash", id.root_hash.to_hex()},
                  {"id",
                   {
                       {"workchain", id.id.workchain},
                       {"seqno", id.id.seqno},
                       {"shard", id.id.shard},
                   }}};

  std::string dump = to_dump.dump();
  if (post_processor_) {
    dump = post_processor_(dump);
  }

  return dump;
}

std::pair<std::string, std::vector<td::Bits256>> BlockParser::parseBlockData(BlockIdExt id, BlockHandle handle,
                                                                             td::Ref<BlockData> data) {
  LOG(DEBUG) << "Parse Data" << id.to_str();

  //  CHECK(block.not_null());

  auto blkid = data->block_id();
  LOG(DEBUG) << "Parse: " << blkid.to_str();

  auto block_root = data->root_cell();
  if (block_root.is_null()) {
    LOG(ERROR) << "block has no valid root cell";
    std::abort();  // TODO:
                   //    return;
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

  LOG(DEBUG) << "Parse: " << blkid.to_str() << " ValueFlow success";
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
  std::vector<td::Bits256> accounts_keys;

  while (!account_blocks_dict->is_empty()) {
    td::Bits256 last_key;
    Ref<vm::CellSlice> data;

    account_blocks_dict->get_minmax_key(last_key);
    auto hex_addr = last_key.to_hex();
    LOG(DEBUG) << "Parse: " << blkid.to_str() << " at " << hex_addr;

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
      transactions.push_back(transaction);

      ++count;
    };
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

    LOG(DEBUG) << "Parse: " << blkid.to_str() << " BlockExtra success, start shard_fees_dict parse";

    vm::Dictionary shard_fees_dict{extra_mc.shard_fees->prefetch_ref(), 96};
    LOG(DEBUG) << "Parse: " << blkid.to_str() << " shard_fees_dict got";
    std::map<std::string, json> shard_fees;

    while (!shard_fees_dict.is_empty()) {
      td::BitArray<96> key{};
      shard_fees_dict.get_minmax_key(key);
      LOG(DEBUG) << "Parse: " << blkid.to_str() << " shard_fees_dict at " << key.to_hex();

      Ref<vm::CellSlice> tvalue;
      tvalue = shard_fees_dict.lookup_delete(key);

      block::gen::ShardFeeCreated::Record sf;
      CHECK(tlb::unpack(tvalue.write(), sf));

      block::gen::CurrencyCollection::Record fees;
      block::gen::CurrencyCollection::Record create;

      CHECK(tlb::unpack(sf.fees.write(), fees));
      CHECK(tlb::unpack(sf.create.write(), create));

      std::vector<std::tuple<int, std::string>> dummy;

      json data = {
          {"fees",
           {{"grams", block::tlb::t_Grams.as_integer(fees.grams)->to_dec_string()},
            {"extra", fees.other->have_refs() ? parse_extra_currency(fees.other->prefetch_ref()) : dummy}}},

          {"create",
           {{"grams", block::tlb::t_Grams.as_integer(create.grams)->to_dec_string()},
            {"extra", create.other->have_refs() ? parse_extra_currency(create.other->prefetch_ref()) : dummy}}}};

      shard_fees[key.to_hex()] = data;
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

    auto f = [&shards_json, &blkid](McShardHash& ms) {
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
    LOG(DEBUG) << "Parse: " << blkid.to_str() << " BlockExtra shards success";
    answer["BlockExtra"]["custom"]["shards"] = shards_json;
  }

  vm::CellSlice upd_cs{vm::NoVmSpec(), blk.state_update};
  if (!(upd_cs.is_special() && upd_cs.prefetch_long(8) == 4  // merkle update
        && upd_cs.size_ext() == 0x20228)) {
    LOG(ERROR) << "invalid Merkle update in block";
    std::abort();  // TODO:
                   //    return;
  }

  CHECK(upd_cs.have_refs(2));
  auto state_old_hash = upd_cs.prefetch_ref(0)->get_hash(0).to_hex();
  auto state_hash = upd_cs.prefetch_ref(1)->get_hash(0).to_hex();

  answer["ShardState"] = {{"state_old_hash", state_old_hash}, {"state_hash", state_hash}};
  LOG(DEBUG) << "Parse: " << blkid.to_str() << " ShardState success";

  json to_dump = {
      {"id", std::to_string(workchain) + ":" + std::to_string(blkid.id.shard) + ":" + std::to_string(blkid.seqno())},
      {"data", answer}};

  std::string dump = to_dump.dump();
  LOG(DEBUG) << "Parse: " << blkid.to_str() << " success";

  if (post_processor_) {
    dump = post_processor_(dump);
  }

  return std::pair{dump, accounts_keys};
}

std::string BlockParser::parseBlockState(BlockIdExt id, BlockHandle handle, td::Ref<ShardState> state,
                                         std::vector<td::Bits256> accounts_keys) {
  LOG(WARNING) << "Parse state: " << id.to_str();

  CHECK(state.not_null());

  auto root_cell = state->root_cell();

  block::gen::ShardStateUnsplit::Record shard_state;
  CHECK(tlb::unpack_cell(root_cell, shard_state));

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

  json answer = {
      {"type", "shard_state"},
      {"id",
       {
           {"workchain", id.id.workchain},
           {"seqno", id.id.seqno},
           {"shard", id.id.shard},
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

        publishers.push_back(publisher.to_hex());
      }

      json data = {{"hash", key.to_hex()}, {"lib", dump_as_boc(libdescr.lib)}, {"publishers", publishers}};
      libs.push_back(data);
    }

    answer["libraries"] = libs;
  }

  vm::AugmentedDictionary accounts{vm::load_cell_slice_ref(shard_state.accounts), 256, block::tlb::aug_ShardAccounts};

  std::vector<json> accounts_list;

  for (const auto& account : accounts_keys) {
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
      data["account_address"] = {{"workchain", id.id.workchain}, {"address", account.to_hex()}};
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

          try {
            data["account"]["state"] = {{"type", "active"}, {"state_init", parse_state_init(active_account.x.write())}};
          } catch (...) {
            LOG(ERROR) << "State init parse fail";
            data["account"]["state"] = {{"type", "uninit"}};
          }

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

  json to_dump = {
      {"id", std::to_string(id.id.workchain) + ":" + std::to_string(id.id.shard) + ":" + std::to_string(id.id.seqno)},
      {"data", answer}};

  std::string dump = to_dump.dump();

  if (post_processor_) {
    dump = post_processor_(dump);
  }

  return dump;
}

void BlockParser::setPostProcessor(std::function<std::string(std::string)> post_processor) {
  post_processor_ = std::move(post_processor);
}

void BlockParser::gotState(BlockHandle handle, td::Ref<ShardState> state, std::vector<td::Bits256> accounts_keys) {
  auto block_id = state->get_block_id();
  LOG(WARNING) << "Parse state: " << block_id.to_str();
  CHECK(state.not_null());

  auto root_cell = state->root_cell();

  block::gen::ShardStateUnsplit::Record shard_state;
  CHECK(tlb::unpack_cell(root_cell, shard_state));

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

        publishers.push_back(publisher.to_hex());
      }

      json data = {{"hash", key.to_hex()}, {"lib", dump_as_boc(libdescr.lib)}, {"publishers", publishers}};
      libs.push_back(data);
    }

    answer["libraries"] = libs;
  }

  vm::AugmentedDictionary accounts{vm::load_cell_slice_ref(shard_state.accounts), 256, block::tlb::aug_ShardAccounts};

  std::vector<json> accounts_list;

  for (const auto& account : accounts_keys) {
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

          try {
            data["account"]["state"] = {{"type", "active"}, {"state_init", parse_state_init(active_account.x.write())}};
          } catch (...) {
            LOG(ERROR) << "State init parse fail";
            data["account"]["state"] = {{"type", "uninit"}};
          }

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

  json to_dump = {{"id", std::to_string(block_id.id.workchain) + ":" + std::to_string(block_id.id.shard) + ":" +
                             std::to_string(block_id.id.seqno)},
                  {"data", answer}};

  std::string dump = to_dump.dump();

  if (post_processor_) {
    dump = post_processor_(dump);
  }

  enqueuePublishBlockState(dump);
}

void BlockParser::enqueuePublishBlockApplied(std::string json) {
  std::unique_lock lock(publish_applied_mtx_);
  publish_applied_queue_.emplace(json);
  lock.unlock();
  publish_applied_cv_.notify_one();
}

void BlockParser::enqueuePublishBlockData(std::string json) {
  std::unique_lock lock(publish_blocks_mtx_);
  publish_blocks_queue_.emplace(json);
  lock.unlock();
  publish_blocks_cv_.notify_one();
}

void BlockParser::enqueuePublishBlockState(std::string json) {
  std::unique_lock lock(publish_states_mtx_);
  publish_states_queue_.emplace(json);
  lock.unlock();
  publish_states_cv_.notify_one();
}

void BlockParser::publish_applied_worker() {
  bool should_run = running_;
  while (should_run) {
    std::unique_lock lock(publish_applied_mtx_);
    publish_applied_cv_.wait(lock, [this] { return !publish_applied_queue_.empty() || !running_; });
    if (publish_applied_queue_.empty())
      return;

    auto block = std::move(publish_applied_queue_.front());
    publish_applied_queue_.pop();

    should_run = running_ || !publish_applied_queue_.empty();
    lock.unlock();

    publisher_->publishBlockApplied(block);
  }
}

void BlockParser::publish_blocks_worker() {
  bool should_run = running_;
  while (should_run) {
    std::unique_lock lock(publish_blocks_mtx_);
    publish_blocks_cv_.wait(lock, [this] { return !publish_blocks_queue_.empty() || !running_; });
    if (publish_blocks_queue_.empty())
      return;

    auto block = std::move(publish_blocks_queue_.front());
    publish_blocks_queue_.pop();

    should_run = running_ || !publish_blocks_queue_.empty();
    lock.unlock();

    publisher_->publishBlockData(block);
  }
}

void BlockParser::publish_states_worker() {
  bool should_run = running_;
  while (should_run) {
    std::unique_lock lock(publish_states_mtx_);
    publish_states_cv_.wait(lock, [this] { return !publish_states_queue_.empty() || !running_; });
    if (publish_states_queue_.empty())
      return;

    auto state = std::move(publish_states_queue_.front());
    publish_states_queue_.pop();

    should_run = running_ || !publish_states_queue_.empty();
    lock.unlock();

    publisher_->publishBlockState(state);
  }
}

}  // namespace ton::validator
