// Copyright 2023 Disintar LLP / andrey@head-labs.com

#include "PyLiteClient.h"
#include "vm/boc.h"
#include "lite-client/lite-client.h"
#include "crypto/vm/cells/MerkleProof.h"

namespace pylite {

std::string ipv4_int_to_str(int ipv4) {
  return td::IPAddress::ipv4_to_str(ipv4);
}

std::unique_ptr<ton::adnl::AdnlExtClient::Callback> LiteClientActorEngine::make_callback() {
  class Callback : public ton::adnl::AdnlExtClient::Callback {
   public:
    void on_ready() override {
      td::actor::send_closure(id_, &LiteClientActorEngine::conn_ready);
    }
    void on_stop_ready() override {
      td::actor::send_closure(id_, &LiteClientActorEngine::conn_closed);
    }
    Callback(td::actor::ActorId<LiteClientActorEngine> id) : id_(std::move(id)) {
    }

   private:
    td::actor::ActorId<LiteClientActorEngine> id_;
  };
  return std::make_unique<Callback>(actor_id(this));
}

LiteClientActorEngine::LiteClientActorEngine(std::string host, int port, td::Ed25519::PublicKey public_key,
                                             std::shared_ptr<OutputQueue> output_queue_, double timeout_)
    : output_queue(std::move(output_queue_)) {
  adnl_id = ton::adnl::AdnlNodeIdFull{ton::PublicKey(std::move(public_key))};
  remote_addr.init_host_port(host, port).ensure();
  timeout = timeout_;
}

void LiteClientActorEngine::run() {
  client = ton::adnl::AdnlExtClient::create(std::move(adnl_id), remote_addr, make_callback());
}

ResponseWrapper PyLiteClient::receive_unlocked() {
  if (response_obj_queue_ready_ == 0) {
    response_obj_queue_ready_ = response_obj_->reader_wait_nonblock();
  }
  if (response_obj_queue_ready_ > 0) {
    response_obj_queue_ready_--;
    auto res = response_obj_->reader_get_unsafe();
    // TODO: translate errors;
    return res;
  }
  if (timeout != 0) {
    response_obj_->reader_get_event_fd().wait(static_cast<int>(timeout * 10 * 1000));
    return receive_unlocked();
  }

  throw std::logic_error("Timeout on receive request");
}

void LiteClientActorEngine::get_time() {
  auto query = ton::create_tl_object<ton::lite_api::liteServer_getTime>();
  auto q = ton::create_tl_object<ton::lite_api::liteServer_query>(ton::serialize_tl_object(query, true));

  td::actor::send_closure(
      client, &ton::adnl::AdnlExtClient::send_query, "query", serialize_tl_object(std::move(q), true),
      td::Timestamp::in(timeout), [&](td::Result<td::BufferSlice> res) -> void {
        if (res.is_error()) {
          output_queue->writer_put(
              ResponseWrapper(std::make_unique<GetTimeResponse>(GetTimeResponse(0, false, "cannot get server time"))));
          return;
        } else {
          auto F = ton::fetch_tl_object<ton::lite_api::liteServer_currentTime>(res.move_as_ok(), true);

          if (F.is_error()) {
            output_queue->writer_put(ResponseWrapper(std::make_unique<GetTimeResponse>(
                GetTimeResponse(0, false, "cannot parse answer to liteServer.getTime"))));
          } else {
            int x = F.move_as_ok()->now_;
            GetTimeResponse t{x, true, ""};
            output_queue->writer_put(ResponseWrapper(std::make_unique<GetTimeResponse>(t)));
          }
        }
      });
}

void LiteClientActorEngine::qprocess(td::BufferSlice q) {
  td::actor::send_closure(
      client, &ton::adnl::AdnlExtClient::send_query, "query",
      ton::serialize_tl_object(ton::create_tl_object<ton::lite_api::liteServer_query>(std::move(q)), true),
      td::Timestamp::in(timeout), [&](td::Result<td::BufferSlice> res) -> void {
        if (res.is_error()) {
          output_queue->writer_put(
              ResponseWrapper(std::make_unique<ResponseObj>(ResponseObj(false, "Error while fetch"))));
          return;
        } else {
          auto F = res.move_as_ok();
          std::unique_ptr<td::BufferSlice> x = std::make_unique<td::BufferSlice>(std::move(F));

          output_queue->writer_put(
              ResponseWrapper(std::make_unique<SuccessBufferSlice>(SuccessBufferSlice(std::move(x)))));
        }
      });
}

void LiteClientActorEngine::admin_qprocess(td::BufferSlice q) {
  td::actor::send_closure(
      client, &ton::adnl::AdnlExtClient::send_query, "adminquery",
      ton::serialize_tl_object(ton::create_tl_object<ton::lite_api::liteServer_adminQuery>(std::move(q)), true),
      td::Timestamp::in(timeout), [&](td::Result<td::BufferSlice> res) -> void {
        if (res.is_error()) {
          output_queue->writer_put(
              ResponseWrapper(std::make_unique<ResponseObj>(ResponseObj(false, "Error while fetch"))));
          return;
        } else {
          auto F = res.move_as_ok();
          std::unique_ptr<td::BufferSlice> x = std::make_unique<td::BufferSlice>(std::move(F));

          output_queue->writer_put(
              ResponseWrapper(std::make_unique<SuccessBufferSlice>(SuccessBufferSlice(std::move(x)))));
        }
      });
}

void LiteClientActorEngine::get_MasterchainInfoExt(int mode) {
  auto q = ton::serialize_tl_object(ton::create_tl_object<ton::lite_api::liteServer_getMasterchainInfoExt>(mode), true);
  qprocess(std::move(q));
}

void LiteClientActorEngine::lookupBlock(int mode, ton::BlockId block, long long lt, long long time) {
  auto q = ton::serialize_tl_object(ton::create_tl_object<ton::lite_api::liteServer_lookupBlock>(
                                        mode, ton::create_tl_lite_block_id_simple(block), lt, time),
                                    true);
  qprocess(std::move(q));
}

void LiteClientActorEngine::get_AccountState(int workchain, td::Bits256 address_bits, ton::BlockIdExt blkid) {
  auto q = ton::serialize_tl_object(
      ton::create_tl_object<ton::lite_api::liteServer_getAccountState>(
          ton::create_tl_lite_block_id(blkid), std::move(ton::create_tl_object<ton::lite_api::liteServer_accountId>(
                                                   std::move(workchain), std::move(address_bits)))),
      true);
  qprocess(std::move(q));
}

void LiteClientActorEngine::get_Transactions(int count, int workchain, td::Bits256 address_bits, unsigned long long lt,
                                             td::Bits256 hash) {
  auto a = ton::create_tl_object<ton::lite_api::liteServer_accountId>(workchain, address_bits);
  auto q = ton::serialize_tl_object(
      ton::create_tl_object<ton::lite_api::liteServer_getTransactions>(count, std::move(a), lt, hash), true);

  qprocess(std::move(q));
}

void LiteClientActorEngine::get_ConfigAll(int mode, ton::BlockIdExt blkid) {
  auto q = ton::serialize_tl_object(
      ton::create_tl_object<ton::lite_api::liteServer_getConfigAll>(mode, ton::create_tl_lite_block_id(blkid)), true);

  qprocess(std::move(q));
}

void LiteClientActorEngine::get_Block(ton::BlockIdExt blkid) {
  auto q = ton::serialize_tl_object(
      ton::create_tl_object<ton::lite_api::liteServer_getBlock>(ton::create_tl_lite_block_id(blkid)), true);

  qprocess(std::move(q));
}

void LiteClientActorEngine::admin_AddUser(td::Bits256 privkey, td::int64 valid_until, td::int32 ratelimit) {
  auto q = ton::serialize_tl_object(
      ton::create_tl_object<ton::lite_api::liteServer_addUser>(std::move(privkey), valid_until, ratelimit), true);

  admin_qprocess(std::move(q));
}

void LiteClientActorEngine::admin_GetStatData() {
  auto q = ton::serialize_tl_object(ton::create_tl_object<ton::lite_api::liteServer_getStatData>(), true);

  admin_qprocess(std::move(q));
}

void LiteClientActorEngine::get_AllShardsInfo(ton::BlockIdExt blkid) {
  auto q = ton::serialize_tl_object(
      ton::create_tl_object<ton::lite_api::liteServer_getAllShardsInfo>(ton::create_tl_lite_block_id(blkid)), true);

  qprocess(std::move(q));
}

void LiteClientActorEngine::get_listBlockTransactionsExt(ton::BlockIdExt blkid, int mode, int count,
                                                         std::optional<td::Bits256> account,
                                                         std::optional<unsigned long long> lt) {
  bool check_proof = mode & 32;
  bool reverse_mode = mode & 64;
  bool has_starting_tx = mode & 128;

  ton::lite_api::object_ptr<ton::lite_api::liteServer_transactionId3> after;

  if (has_starting_tx) {
    after = ton::lite_api::make_object<ton::lite_api::liteServer_transactionId3>(std::move(account.value()),
                                                                                 std::move(lt.value()));
    LOG(ERROR) << after->account_.to_hex();
    LOG(ERROR) << after->lt_;
  } else {
    after = nullptr;
  }

  auto q = ton::serialize_tl_object(
      ton::create_tl_object<ton::lite_api::liteServer_listBlockTransactionsExt>(
          ton::create_tl_lite_block_id(std::move(blkid)), mode, count, std::move(after), reverse_mode, check_proof),
      true);

  qprocess(std::move(q));
}

void LiteClientActorEngine::get_BlockHeader(ton::BlockIdExt blkid, int mode) {
  auto q = ton::serialize_tl_object(
      ton::create_tl_object<ton::lite_api::liteServer_getBlockHeader>(ton::create_tl_lite_block_id(blkid), mode), true);
  qprocess(std::move(q));
}

void LiteClientActorEngine::get_Libraries(std::vector<td::Bits256> libs) {
  auto q =
      ton::serialize_tl_object(ton::create_tl_object<ton::lite_api::liteServer_getLibraries>(std::move(libs)), true);
  qprocess(std::move(q));
}

void LiteClientActorEngine::send_message(vm::Ref<vm::Cell> cell) {
  auto q = ton::serialize_tl_object(
      ton::create_tl_object<ton::lite_api::liteServer_sendMessage>(vm::std_boc_serialize(cell).move_as_ok()), true);

  //  auto q = ton::create_tl_object<ton::lite_api::liteServer_query>(ton::serialize_tl_object(query, true));

  td::actor::send_closure(
      client, &ton::adnl::AdnlExtClient::send_query, "query", std::move(q), td::Timestamp::in(timeout),
      [&](td::Result<td::BufferSlice> res) -> void {
        if (res.is_error()) {
          output_queue->writer_put(
              ResponseWrapper(std::make_unique<ResponseObj>(ResponseObj(false, res.move_as_error().to_string()))));
          return;
        } else {
          output_queue->writer_put(ResponseWrapper(std::make_unique<ResponseObj>(ResponseObj(true, "all good"))));
        }
      });
}

std::unique_ptr<ResponseObj> PyLiteClient::wait_response() {
  auto is_locked = receive_lock_.exchange(true);
  CHECK(!is_locked);
  auto response = receive_unlocked();
  is_locked = receive_lock_.exchange(false);
  CHECK(is_locked);
  return std::move(response.object);
}

void throw_lite_error(td::BufferSlice b) {
  auto r_error = ton::fetch_tl_object<ton::lite_api::liteServer_error>(b, true);

  if (r_error.is_error()) {
    throw std::logic_error(r_error.move_as_error().to_string());
  } else {
    auto error_data = r_error.move_as_ok();
    throw std::logic_error(error_data->message_);
  }
}

std::unique_ptr<block::AccountState::Info> PyLiteClient::get_AccountState(int workchain, std::string address_string,
                                                                          ton::BlockIdExt& blk) {
  td::RefInt256 address_int = td::string_to_int256(address_string);
  td::Bits256 address_bits;
  if (!address_int->export_bytes(address_bits.data(), 32, false)) {
    throw std::logic_error("Invalid address");
  }

  scheduler_.run_in_context_external(
      [&] { send_closure(engine, &LiteClientActorEngine::get_AccountState, workchain, address_bits, blk); });

  auto response = wait_response();
  if (response->success) {
    SuccessBufferSlice* data = dynamic_cast<SuccessBufferSlice*>(response.get());

    auto R = ton::fetch_tl_object<ton::lite_api::liteServer_accountState>(std::move(data->obj->clone()), true);
    if (R.is_error()) {
      throw_lite_error(data->obj->clone());
    }
    auto x = R.move_as_ok();

    // think of separate proofs if light-server is trusted (?)
    block::AccountState account_state;
    account_state.blk = ton::create_block_id(x->id_);
    account_state.shard_blk = ton::create_block_id(x->shardblk_);
    account_state.shard_proof = std::move(x->shard_proof_);
    account_state.proof = std::move(x->proof_);
    account_state.state = std::move(x->state_);
    account_state.is_virtualized = false;

    auto r_info = account_state.validate(blk, block::StdAddress(workchain, address_bits));
    if (r_info.is_error()) {
      throw std::logic_error(r_info.error().message().str());
    }
    auto info = std::make_unique<block::AccountState::Info>(r_info.move_as_ok());
    return info;
  } else {
    throw std::logic_error(response->error_message);
  }
}

std::unique_ptr<ton::lite_api::liteServer_masterchainInfoExt> PyLiteClient::get_MasterchainInfoExt() {
  scheduler_.run_in_context_external([&] { send_closure(engine, &LiteClientActorEngine::get_MasterchainInfoExt, 0); });

  auto response = wait_response();
  if (response->success) {
    SuccessBufferSlice* data = dynamic_cast<SuccessBufferSlice*>(response.get());
    auto R = ton::fetch_tl_object<ton::lite_api::liteServer_masterchainInfoExt>(std::move(data->obj->clone()), true);

    if (R.is_error()) {
      throw_lite_error(data->obj->clone());
    }

    auto x = R.move_as_ok();

    return std::move(x);
  } else {
    throw std::logic_error(response->error_message);
  }
}

TestNode::BlockHdrInfo process_block_header(ton::BlockIdExt req_blkid, td::BufferSlice data, bool exact) {
  auto y = ton::fetch_tl_object<ton::lite_api::liteServer_blockHeader>(std::move(data), true);
  if (y.is_error()) {
    throw std::logic_error(y.move_as_error().to_string());
  }

  auto f = y.move_as_ok();
  auto blk_id = ton::create_block_id(f->id_);

  if (exact) {
    if (req_blkid.is_valid() && blk_id != req_blkid) {
      throw std::logic_error("block id mismatch: expected data for block " + req_blkid.to_str() + ", obtained for " +
                             blk_id.to_str());
    }
  } else {
    if (blk_id.id.workchain != req_blkid.id.workchain) {
      throw std::logic_error("block id mismatch: expected data for block " + req_blkid.id.to_str() + ", obtained for " +
                             blk_id.id.to_str());
    }
  };

  auto z = vm::std_boc_deserialize(std::move(f->header_proof_));
  if (z.is_error()) {
    throw std::logic_error(z.move_as_error().to_string());
  }
  auto root = z.move_as_ok();
  auto virt_root = vm::MerkleProof::virtualize(root, 1);
  if (virt_root.is_null()) {
    throw std::logic_error("block header proof for block " + blk_id.to_str() + " is not a valid Merkle proof");
  }

  return TestNode::BlockHdrInfo{blk_id, std::move(root), std::move(virt_root), f->mode_};
}

TestNode::BlockHdrInfo PyLiteClient::lookupBlock(int mode, ton::BlockId req_blkid, long long lt, long long time) {
  scheduler_.run_in_context_external(
      [&] { send_closure(engine, &LiteClientActorEngine::lookupBlock, mode, req_blkid, lt, time); });

  auto response = wait_response();
  if (response->success) {
    SuccessBufferSlice* data = dynamic_cast<SuccessBufferSlice*>(response.get());
    td::Bits256 a;
    td::Bits256 b;
    return process_block_header(ton::BlockIdExt(req_blkid, a, b), data->obj->clone(), false);
  } else {
    throw std::logic_error(response->error_message);
  }
}

std::pair<ton::BlockIdExt, PyCell> PyLiteClient::get_ConfigAll(int mode, ton::BlockIdExt req_blkid,
                                                               bool force_check_on_key_block) {
  scheduler_.run_in_context_external(
      [&] { send_closure(engine, &LiteClientActorEngine::get_ConfigAll, mode, req_blkid); });

  auto response = wait_response();
  if (response->success) {
    SuccessBufferSlice* data = dynamic_cast<SuccessBufferSlice*>(response.get());
    auto R = ton::fetch_tl_object<ton::lite_api::liteServer_configInfo>(std::move(data->obj->clone()), true);

    if (R.is_error()) {
      throw_lite_error(data->obj->clone());
    }

    auto x = R.move_as_ok();
    auto blkid = ton::create_block_id(x->id_);

    if (!blkid.is_masterchain_ext()) {
      throw std::logic_error("reference block " + blkid.to_str() +
                             " for the configuration is not a valid masterchain block");
    }

    bool from_key = (mode & 0x8000);
    if (blkid.seqno() > req_blkid.seqno() || (!from_key && blkid != req_blkid)) {
      throw std::logic_error("got configuration parameters with respect to block " + blkid.to_str() + " instead of " +
                             req_blkid.to_str());
    }

    Ref<vm::Cell> state, block, config_proof;
    if (!from_key) {
      auto y = block::check_extract_state_proof(blkid, x->state_proof_.as_slice(), x->config_proof_.as_slice());

      if (y.is_error()) {
        throw std::logic_error(y.move_as_error().to_string());
      }
      state = y.move_as_ok();
    } else {
      auto y = vm::std_boc_deserialize(x->config_proof_.as_slice());

      if (y.is_error()) {
        throw std::logic_error(y.move_as_error().to_string());
      }
      config_proof = y.move_as_ok();
      if (config_proof.is_null()) {
        throw std::logic_error("cannot virtualize configuration proof constructed from key block " + blkid.to_str());
      }

      block = vm::MerkleProof::virtualize(config_proof, 1);
      if (block.is_null()) {
        throw std::logic_error("cannot virtualize configuration proof constructed from key block " + blkid.to_str());
      }

      if (force_check_on_key_block) {
        auto c = block::check_block_header_proof(block, req_blkid);
        if (c.is_error()) {
          throw std::logic_error(c.move_as_error().to_string());
        }
      }
    }

    auto y = from_key ? block::Config::extract_from_key_block(block, mode & 0xfff)
                      : block::Config::extract_from_state(state, mode & 0xfff);
    if (y.is_error()) {
      throw std::logic_error(y.move_as_error().to_string());
    }

    return std::make_pair(blkid, PyCell(std::move(y.move_as_ok()->get_root_cell())));
  } else {
    throw std::logic_error(response->error_message);
  }
}

block::TransactionList::Info PyLiteClient::get_Transactions(int count, int workchain, std::string address_string,
                                                            unsigned long long lt, std::string hash_int_string) {
  td::RefInt256 address_int = td::string_to_int256(address_string);
  td::Bits256 address_bits;
  if (!address_int->export_bytes(address_bits.data(), 32, false)) {
    throw std::logic_error("Invalid address");
  }

  td::RefInt256 hash_int = td::string_to_int256(hash_int_string);
  td::Bits256 hash_bits;
  if (!hash_int->export_bytes(hash_bits.data(), 32, false)) {
    throw std::logic_error("Invalid address");
  }

  scheduler_.run_in_context_external([&] {
    send_closure(engine, &LiteClientActorEngine::get_Transactions, count, workchain, address_bits, lt, hash_bits);
  });

  auto response = wait_response();
  if (response->success) {
    SuccessBufferSlice* data = dynamic_cast<SuccessBufferSlice*>(response.get());
    auto R = ton::fetch_tl_object<ton::lite_api::liteServer_transactionList>(std::move(data->obj->clone()), true);

    if (R.is_error()) {
      throw_lite_error(data->obj->clone());
    }

    auto x = R.move_as_ok();

    std::vector<ton::BlockIdExt> ids;
    while (!x->ids_.empty()) {
      auto jj = ton::create_block_id(x->ids_.back());
      ids.push_back(std::move(jj));
      x->ids_.pop_back();
    }
    std::reverse(ids.begin(), ids.end());

    block::TransactionList list;
    list.blkids = std::move(ids);
    list.hash = hash_bits;
    list.lt = lt;
    list.transactions_boc = std::move(x->transactions_);

    // this will validate chain of transactions
    // because next tx contains prev hash
    auto r = list.validate();

    if (r.is_error()) {
      throw std::logic_error(r.move_as_error().to_string());
    }

    return r.move_as_ok();
  } else {
    throw std::logic_error(response->error_message);
  }
}

TestNode::BlockHdrInfo PyLiteClient::get_BlockHeader(ton::BlockIdExt req_blkid, int mode) {
  scheduler_.run_in_context_external(
      [&] { send_closure(engine, &LiteClientActorEngine::get_BlockHeader, req_blkid, mode); });

  auto response = wait_response();
  if (response->success) {
    SuccessBufferSlice* data = dynamic_cast<SuccessBufferSlice*>(response.get());
    return process_block_header(req_blkid, data->obj->clone(), true);
  } else {
    throw std::logic_error(response->error_message);
  }
}

std::tuple<PubKeyHex, ShortKeyHex> PyLiteClient::admin_AddUser(std::string privkey, td::int64 valid_until,
                                                               td::int32 ratelimit) {
  td::RefInt256 pubkey_int = td::string_to_int256(privkey);
  td::Bits256 privkey_bits;
  if (!pubkey_int->export_bytes(privkey_bits.data(), 32, false)) {
    throw std::logic_error("Invalid pubkey");
  }

  scheduler_.run_in_context_external(
      [&] { send_closure(engine, &LiteClientActorEngine::admin_AddUser, privkey_bits, valid_until, ratelimit); });

  auto response = wait_response();
  if (response->success) {
    SuccessBufferSlice* data = dynamic_cast<SuccessBufferSlice*>(response.get());
    auto R = ton::fetch_tl_object<ton::lite_api::liteServer_newUser>(std::move(data->obj->clone()), true);
    if (R.is_error()) {
      throw_lite_error(data->obj->clone());
    }
    auto x = R.move_as_ok();

    return std::make_tuple(x->pubkey_.to_hex(), x->short_.to_hex());
  } else {
    throw std::logic_error(response->error_message);
  }
}

std::vector<std::tuple<ShortKeyHex, int, td::int64, td::int64, bool>> PyLiteClient::admin_getStatData() {
  scheduler_.run_in_context_external([&] { send_closure(engine, &LiteClientActorEngine::admin_GetStatData); });

  auto response = wait_response();
  if (response->success) {
    SuccessBufferSlice* data = dynamic_cast<SuccessBufferSlice*>(response.get());
    auto R = ton::fetch_tl_object<ton::lite_api::liteServer_stats>(std::move(data->obj->clone()), true);
    if (R.is_error()) {
      throw_lite_error(data->obj->clone());
    }
    auto x = R.move_as_ok();

    std::vector<std::tuple<ShortKeyHex, int, td::int64, td::int64, bool>> tmp;
    tmp.reserve(x->data_.size());

    while (!x->data_.empty()) {
      auto e = std::move(x->data_.back());
      x->data_.pop_back();

      tmp.emplace_back(e->shortid_.to_hex(), e->method_, e->start_at_, e->end_at_, e->success_);
    }

    return tmp;
  } else {
    throw std::logic_error(response->error_message);
  }
};

PyCell PyLiteClient::get_Block(ton::BlockIdExt req_blkid) {
  scheduler_.run_in_context_external([&] { send_closure(engine, &LiteClientActorEngine::get_Block, req_blkid); });

  auto response = wait_response();
  if (response->success) {
    SuccessBufferSlice* rdata = dynamic_cast<SuccessBufferSlice*>(response.get());
    auto R = ton::fetch_tl_object<ton::lite_api::liteServer_blockData>(std::move(rdata->obj->clone()), true);
    if (R.is_error()) {
      throw_lite_error(rdata->obj->clone());
    }

    auto x = R.move_as_ok();
    auto blk_id = ton::create_block_id(x->id_);

    if (blk_id != req_blkid) {
      throw std::logic_error("block id mismatch: expected data for block " + req_blkid.to_str() + ", obtained for " +
                             blk_id.to_str());
    }

    auto data = std::move(x->data_);
    ton::FileHash fhash;
    td::sha256(data.as_slice(), fhash.as_slice());

    if (fhash != req_blkid.file_hash) {
      throw std::logic_error("file hash mismatch for block " + req_blkid.to_str() + ": expected " +
                             req_blkid.file_hash.to_hex() + ", computed " + fhash.to_hex());
    }

    auto res = vm::std_boc_deserialize(std::move(data));
    if (res.is_error()) {
      std::logic_error("cannot deserialize block data : " + res.move_as_error().to_string());
    }

    auto root = res.move_as_ok();
    ton::RootHash rhash{root->get_hash().bits()};
    if (rhash != req_blkid.root_hash) {
      std::logic_error("block root hash mismatch: data has " + rhash.to_hex() + " , expected " +
                       req_blkid.root_hash.to_hex());
    }

    return PyCell(std::move(root));
  } else {
    throw std::logic_error(response->error_message);
  }
}

std::vector<ton::BlockId> PyLiteClient::get_AllShardsInfo(ton::BlockIdExt req_blkid) {
  scheduler_.run_in_context_external(
      [&] { send_closure(engine, &LiteClientActorEngine::get_AllShardsInfo, std::move(req_blkid)); });

  auto response = wait_response();
  if (response->success) {
    SuccessBufferSlice* rdata = dynamic_cast<SuccessBufferSlice*>(response.get());
    auto R = ton::fetch_tl_object<ton::lite_api::liteServer_allShardsInfo>(std::move(rdata->obj->clone()), true);
    if (R.is_error()) {
      throw_lite_error(rdata->obj->clone());
    }

    auto x = R.move_as_ok();
    // todo: proofs

    td::BufferSlice data = std::move((*x).data_);
    if (data.empty()) {
      throw std::logic_error("shard configuration is empty");
    } else {
      auto R2 = vm::std_boc_deserialize(data.clone());
      if (R2.is_error()) {
        throw std::logic_error(R2.move_as_error().to_string());
      }
      auto root = R2.move_as_ok();

      block::ShardConfig sh_conf;
      if (!sh_conf.unpack(vm::load_cell_slice_ref(root))) {
        throw std::logic_error("cannot extract shard block list from shard configuration");
      } else {
        return sh_conf.get_shard_hash_ids(true);
      }
    }
  } else {
    throw std::logic_error(response->error_message);
  }
}

PyDict PyLiteClient::get_Libraries(std::vector<std::string> libs) {
  std::vector<td::Bits256> libs_bits;
  libs_bits.reserve(libs.size());

  while (!libs.empty()) {
    td::RefInt256 address_int = td::string_to_int256(libs.back());
    td::Bits256 address_bits;
    if (!address_int->export_bytes(address_bits.data(), 32, false)) {
      throw std::logic_error("Invalid lib hash");
    }
    libs_bits.push_back(std::move(address_bits));
    libs.pop_back();
  }

  std::reverse(libs_bits.begin(), libs_bits.end());
  scheduler_.run_in_context_external([&] { send_closure(engine, &LiteClientActorEngine::get_Libraries, libs_bits); });

  auto response = wait_response();
  if (response->success) {
    SuccessBufferSlice* data = dynamic_cast<SuccessBufferSlice*>(response.get());
    auto R = ton::fetch_tl_object<ton::lite_api::liteServer_libraryResult>(std::move(data->obj->clone()), true);
    if (R.is_error()) {
      throw_lite_error(data->obj->clone());
    }
    auto libraries = R.move_as_ok();
    vm::Dictionary libraries_dict{256};

    for (auto& lr : libraries->result_) {
      auto contents = vm::std_boc_deserialize(lr->data_);
      if (contents.is_ok() && contents.ok().not_null()) {
        if (contents.ok()->get_hash().bits().compare(lr->hash_.cbits(), 256)) {
          LOG(WARNING) << "hash mismatch for library " << lr->hash_.to_hex();
          continue;
        }

        libraries_dict.set_ref(lr->hash_, contents.move_as_ok());
      }
    }

    return PyDict(libraries_dict);
  } else {
    throw std::logic_error(response->error_message);
  }
}

BlockTransactionsExt PyLiteClient::get_listBlockTransactionsExt(ton::BlockIdExt blkid, int mode, int count,
                                                                std::optional<std::string> account,
                                                                std::optional<unsigned long long> lt) {
  bool check_proof = mode & 32;
  bool reverse_mode = mode & 64;
  bool has_starting_tx = mode & 128;

  std::optional<td::Bits256> tmp;

  if (has_starting_tx) {
    if (!account || !lt) {
      throw std::logic_error("No account or lt with this flag");
    }

    td::RefInt256 root_hash_int = td::string_to_int256(std::move(account.value()));
    td::Bits256 root_hash_bits;
    if (!root_hash_int->export_bytes(root_hash_bits.data(), 32, false)) {
      throw std::logic_error("Invalid root_hash");
    }

    tmp = std::make_optional<td::Bits256>(std::move(root_hash_bits));
  } else {
    tmp = std::make_optional<td::Bits256>();
  }

  scheduler_.run_in_context_external([&] {
    send_closure(engine, &LiteClientActorEngine::get_listBlockTransactionsExt, std::move(blkid), mode, count,
                 std::move(tmp), std::move(lt));
  });

  auto response = wait_response();
  if (response->success) {
    SuccessBufferSlice* rdata = dynamic_cast<SuccessBufferSlice*>(response.get());
    auto R = ton::fetch_tl_object<ton::lite_api::liteServer_blockTransactionsExt>(std::move(rdata->obj->clone()), true);
    if (R.is_error()) {
      throw_lite_error(rdata->obj->clone());
    }

    auto x = R.move_as_ok();

    // todo: check proof if check_proof
    auto r = vm::std_boc_deserialize_multi(std::move(x->transactions_));
    if (r.is_error()) {
      throw std::logic_error(r.move_as_error().to_string());
    }

    std::vector<PyCell> txs;
    for (auto y : r.move_as_ok()) {
      txs.push_back(PyCell(y));
    };

    BlockTransactionsExt answer;
    answer.id = ton::create_block_id(std::move(x->id_));
    answer.incomplete = x->incomplete_;
    answer.req_count = x->req_count_;
    answer.transactions = std::move(txs);

    return answer;
  } else {
    throw std::logic_error(response->error_message);
  }
}

}  // namespace pylite