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
#include "manager-disk.hpp"
#include "validator-group.hpp"
#include "adnl/utils.hpp"
#include "adnl/adnl-ext-client.h"
#include "downloaders/wait-block-state.hpp"
#include "downloaders/wait-block-state-merge.hpp"
#include "downloaders/wait-block-data-disk.hpp"
#include "validator-group.hpp"
#include "fabric.h"
#include "manager.h"
#include "ton/ton-io.hpp"
#include "td/utils/overloaded.h"
#include "auto/tl/lite_api.h"
#include "tl-utils/lite-utils.hpp"
#include "common/delay.h"
#include "lite-server-rate-limiter.h"
#include "block/block.h"
#include "block/block-parse.h"
#include "block/block-auto.h"
#include "td/utils/date.h"

namespace ton {

namespace validator {

void ShardClientDetector::start_up() {
  //  alarm_timestamp() = td::Timestamp::in(0.2);
}

void ShardClientDetector::init_wait(BlockIdExt blkid, td::Ref<MasterchainState> mcs) {
  if (mc_states_.size() >= allow_degrade) {  // todo: rewrite on blockrate
    // We're in degrade mode, too many blocks without actual shard
    // Return current block as up-to-date and clear all waiters
    td::actor::send_closure_later(manager_, &ValidatorManager::update_lite_server_state, blkid, std::move(mcs));

    mc_states_.clear();
    mc_shards_waits_.clear();
  } else {
    mc_states_[blkid] = std::move(mcs);
    mc_shards_waits_[blkid] = 0;
  }
}

void ShardClientDetector::increase_wait(BlockIdExt blkid) {
  mc_shards_waits_[blkid] += 1;
}

void ShardClientDetector::receive_result(BlockIdExt mc_blkid, BlockIdExt shard_blkid,
                                         td::Result<td::Ref<ShardState>> R) {
  if (mc_shards_waits_.find(mc_blkid) == mc_shards_waits_.end()) {
    // skip degraded block
    return;
  }

  if (R.is_ok()) {
    auto x = R.move_as_ok();
    auto cell = x->root_cell();

    if (!cell.is_null()) {
      //      LOG(WARNING) << "Validate main block: " << mc_blkid << ", shard: " << shard_blkid;
      mc_shards_waits_[mc_blkid] -= 1;
      if (mc_shards_waits_[mc_blkid] == 0) {
        mc_shards_waits_.erase(mc_blkid);
        auto state = std::move(mc_states_[mc_blkid]);
        mc_states_.erase(mc_blkid);

        if (shard_waiters.empty()) {
          alarm_timestamp() = td::Timestamp::never();
        }

        td::actor::send_closure_later(manager_, &ValidatorManager::update_lite_server_state, mc_blkid,
                                      std::move(state));
        return;
      }
    }
  }

  LOG(WARNING) << "Can't get shard for: " << mc_blkid << " shard: " << shard_blkid;
  shard_waiters.push_back(std::make_tuple(mc_blkid, shard_blkid));
  alarm_timestamp() = td::Timestamp::in(0.2);
}

void ShardClientDetector::alarm() {
  alarm_timestamp() = td::Timestamp::never();

  while (!shard_waiters.empty()) {
    auto x = shard_waiters.back();
    shard_waiters.pop_back();

    auto P_cb = td::PromiseCreator::lambda([DetectorId = actor_id(this), mc_id = std::get<0>(x),
                                            my_id = std::get<1>(x)](td::Result<td::Ref<ShardState>> R) {
      td::actor::send_closure_later(DetectorId, &ShardClientDetector::receive_result, mc_id, my_id, std::move(R));
    });

    LOG(WARNING) << "Try shard client one more time: " << std::get<0>(x);
    td::actor::send_closure_later(manager_, &ValidatorManager::get_shard_state_from_db_short, std::get<0>(x),
                                  std::move(P_cb));
  }
}

void ValidatorManagerImpl::validate_block_is_next_proof(BlockIdExt prev_block_id, BlockIdExt next_block_id,
                                                        td::BufferSlice proof, td::Promise<td::Unit> promise) {
  UNREACHABLE();
}

void ValidatorManagerImpl::validate_block_proof(BlockIdExt block_id, td::BufferSlice proof,
                                                td::Promise<td::Unit> promise) {
  auto pp = create_proof(block_id, std::move(proof));
  if (pp.is_error()) {
    promise.set_error(pp.move_as_error());
    return;
  }

  auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<BlockHandle> R) mutable {
    if (R.is_error()) {
      promise.set_error(R.move_as_error());
    } else {
      promise.set_value(td::Unit());
    }
  });
  run_check_proof_query(block_id, pp.move_as_ok(), actor_id(this), td::Timestamp::in(2.0), std::move(P));
}

void ValidatorManagerImpl::validate_block_proof_link(BlockIdExt block_id, td::BufferSlice proof,
                                                     td::Promise<td::Unit> promise) {
  UNREACHABLE();
}

void ValidatorManagerImpl::check_ext_query(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data,
                                           td::Promise<td::BufferSlice> promise) {
  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this),
       dst](td::Result<std::tuple<td::BufferSlice, td::Promise<td::BufferSlice>, td::uint8>> R) {
        if (R.is_ok()) {
          auto proxy_data = R.move_as_ok();
          auto promise = std::move(std::get<1>(proxy_data));
          auto status = std::get<2>(proxy_data);

          if (status == ton::liteserver::StatusCode::OK) {
            td::actor::send_closure(SelfId, &ValidatorManagerImpl::run_ext_query_extended,
                                    std::move(std::get<0>(proxy_data)), dst, std::move(promise));
          } else if (status == ton::liteserver::StatusCode::PROCESSED) {
            return;
          } else {
            promise.set_error(td::Status::Error(status, "ratelimit"));
          }
        } else {
          return;
        }
      });
  td::actor::send_closure(lslimiter_, &liteserver::LiteServerLimiter::recv_connection, src, dst, std::move(data),
                          std::move(promise), std::move(P));
};

void ValidatorManagerImpl::add_ext_server_id(adnl::AdnlNodeIdShort id) {
  if (offline_) {
    UNREACHABLE();
  } else {
    class Cb : public adnl::Adnl::Callback {
     private:
      td::actor::ActorId<ValidatorManagerImpl> id_;

      void receive_message(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data) override {
      }
      void receive_query(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data,
                         td::Promise<td::BufferSlice> promise) override {
        td::actor::send_closure(id_, &ValidatorManagerImpl::check_ext_query, src, dst, std::move(data),
                                std::move(promise));
      }

     public:
      Cb(td::actor::ActorId<ValidatorManagerImpl> id) : id_(std::move(id)) {
      }
    };

    td::actor::send_closure(adnl_, &adnl::Adnl::subscribe, id,
                            adnl::Adnl::int_to_bytestring(lite_api::liteServer_query::ID),
                            std::make_unique<Cb>(actor_id(this)));

    td::actor::send_closure(adnl_, &adnl::Adnl::subscribe, id,
                            adnl::Adnl::int_to_bytestring(lite_api::liteServer_adminQuery::ID),
                            std::make_unique<Cb>(actor_id(this)));

    if (lite_server_.empty()) {
      pending_ext_ids_.push_back(id);
    } else {
      td::actor::send_closure(lite_server_, &adnl::AdnlExtServer::add_local_id, id);
    }
  }
}

void ValidatorManagerImpl::add_ext_server_port(td::uint16 port) {
  if (offline_) {
    UNREACHABLE();
  } else {
    if (lite_server_.empty()) {
      pending_ext_ports_.push_back(port);
    } else {
      td::actor::send_closure(lite_server_, &adnl::AdnlExtServer::add_tcp_port, port);
    }
  }
}

void ValidatorManagerImpl::validate_block(ReceivedBlock block, td::Promise<BlockHandle> promise) {
  UNREACHABLE();
}

void ValidatorManagerImpl::new_block_broadcast(BlockBroadcast broadcast, td::Promise<td::Unit> promise) {
  UNREACHABLE();
}

void ValidatorManagerImpl::sync_complete(td::Promise<td::Unit> promise) {
  started_ = true;

  //ShardIdFull shard_id{masterchainId, shardIdAll};
  auto shard_id = shard_to_generate_;

  auto block_id = block_to_generate_;

  std::vector<BlockIdExt> prev;
  if (!block_id.is_valid()) {
    if (shard_id.is_masterchain()) {
      prev = {last_masterchain_block_id_};
    } else {
      auto S = last_masterchain_state_->get_shard_from_config(shard_id);
      if (S.not_null()) {
        prev = {S->top_block_id()};
      } else {
        S = last_masterchain_state_->get_shard_from_config(shard_parent(shard_id));
        if (S.not_null()) {
          CHECK(S->before_split());
          prev = {S->top_block_id()};
        } else {
          S = last_masterchain_state_->get_shard_from_config(shard_child(shard_id, true));
          CHECK(S.not_null());
          CHECK(S->before_merge());
          auto S2 = last_masterchain_state_->get_shard_from_config(shard_child(shard_id, false));
          CHECK(S2.not_null());
          CHECK(S2->before_merge());
          prev = {S->top_block_id(), S2->top_block_id()};
        }
      }
    }
  } else {
    CHECK(block_id.shard_full() == shard_id);
    prev = {block_id};
  }

  //LOG(DEBUG) << "before get_validator_set";
  auto val_set = last_masterchain_state_->get_validator_set(shard_id);
  //LOG(DEBUG) << "after get_validator_set: addr=" << (const void*)val_set.get();

  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), last = last_masterchain_block_id_, val_set, prev](td::Result<BlockCandidate> R) {
        if (R.is_ok()) {
          auto v = R.move_as_ok();
          LOG(ERROR) << "created block " << v.id;
          td::actor::send_closure(SelfId, &ValidatorManagerImpl::validate_fake, std::move(v), std::move(prev), last,
                                  val_set);
        } else {
          LOG(ERROR) << "failed to create block: " << R.move_as_error();
          std::exit(2);
        }
      });

  LOG(ERROR) << "running collate query";
  if (local_id_.is_zero()) {
    //td::as<td::uint32>(created_by_.data() + 32 - 4) = ((unsigned)std::time(nullptr) >> 8);
  }
  Ed25519_PublicKey created_by{td::Bits256::zero()};
  td::as<td::uint32>(created_by.as_bits256().data() + 32 - 4) = ((unsigned)std::time(nullptr) >> 8);
  run_collate_query(shard_id, last_masterchain_block_id_, prev, created_by, val_set, td::Ref<CollatorOptions>{true},
                    actor_id(this), td::Timestamp::in(10.0), std::move(P), adnl::AdnlNodeIdShort::zero(),
                    td::CancellationToken{}, 0);
}

void ValidatorManagerImpl::validate_fake(BlockCandidate candidate, std::vector<BlockIdExt> prev, BlockIdExt last,
                                         td::Ref<ValidatorSet> val_set) {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), c = candidate.clone(), prev, last,
                                       val_set](td::Result<ValidateCandidateResult> R) mutable {
    if (R.is_ok()) {
      auto v = R.move_as_ok();
      v.visit(td::overloaded(
          [&](UnixTime ts) {
            td::actor::send_closure(SelfId, &ValidatorManagerImpl::write_fake, std::move(c), prev, last, val_set);
          },
          [&](CandidateReject reject) {
            LOG(ERROR) << "failed to create block: " << reject.reason;
            std::exit(2);
          }));
    } else {
      LOG(ERROR) << "failed to create block: " << R.move_as_error();
      std::exit(2);
    }
  });
  auto shard = candidate.id.shard_full();
  run_validate_query(shard, last, prev, std::move(candidate), std::move(val_set), PublicKeyHash::zero(), actor_id(this),
                     td::Timestamp::in(10.0), std::move(P), ValidateMode::fake);
}

void ValidatorManagerImpl::write_fake(BlockCandidate candidate, std::vector<BlockIdExt> prev, BlockIdExt last,
                                      td::Ref<ValidatorSet> val_set) {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), id = candidate.id](td::Result<td::Unit> R) {
    if (R.is_ok()) {
      td::actor::send_closure(SelfId, &ValidatorManagerImpl::complete_fake, id);
    } else {
      LOG(ERROR) << "failed to create block: " << R.move_as_error();
      std::exit(2);
    }
  });
  auto data = create_block(candidate.id, std::move(candidate.data)).move_as_ok();

  run_fake_accept_block_query(candidate.id, data, prev, val_set, actor_id(this), std::move(P));
}

void ValidatorManagerImpl::complete_fake(BlockIdExt block_id) {
  LOG(ERROR) << "success, block " << block_id << " = " << block_id.to_str() << " saved to disk";
  std::exit(0);
}

void ValidatorManagerImpl::get_next_block(BlockIdExt block_id, td::Promise<BlockHandle> promise) {
  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), promise = std::move(promise)](td::Result<BlockHandle> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
          return;
        }
        auto handle = R.move_as_ok();
        if (!handle->inited_next_left()) {
          promise.set_error(td::Status::Error(ErrorCode::notready, "next block not known"));
          return;
        }

        td::actor::send_closure(SelfId, &ValidatorManagerImpl::get_block_handle, handle->one_next(true), true,
                                std::move(promise));
      });

  get_block_handle(block_id, false, std::move(P));
}

void ValidatorManagerImpl::get_block_data(BlockHandle handle, td::Promise<td::BufferSlice> promise) {
  auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<td::Ref<BlockData>> R) mutable {
    if (R.is_error()) {
      promise.set_error(R.move_as_error());
    } else {
      auto B = R.move_as_ok();
      promise.set_value(B->data());
    }
  });

  get_block_data_from_db(handle, std::move(P));
}

void ValidatorManagerImpl::check_zero_state_exists(BlockIdExt block_id, td::Promise<bool> promise) {
  td::actor::send_closure(db_, &Db::check_zero_state_file_exists, block_id, std::move(promise));
}
void ValidatorManagerImpl::get_zero_state(BlockIdExt block_id, td::Promise<td::BufferSlice> promise) {
  td::actor::send_closure(db_, &Db::get_zero_state_file, block_id, std::move(promise));
}

void ValidatorManagerImpl::get_persistent_state_size(BlockIdExt block_id, BlockIdExt masterchain_block_id,
                                                     PersistentStateType type, td::Promise<td::uint64> promise) {
  td::actor::send_closure(db_, &Db::get_persistent_state_file_size, block_id, masterchain_block_id, type,
                          std::move(promise));
}
void ValidatorManagerImpl::get_persistent_state(BlockIdExt block_id, BlockIdExt masterchain_block_id,
                                                PersistentStateType type, td::Promise<td::BufferSlice> promise) {
  td::actor::send_closure(db_, &Db::get_persistent_state_file, block_id, masterchain_block_id, type,
                          std::move(promise));
}

void ValidatorManagerImpl::get_block_proof(BlockHandle handle, td::Promise<td::BufferSlice> promise) {
  auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<td::Ref<Proof>> R) mutable {
    if (R.is_error()) {
      promise.set_error(R.move_as_error());
    } else {
      auto B = R.move_as_ok();
      promise.set_value(B->data());
    }
  });

  td::actor::send_closure(db_, &Db::get_block_proof, handle, std::move(P));
}

void ValidatorManagerImpl::get_key_block_proof(BlockIdExt block_id, td::Promise<td::BufferSlice> promise) {
  auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<td::Ref<Proof>> R) mutable {
    if (R.is_error()) {
      promise.set_error(R.move_as_error());
    } else {
      auto B = R.move_as_ok();
      promise.set_value(B->data());
    }
  });

  td::actor::send_closure(db_, &Db::get_key_block_proof, block_id, std::move(P));
}

void ValidatorManagerImpl::get_key_block_proof_link(BlockIdExt block_id, td::Promise<td::BufferSlice> promise) {
  auto P = td::PromiseCreator::lambda(
      [promise = std::move(promise), block_id, db = db_.get()](td::Result<td::Ref<Proof>> R) mutable {
        if (R.is_error()) {
          auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<td::Ref<Proof>> R) mutable {
            if (R.is_error()) {
              promise.set_error(R.move_as_error());
            } else {
              auto B = R.move_as_ok();
              promise.set_value(B->data());
            }
          });

          td::actor::send_closure(db, &Db::get_key_block_proof, block_id, std::move(P));
        } else {
          auto B = R.move_as_ok()->export_as_proof_link().move_as_ok();
          promise.set_value(B->data());
        }
      });

  td::actor::send_closure(db_, &Db::get_key_block_proof, block_id, std::move(P));
}

void ValidatorManagerImpl::new_external_message(td::BufferSlice data, int priority) {
  if (last_masterchain_state_.is_null()) {
    return;
  }
  auto R = create_ext_message(std::move(data), last_masterchain_state_->get_ext_msg_limits());
  if (R.is_ok()) {
    ext_messages_.emplace_back(R.move_as_ok());
  }
}

void ValidatorManagerImpl::new_ihr_message(td::BufferSlice data) {
  auto R = create_ihr_message(std::move(data));
  if (R.is_ok()) {
    ihr_messages_.emplace_back(R.move_as_ok());
  }
}

void ValidatorManagerImpl::new_shard_block_description_broadcast(BlockIdExt block_id, CatchainSeqno cc_seqno,
                                                                 td::BufferSlice data) {
  if (!last_masterchain_block_handle_) {
    shard_blocks_raw_.push_back(std::move(data));
    return;
  }
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<ShardTopBlockDescription>> R) {
    if (R.is_error()) {
      LOG(WARNING) << "dropping invalid new shard block description: " << R.move_as_error();
      td::actor::send_closure(SelfId, &ValidatorManagerImpl::dec_pending_new_blocks);
    } else {
      // LOG(DEBUG) << "run_validate_shard_block_description() completed successfully";
      td::actor::send_closure(SelfId, &ValidatorManagerImpl::add_shard_block_description, R.move_as_ok());
    }
  });
  ++pending_new_shard_block_descr_;
  // LOG(DEBUG) << "new run_validate_shard_block_description()";
  run_validate_shard_block_description(std::move(data), last_masterchain_block_handle_, last_masterchain_state_,
                                       actor_id(this), td::Timestamp::in(2.0), std::move(P), true);
}

void ValidatorManagerImpl::add_shard_block_description(td::Ref<ShardTopBlockDescription> desc) {
  if (desc->may_be_valid(last_masterchain_block_handle_, last_masterchain_state_)) {
    shard_blocks_.insert(std::move(desc));
  }
  dec_pending_new_blocks();
}

void ValidatorManagerImpl::dec_pending_new_blocks() {
  if (!--pending_new_shard_block_descr_ && !waiting_new_shard_block_descr_.empty()) {
    std::vector<td::Ref<ShardTopBlockDescription>> res{shard_blocks_.begin(), shard_blocks_.end()};
    auto promises = std::move(waiting_new_shard_block_descr_);
    waiting_new_shard_block_descr_.clear();
    for (auto &promise : promises) {
      promise.set_result(res);
    }
  }
}

void ValidatorManagerImpl::wait_block_state(BlockHandle handle, td::uint32 priority, td::Timestamp timeout,
                                            td::Promise<td::Ref<ShardState>> promise) {
  auto it = wait_state_.find(handle->id());
  if (it == wait_state_.end()) {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), handle](td::Result<td::Ref<ShardState>> R) {
      td::actor::send_closure(SelfId, &ValidatorManagerImpl::finished_wait_state, handle->id(), std::move(R));
    });
    auto id = td::actor::create_actor<WaitBlockState>("waitstate", handle, 0, opts_, last_masterchain_state_,
                                                      actor_id(this), td::Timestamp::in(10.0), std::move(P))
                  .release();
    wait_state_[handle->id()].actor_ = id;
    it = wait_state_.find(handle->id());
  }

  it->second.waiting_.emplace_back(
      std::pair<td::Timestamp, td::Promise<td::Ref<ShardState>>>(timeout, std::move(promise)));
  td::actor::send_closure(it->second.actor_, &WaitBlockState::update_timeout, timeout, 0);
}

void ValidatorManagerImpl::wait_block_state_short(BlockIdExt block_id, td::uint32 priority, td::Timestamp timeout,
                                                  td::Promise<td::Ref<ShardState>> promise) {
  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), timeout, promise = std::move(promise)](td::Result<BlockHandle> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
          return;
        }
        td::actor::send_closure(SelfId, &ValidatorManagerImpl::wait_block_state, R.move_as_ok(), 0, timeout,
                                std::move(promise));
      });
  get_block_handle(block_id, true, std::move(P));
}

void ValidatorManagerImpl::wait_block_data(BlockHandle handle, td::uint32 priority, td::Timestamp timeout,
                                           td::Promise<td::Ref<BlockData>> promise) {
  auto it = wait_block_data_.find(handle->id());
  if (it == wait_block_data_.end()) {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), handle](td::Result<td::Ref<BlockData>> R) {
      td::actor::send_closure(SelfId, &ValidatorManagerImpl::finished_wait_data, handle->id(), std::move(R));
    });
    auto id = td::actor::create_actor<WaitBlockDataDisk>("waitdata", handle, actor_id(this), td::Timestamp::in(10.0),
                                                         std::move(P))
                  .release();
    wait_block_data_[handle->id()].actor_ = id;
    it = wait_block_data_.find(handle->id());
  }

  it->second.waiting_.emplace_back(
      std::pair<td::Timestamp, td::Promise<td::Ref<BlockData>>>(timeout, std::move(promise)));
  td::actor::send_closure(it->second.actor_, &WaitBlockDataDisk::update_timeout, timeout);
}

void ValidatorManagerImpl::wait_block_data_short(BlockIdExt block_id, td::uint32 priority, td::Timestamp timeout,
                                                 td::Promise<td::Ref<BlockData>> promise) {
  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), timeout, promise = std::move(promise)](td::Result<BlockHandle> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
          return;
        }
        td::actor::send_closure(SelfId, &ValidatorManagerImpl::wait_block_data, R.move_as_ok(), 0, timeout,
                                std::move(promise));
      });
  get_block_handle(block_id, true, std::move(P));
}

void ValidatorManagerImpl::wait_block_state_merge(BlockIdExt left_id, BlockIdExt right_id, td::uint32 priority,
                                                  td::Timestamp timeout, td::Promise<td::Ref<ShardState>> promise) {
  td::actor::create_actor<WaitBlockStateMerge>("merge", left_id, right_id, 0, actor_id(this), timeout,
                                               std::move(promise))
      .release();
}

void ValidatorManagerImpl::wait_prev_block_state(BlockHandle handle, td::uint32 priority, td::Timestamp timeout,
                                                 td::Promise<td::Ref<ShardState>> promise) {
  CHECK(handle);
  CHECK(!handle->is_zero());
  if (!handle->merge_before()) {
    auto shard = handle->id().shard_full();
    auto prev_shard = handle->one_prev(true).shard_full();
    if (shard == prev_shard) {
      wait_block_state_short(handle->one_prev(true), 0, timeout, std::move(promise));
    } else {
      CHECK(shard_parent(shard) == prev_shard);
      bool left = shard_child(prev_shard, true) == shard;
      auto P =
          td::PromiseCreator::lambda([promise = std::move(promise), left](td::Result<td::Ref<ShardState>> R) mutable {
            if (R.is_error()) {
              promise.set_error(R.move_as_error());
            } else {
              auto s = R.move_as_ok();
              auto r = s->split();
              if (r.is_error()) {
                promise.set_error(r.move_as_error());
              } else {
                auto v = r.move_as_ok();
                promise.set_value(left ? std::move(v.first) : std::move(v.second));
              }
            }
          });
      wait_block_state_short(handle->one_prev(true), 0, timeout, std::move(P));
    }
  } else {
    wait_block_state_merge(handle->one_prev(true), handle->one_prev(false), 0, timeout, std::move(promise));
  }
}

void ValidatorManagerImpl::wait_block_proof(BlockHandle handle, td::Timestamp timeout,
                                            td::Promise<td::Ref<Proof>> promise) {
  td::actor::send_closure(db_, &Db::get_block_proof, handle, std::move(promise));
}

void ValidatorManagerImpl::wait_block_proof_short(BlockIdExt block_id, td::Timestamp timeout,
                                                  td::Promise<td::Ref<Proof>> promise) {
  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), timeout, promise = std::move(promise)](td::Result<BlockHandle> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
          return;
        }
        td::actor::send_closure(SelfId, &ValidatorManagerImpl::wait_block_proof, R.move_as_ok(), timeout,
                                std::move(promise));
      });
  get_block_handle(block_id, true, std::move(P));
}

void ValidatorManagerImpl::wait_block_proof_link(BlockHandle handle, td::Timestamp timeout,
                                                 td::Promise<td::Ref<ProofLink>> promise) {
  td::actor::send_closure(db_, &Db::get_block_proof_link, std::move(handle), std::move(promise));
}

void ValidatorManagerImpl::wait_block_proof_link_short(BlockIdExt block_id, td::Timestamp timeout,
                                                       td::Promise<td::Ref<ProofLink>> promise) {
  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), timeout, promise = std::move(promise)](td::Result<BlockHandle> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
          return;
        }
        td::actor::send_closure(SelfId, &ValidatorManagerImpl::wait_block_proof_link, R.move_as_ok(), timeout,
                                std::move(promise));
      });
  get_block_handle(block_id, true, std::move(P));
}

void ValidatorManagerImpl::wait_block_signatures(BlockHandle handle, td::Timestamp timeout,
                                                 td::Promise<td::Ref<BlockSignatureSet>> promise) {
  td::actor::send_closure(db_, &Db::get_block_signatures, handle, std::move(promise));
}

void ValidatorManagerImpl::wait_block_signatures_short(BlockIdExt block_id, td::Timestamp timeout,
                                                       td::Promise<td::Ref<BlockSignatureSet>> promise) {
  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), timeout, promise = std::move(promise)](td::Result<BlockHandle> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
          return;
        }
        td::actor::send_closure(SelfId, &ValidatorManagerImpl::wait_block_signatures, R.move_as_ok(), timeout,
                                std::move(promise));
      });
  get_block_handle(block_id, true, std::move(P));
}

void ValidatorManagerImpl::wait_block_message_queue(BlockHandle handle, td::uint32 priority, td::Timestamp timeout,
                                                    td::Promise<td::Ref<MessageQueue>> promise) {
  auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<td::Ref<ShardState>> R) mutable {
    if (R.is_error()) {
      promise.set_error(R.move_as_error());
    } else {
      auto state = R.move_as_ok();
      promise.set_result(state->message_queue());
    }
  });

  wait_block_state(handle, 0, timeout, std::move(P));
}

void ValidatorManagerImpl::wait_block_message_queue_short(BlockIdExt block_id, td::uint32 priority,
                                                          td::Timestamp timeout,
                                                          td::Promise<td::Ref<MessageQueue>> promise) {
  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), timeout, promise = std::move(promise)](td::Result<BlockHandle> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
          return;
        }
        td::actor::send_closure(SelfId, &ValidatorManagerImpl::wait_block_message_queue, R.move_as_ok(), 0, timeout,
                                std::move(promise));
      });
  get_block_handle(block_id, true, std::move(P));
}

void ValidatorManagerImpl::get_external_messages(
    ShardIdFull shard, td::Promise<std::vector<std::pair<td::Ref<ExtMessage>, int>>> promise) {
  std::vector<std::pair<td::Ref<ExtMessage>, int>> res;
  for (const auto& x : ext_messages_) {
    res.emplace_back(x, 0);
  }
  promise.set_result(std::move(res));
}

void ValidatorManagerImpl::get_ihr_messages(ShardIdFull shard, td::Promise<std::vector<td::Ref<IhrMessage>>> promise) {
  promise.set_result(ihr_messages_);
}

void ValidatorManagerImpl::get_shard_blocks_for_collator(
    BlockIdExt masterchain_block_id, td::Promise<std::vector<td::Ref<ShardTopBlockDescription>>> promise) {
  if (!last_masterchain_block_handle_) {
    promise.set_result(std::vector<td::Ref<ShardTopBlockDescription>>{});
    return;
  }
  if (!shard_blocks_raw_.empty()) {
    for (auto &raw : shard_blocks_raw_) {
      new_shard_block_description_broadcast(BlockIdExt{}, 0, std::move(raw));
    }
    shard_blocks_raw_.clear();
  }
  if (!pending_new_shard_block_descr_) {
    promise.set_result(std::vector<td::Ref<ShardTopBlockDescription>>{shard_blocks_.begin(), shard_blocks_.end()});
  } else {
    // LOG(DEBUG) << "postponed get_shard_blocks query because pending_new_shard_block_descr_=" << pending_new_shard_block_descr_;
    waiting_new_shard_block_descr_.push_back(std::move(promise));
  }
}

void ValidatorManagerImpl::complete_external_messages(std::vector<ExtMessage::Hash> to_delay,
                                                      std::vector<ExtMessage::Hash> to_delete) {
}

void ValidatorManagerImpl::complete_ihr_messages(std::vector<IhrMessage::Hash> to_delay,
                                                 std::vector<IhrMessage::Hash> to_delete) {
}

void ValidatorManagerImpl::get_block_data_from_db(ConstBlockHandle handle, td::Promise<td::Ref<BlockData>> promise) {
  td::actor::send_closure(db_, &Db::get_block_data, handle, std::move(promise));
}

void ValidatorManagerImpl::get_block_data_from_db_short(BlockIdExt block_id, td::Promise<td::Ref<BlockData>> promise) {
  auto P =
      td::PromiseCreator::lambda([db = db_.get(), promise = std::move(promise)](td::Result<BlockHandle> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
        } else {
          auto handle = R.move_as_ok();
          td::actor::send_closure(db, &Db::get_block_data, std::move(handle), std::move(promise));
        }
      });
  get_block_handle(block_id, false, std::move(P));
}

void ValidatorManagerImpl::get_shard_state_from_db(ConstBlockHandle handle, td::Promise<td::Ref<ShardState>> promise) {
  td::actor::send_closure(db_, &Db::get_block_state, handle, std::move(promise));
}

void ValidatorManagerImpl::get_shard_state_root_cell_from_db(ConstBlockHandle handle,
                                                             td::Promise<td::Ref<vm::DataCell>> promise) {
  td::actor::send_closure(db_, &Db::get_block_state_root_cell, handle, std::move(promise));
}

void ValidatorManagerImpl::get_shard_state_from_db_short(BlockIdExt block_id,
                                                         td::Promise<td::Ref<ShardState>> promise) {
  if (read_only_) {
    auto P =
        td::PromiseCreator::lambda([db = db_.get(), promise = std::move(promise)](td::Result<BlockHandle> R) mutable {
          if (R.is_error()) {
            promise.set_error(R.move_as_error());
          } else {
            auto handle = R.move_as_ok();
            td::actor::send_closure(db, &Db::get_block_state, std::move(handle), std::move(promise));
          }
        });
    get_block_handle(block_id, false, std::move(P));
  } else {
    auto P = td::PromiseCreator::lambda(
        [SelfId = actor_id(this), db = db_.get(), promise = std::move(promise)](td::Result<BlockHandle> R) mutable {
          if (R.is_error()) {
            promise.set_error(R.move_as_error());
          } else {
            auto handle = R.move_as_ok();
            td::actor::send_closure(db, &Db::get_block_state, std::move(handle), std::move(promise));
          }
        });
    get_block_handle(block_id, false, std::move(P));
  }
}

void ValidatorManagerImpl::get_block_candidate_from_db(PublicKey source, BlockIdExt id,
                                                       FileHash collated_data_file_hash,
                                                       td::Promise<BlockCandidate> promise) {
  td::actor::send_closure(db_, &Db::get_block_candidate, source, id, collated_data_file_hash, std::move(promise));
}

void ValidatorManagerImpl::get_candidate_data_by_block_id_from_db(BlockIdExt id, td::Promise<td::BufferSlice> promise) {
  td::actor::send_closure(db_, &Db::get_block_candidate_by_block_id, id,
                          promise.wrap([](BlockCandidate &&b) { return std::move(b.data); }));
}

void ValidatorManagerImpl::get_block_proof_from_db(ConstBlockHandle handle, td::Promise<td::Ref<Proof>> promise) {
  td::actor::send_closure(db_, &Db::get_block_proof, std::move(handle), std::move(promise));
}

void ValidatorManagerImpl::get_block_proof_from_db_short(BlockIdExt block_id, td::Promise<td::Ref<Proof>> promise) {
  auto P =
      td::PromiseCreator::lambda([db = db_.get(), promise = std::move(promise)](td::Result<BlockHandle> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
        } else {
          auto handle = R.move_as_ok();
          td::actor::send_closure(db, &Db::get_block_proof, std::move(handle), std::move(promise));
        }
      });
  get_block_handle(block_id, false, std::move(P));
}

void ValidatorManagerImpl::get_block_proof_link_from_db(ConstBlockHandle handle,
                                                        td::Promise<td::Ref<ProofLink>> promise) {
  td::actor::send_closure(db_, &Db::get_block_proof_link, std::move(handle), std::move(promise));
}

void ValidatorManagerImpl::get_block_proof_link_from_db_short(BlockIdExt block_id,
                                                              td::Promise<td::Ref<ProofLink>> promise) {
  auto P =
      td::PromiseCreator::lambda([db = db_.get(), promise = std::move(promise)](td::Result<BlockHandle> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
        } else {
          auto handle = R.move_as_ok();
          td::actor::send_closure(db, &Db::get_block_proof_link, std::move(handle), std::move(promise));
        }
      });
  get_block_handle(block_id, false, std::move(P));
}

void ValidatorManagerImpl::get_block_by_lt_from_db(AccountIdPrefixFull account, LogicalTime lt,
                                                   td::Promise<ConstBlockHandle> promise) {
  td::actor::send_closure(db_, &Db::get_block_by_lt, account, lt, std::move(promise));
}

void ValidatorManagerImpl::get_block_by_unix_time_from_db(AccountIdPrefixFull account, UnixTime ts,
                                                          td::Promise<ConstBlockHandle> promise) {
  td::actor::send_closure(db_, &Db::get_block_by_unix_time, account, ts, std::move(promise));
}

void ValidatorManagerImpl::get_block_by_seqno_from_db(AccountIdPrefixFull account, BlockSeqno seqno,
                                                      td::Promise<ConstBlockHandle> promise) {
  td::actor::send_closure(db_, &Db::get_block_by_seqno, account, seqno, std::move(promise));
}

void ValidatorManagerImpl::finished_wait_state(BlockIdExt block_id, td::Result<td::Ref<ShardState>> R) {
  auto it = wait_state_.find(block_id);
  if (it != wait_state_.end()) {
    if (R.is_error()) {
      auto S = R.move_as_error();
      for (auto &X : it->second.waiting_) {
        X.second.set_error(S.clone());
      }
    } else {
      auto r = R.move_as_ok();
      for (auto &X : it->second.waiting_) {
        X.second.set_result(r);
      }
    }
    wait_state_.erase(it);
  }
}

void ValidatorManagerImpl::finished_wait_data(BlockIdExt block_id, td::Result<td::Ref<BlockData>> R) {
  auto it = wait_block_data_.find(block_id);
  if (it != wait_block_data_.end()) {
    if (R.is_error()) {
      auto S = R.move_as_error();
      for (auto &X : it->second.waiting_) {
        X.second.set_error(S.clone());
      }
    } else {
      auto r = R.move_as_ok();
      for (auto &X : it->second.waiting_) {
        X.second.set_result(r);
      }
    }
    wait_block_data_.erase(it);
  }
}

void ValidatorManagerImpl::set_block_state(BlockHandle handle, td::Ref<ShardState> state,
                                           td::Promise<td::Ref<ShardState>> promise) {
  td::actor::send_closure(db_, &Db::store_block_state, handle, state, std::move(promise));
}

void ValidatorManagerImpl::store_block_state_part(BlockId effective_block, td::Ref<vm::Cell> cell,
                                                  td::Promise<td::Ref<vm::DataCell>> promise) {
  td::actor::send_closure(db_, &Db::store_block_state_part, effective_block, cell, std::move(promise));
}

void ValidatorManagerImpl::set_block_state_from_data(BlockHandle handle, td::Ref<BlockData> block,
                                                     td::Promise<td::Ref<ShardState>> promise) {
  td::actor::send_closure(db_, &Db::store_block_state_from_data, handle, block, std::move(promise));
}

void ValidatorManagerImpl::set_block_state_from_data_preliminary(std::vector<td::Ref<BlockData>> blocks,
                                                                 td::Promise<td::Unit> promise) {
  td::actor::send_closure(db_, &Db::store_block_state_from_data_preliminary, std::move(blocks), std::move(promise));
}

void ValidatorManagerImpl::get_cell_db_reader(td::Promise<std::shared_ptr<vm::CellDbReader>> promise) {
  td::actor::send_closure(db_, &Db::get_cell_db_reader, std::move(promise));
}

void ValidatorManagerImpl::store_persistent_state_file(BlockIdExt block_id, BlockIdExt masterchain_block_id,
                                                       PersistentStateType type, td::BufferSlice state,
                                                       td::Promise<td::Unit> promise) {
  td::actor::send_closure(db_, &Db::store_persistent_state_file, block_id, masterchain_block_id, type, std::move(state),
                          std::move(promise));
}

void ValidatorManagerImpl::store_persistent_state_file_gen(BlockIdExt block_id, BlockIdExt masterchain_block_id,
                                                           PersistentStateType type,
                                                           std::function<td::Status(td::FileFd &)> write_data,
                                                           td::Promise<td::Unit> promise) {
  td::actor::send_closure(db_, &Db::store_persistent_state_file_gen, block_id, masterchain_block_id, type,
                          std::move(write_data), std::move(promise));
}

void ValidatorManagerImpl::store_zero_state_file(BlockIdExt block_id, td::BufferSlice state,
                                                 td::Promise<td::Unit> promise) {
  td::actor::send_closure(db_, &Db::store_zero_state_file, block_id, std::move(state), std::move(promise));
}

void ValidatorManagerImpl::set_block_data(BlockHandle handle, td::Ref<BlockData> data, td::Promise<td::Unit> promise) {
  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), promise = std::move(promise), handle](td::Result<td::Unit> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
        } else {
          handle->set_received();
          handle->flush(SelfId, handle, std::move(promise));
        }
      });

  td::actor::send_closure(db_, &Db::store_block_data, handle, std::move(data), std::move(P));
}

void ValidatorManagerImpl::set_block_proof(BlockHandle handle, td::Ref<Proof> proof, td::Promise<td::Unit> promise) {
  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), promise = std::move(promise), handle](td::Result<td::Unit> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
        } else {
          promise.set_value(td::Unit());
        }
      });

  td::actor::send_closure(db_, &Db::store_block_proof, handle, std::move(proof), std::move(P));
}

void ValidatorManagerImpl::set_block_proof_link(BlockHandle handle, td::Ref<ProofLink> proof,
                                                td::Promise<td::Unit> promise) {
  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), handle, promise = std::move(promise)](td::Result<td::Unit> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
        } else {
          promise.set_value(td::Unit());
        }
      });
  td::actor::send_closure(db_, &Db::store_block_proof_link, handle, std::move(proof), std::move(P));
}

void ValidatorManagerImpl::set_block_signatures(BlockHandle handle, td::Ref<BlockSignatureSet> signatures,
                                                td::Promise<td::Unit> promise) {
  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), promise = std::move(promise), handle](td::Result<td::Unit> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
        } else {
          promise.set_value(td::Unit());
        }
      });

  td::actor::send_closure(db_, &Db::store_block_signatures, handle, std::move(signatures), std::move(P));
}

void ValidatorManagerImpl::set_next_block(BlockIdExt block_id, BlockIdExt next, td::Promise<td::Unit> promise) {
  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), next, promise = std::move(promise)](td::Result<BlockHandle> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
        } else {
          auto handle = R.move_as_ok();
          handle->set_next(next);
          if (handle->need_flush()) {
            handle->flush(SelfId, handle, std::move(promise));
          } else {
            promise.set_value(td::Unit());
          }
        }
      });
  get_block_handle(block_id, true, std::move(P));
}

void ValidatorManagerImpl::set_block_candidate(BlockIdExt id, BlockCandidate candidate, CatchainSeqno cc_seqno,
                                               td::uint32 validator_set_hash, td::Promise<td::Unit> promise) {
  td::actor::send_closure(db_, &Db::store_block_candidate, std::move(candidate), std::move(promise));
}

void ValidatorManagerImpl::send_block_candidate_broadcast(BlockIdExt id, CatchainSeqno cc_seqno,
                                                          td::uint32 validator_set_hash, td::BufferSlice data,
                                                          int mode) {
  callback_->send_block_candidate(id, cc_seqno, validator_set_hash, std::move(data), mode);
}

void ValidatorManagerImpl::write_handle(BlockHandle handle, td::Promise<td::Unit> promise) {
  td::actor::send_closure(db_, &Db::store_block_handle, std::move(handle), std::move(promise));
}

void ValidatorManagerImpl::new_block_cont(BlockHandle handle, td::Ref<ShardState> state,
                                          td::Promise<td::Unit> promise) {
  handle->set_processed();
  if (state->get_shard().is_masterchain() && handle->id().id.seqno > last_masterchain_seqno_) {
    CHECK(handle->id().id.seqno == last_masterchain_seqno_ + 1);
    last_masterchain_seqno_ = handle->id().id.seqno;
    last_masterchain_state_ = td::Ref<MasterchainState>{state};
    last_masterchain_block_id_ = handle->id();
    last_masterchain_block_handle_ = handle;

    update_shards();
    update_shard_blocks();

    td::actor::send_closure(db_, &Db::update_init_masterchain_block, last_masterchain_block_id_, std::move(promise));
  } else {
    promise.set_value(td::Unit());
  }
}

void ValidatorManagerImpl::new_block(BlockHandle handle, td::Ref<ShardState> state, td::Promise<td::Unit> promise) {
  if (handle->is_applied()) {
    return new_block_cont(std::move(handle), std::move(state), std::move(promise));
  } else {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), handle, state = std::move(state),
                                         promise = std::move(promise)](td::Result<td::Unit> R) mutable {
      if (R.is_error()) {
        promise.set_error(R.move_as_error());
      } else {
        td::actor::send_closure(SelfId, &ValidatorManagerImpl::new_block_cont, std::move(handle), std::move(state),
                                std::move(promise));
      }
    });
    td::actor::send_closure(db_, &Db::apply_block, handle, std::move(P));
  }
}

void ValidatorManagerImpl::add_lite_query_stats_extended(int lite_query_id, adnl::AdnlNodeIdShort dst, long start_at,
                                                         long end_at, bool success) {
  td::actor::send_closure(lslimiter_, &liteserver::LiteServerLimiter::add_lite_query_stats, lite_query_id, dst,
                          start_at, end_at, success);
}

void ValidatorManagerImpl::get_block_handle(BlockIdExt id, bool force, td::Promise<BlockHandle> promise) {
  auto it = handles_.find(id);
  if (it != handles_.end()) {
    auto handle = it->second.lock();
    if (handle) {
      promise.set_value(std::move(handle));
      return;
    } else {
      handles_.erase(it);
    }
  }
  auto P = td::PromiseCreator::lambda(
      [id, force, promise = std::move(promise), SelfId = actor_id(this)](td::Result<BlockHandle> R) mutable {
        BlockHandle handle;
        if (R.is_error()) {
          auto S = R.move_as_error();
          if (S.code() == ErrorCode::notready && force) {
            handle = create_empty_block_handle(id);
          } else {
            promise.set_error(std::move(S));
            return;
          }
        } else {
          handle = R.move_as_ok();
        }
        td::actor::send_closure(SelfId, &ValidatorManagerImpl::register_block_handle, std::move(handle),
                                std::move(promise));
      });

  td::actor::send_closure(db_, &Db::get_block_handle, id, std::move(P));
}

void ValidatorManagerImpl::register_block_handle(BlockHandle handle, td::Promise<BlockHandle> promise) {
  auto it = handles_.find(handle->id());
  if (it != handles_.end()) {
    auto h = it->second.lock();
    if (h) {
      promise.set_value(std::move(h));
      return;
    }
    handles_.erase(it);
  }
  handles_.emplace(handle->id(), std::weak_ptr<BlockHandleInterface>(handle));
  promise.set_value(std::move(handle));
}

void ValidatorManagerImpl::get_top_masterchain_state(td::Promise<td::Ref<MasterchainState>> promise) {
  promise.set_result(last_masterchain_state_);
}

void ValidatorManagerImpl::get_top_masterchain_block(td::Promise<BlockIdExt> promise) {
  promise.set_result(last_masterchain_block_id_);
}

void ValidatorManagerImpl::get_top_masterchain_state_block(
    td::Promise<std::pair<td::Ref<MasterchainState>, BlockIdExt>> promise) {
  promise.set_result(
      std::pair<td::Ref<MasterchainState>, BlockIdExt>{last_masterchain_state_, last_masterchain_block_id_});
}

void ValidatorManagerImpl::get_last_liteserver_state_block(
    td::Promise<std::pair<td::Ref<MasterchainState>, BlockIdExt>> promise) {
  promise.set_result(
      std::pair<td::Ref<MasterchainState>, BlockIdExt>{last_liteserver_state_, last_liteserver_block_id_});
}

void ValidatorManagerImpl::send_get_block_request(BlockIdExt id, td::uint32 priority,
                                                  td::Promise<ReceivedBlock> promise) {
  UNREACHABLE();
}

void ValidatorManagerImpl::send_get_zero_state_request(BlockIdExt id, td::uint32 priority,
                                                       td::Promise<td::BufferSlice> promise) {
  UNREACHABLE();
}

void ValidatorManagerImpl::send_get_persistent_state_request(BlockIdExt id, BlockIdExt masterchain_block_id,
                                                             PersistentStateType type, td::uint32 priority,
                                                             td::Promise<td::BufferSlice> promise) {
  UNREACHABLE();
}

void ValidatorManagerImpl::send_top_shard_block_description(td::Ref<ShardTopBlockDescription> desc) {
  callback_->send_shard_block_info(desc->block_id(), desc->catchain_seqno(), desc->serialize());
}

void ValidatorManagerImpl::start_up() {
  db_ = create_db_actor(actor_id(this), db_root_, opts_, read_only_);
  // Need to detect last mc block with shards
  shardclientdetector_ = td::actor::create_actor<ShardClientDetector>("ShardClientDetector", actor_id(this)).release();

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<ValidatorManagerInitResult> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &ValidatorManagerImpl::started, R.move_as_ok(), false);
  });

  if (!offline_) {
    auto Q =
        td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::actor::ActorOwn<adnl::AdnlExtServer>> R) {
          R.ensure();
          td::actor::send_closure(SelfId, &ValidatorManagerImpl::created_ext_server, R.move_as_ok());
        });
    td::actor::send_closure(adnl_, &adnl::Adnl::create_ext_server, std::vector<adnl::AdnlNodeIdShort>{},
                            std::vector<td::uint16>{}, std::move(Q));

    lite_server_cache_ = create_liteserver_cache_actor(actor_id(this), db_root_);
  }

  validator_manager_init(opts_, actor_id(this), db_.get(), std::move(P), read_only_);

  if (!offline_) {
    delay_action([SelfId = actor_id(this)]() { td::actor::send_closure(SelfId, &ValidatorManagerImpl::reinit); },
                 td::Timestamp::in(1.0));
  }
}

void ValidatorManagerImpl::reinit() {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<ValidatorManagerInitResult> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &ValidatorManagerImpl::started, R.move_as_ok(), true);

    delay_action([SelfId]() { td::actor::send_closure(SelfId, &ValidatorManagerImpl::reinit); },
                 td::Timestamp::in(0.3));
  });

  validator_manager_init(opts_, actor_id(this), db_.get(), std::move(P), read_only_);
}

void ValidatorManagerImpl::created_ext_server(td::actor::ActorOwn<adnl::AdnlExtServer> server) {
  if (offline_) {
    UNREACHABLE();
  } else {
    lite_server_ = std::move(server);
    for (auto &id : pending_ext_ids_) {
      td::actor::send_closure(lite_server_, &adnl::AdnlExtServer::add_local_id, id);
    }
    for (auto port : pending_ext_ports_) {
      td::actor::send_closure(lite_server_, &adnl::AdnlExtServer::add_tcp_port, port);
    }
    pending_ext_ids_.clear();
    pending_ext_ports_.clear();
  }
}
void ValidatorManagerImpl::wait_shard_client_state(BlockSeqno seqno, td::Timestamp timeout,
                                                   td::Promise<td::Unit> promise)  {
  if (seqno <= last_masterchain_seqno_) {
    promise.set_value(td::Unit());
    return;
  }
  if (timeout.is_in_past()) {
    promise.set_error(td::Status::Error(ErrorCode::timeout, "timeout"));
    return;
  }
  if (seqno > last_masterchain_seqno_ + 100) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "too big masterchain block seqno"));
    return;
  }

  shard_client_waiters_[seqno].waiting_.emplace_back(timeout, 0, std::move(promise));
}

void ValidatorManagerImpl::run_ext_query_extended(td::BufferSlice data, adnl::AdnlNodeIdShort dst,
                                                  td::Promise<td::BufferSlice> promise) {
  if (offline_) {
    UNREACHABLE();
  } else {
    auto F = fetch_tl_object<lite_api::liteServer_query>(data.clone(), true);
    if (F.is_ok()) {
      data = std::move(F.move_as_ok()->data_);
    } else {
      auto G = fetch_tl_prefix<lite_api::liteServer_queryPrefix>(data, true);
      if (G.is_error()) {
        promise.set_error(G.move_as_error());
        return;
      }
    }

    auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<td::BufferSlice> R) mutable {
      td::BufferSlice data;
      if (R.is_error()) {
        auto S = R.move_as_error();
        data = create_serialize_tl_object<lite_api::liteServer_error>(S.code(), S.message().c_str());
      } else {
        data = R.move_as_ok();
      }
      promise.set_value(std::move(data));
    });

    auto E = fetch_tl_prefix<lite_api::liteServer_waitMasterchainSeqno>(data, true);
    if (E.is_error()) {
      run_liteserver_query(std::move(data), actor_id(this), lite_server_cache_.get(), std::move(P), dst);
    } else {
      auto e = E.move_as_ok();
      if (static_cast<BlockSeqno>(e->seqno_) <= last_masterchain_seqno_) {
        run_liteserver_query(std::move(data), actor_id(this), lite_server_cache_.get(), std::move(P), dst);
      } else {
        auto t = e->timeout_ms_ < 10000 ? e->timeout_ms_ * 0.001 : 10.0;
        auto Q = td::PromiseCreator::lambda([data = std::move(data), SelfId = actor_id(this),
                                             cache = lite_server_cache_.get(),
                                             promise = std::move(P)](td::Result<td::Unit> R) mutable {
          if (R.is_error()) {
            promise.set_error(R.move_as_error());
            return;
          }
          run_liteserver_query(std::move(data), SelfId, cache, std::move(promise));
        });
        wait_shard_client_state(e->seqno_, td::Timestamp::in(t), std::move(Q));
      }
    }
  }
}

void ValidatorManagerImpl::receiveLastBlock(td::Result<td::Ref<BlockData>> block_result,
                                            ValidatorManagerInitResult init_result) {
  if (block_result.is_ok()) {
    auto block_data = block_result.move_as_ok();
    auto block_root = block_data->root_cell();
    if (block_root.is_null()) {
      LOG(ERROR) << "block has no valid root cell";
      return;
    }

    last_masterchain_block_handle_ = std::move(init_result.handle);
    last_masterchain_block_id_ = last_masterchain_block_handle_->id();
    last_masterchain_seqno_ = last_masterchain_block_id_.id.seqno;
    last_masterchain_state_ = std::move(init_result.state);

    block::gen::Block::Record blk;
    block::gen::BlockInfo::Record info;
    block::gen::BlockExtra::Record extra;
    block::gen::McBlockExtra::Record extra_mc;
    block::ShardConfig shards;

    if (!(tlb::unpack_cell(block_root, blk) && tlb::unpack_cell(blk.extra, extra) && tlb::unpack_cell(blk.info, info) &&
          tlb::unpack_cell(extra.custom->prefetch_ref(), extra_mc) && shards.unpack(extra_mc.shard_hashes))) {
      LOG(ERROR) << "Error unpack tlb in block: " << last_masterchain_block_id_;
      return;
    }

    std::string shards_idents;
    int total_shards{0};

    td::actor::send_closure_later(shardclientdetector_, &ShardClientDetector::init_wait, last_masterchain_block_id_,
                                  last_masterchain_state_);

    auto parseShards = [&shards_idents, SelfId = actor_id(this), DetectorId = shardclientdetector_,
                        mc_id = last_masterchain_block_id_, &total_shards](McShardHash &ms) {
      auto shard_seqno = ms.top_block_id().id.seqno;
      auto shard_shard = ms.top_block_id().id.shard;
      auto shard_workchain = ms.shard().workchain;

      shards_idents += (shards_idents.empty() ? "" : ", ") + std::to_string(shard_workchain) + ":" +
                       std::to_string(shard_shard) + ":" + std::to_string(shard_seqno);
      total_shards += 1;

      td::actor::send_closure_later(DetectorId, &ShardClientDetector::increase_wait, mc_id);

      auto P_cb =
          td::PromiseCreator::lambda([DetectorId, mc_id, my_id = ms.top_block_id()](td::Result<td::Ref<ShardState>> R) {
            td::actor::send_closure_later(DetectorId, &ShardClientDetector::receive_result, mc_id, my_id, std::move(R));
          });

      td::actor::send_closure_later(SelfId, &ValidatorManagerImpl::get_shard_state_from_db_short, ms.top_block_id(),
                                    std::move(P_cb));
      return 1;
    };

    //    mc_shards_waits_[last_masterchain_block_id_] = total_shards;
    shards.process_shard_hashes(parseShards);

    last_masterchain_time_ = info.gen_utime;

    // update DB if needed
    LOG(INFO) << "New MC block: " << last_masterchain_block_id_
              << " at: " << last_masterchain_time_ << ", shards: " << shards_idents;

    // Handle wait_block
    while (!shard_client_waiters_.empty()) {
      auto it = shard_client_waiters_.begin();
      if (it->first > last_masterchain_seqno_) {
        break;
      }
      for (auto &y : it->second.waiting_) {
        y.promise.set_value(td::Unit());
      }
      shard_client_waiters_.erase(it);
    }
  } else {
    LOG(ERROR) << "Last block error" << init_result.handle->id() << ", skip, in db: " << last_masterchain_block_id_;
  }
}

void ValidatorManagerImpl::started_final(ton::validator::ValidatorManagerInitResult R) {
  auto blk = R.handle;
  auto P = td::PromiseCreator::lambda([validatorInitResult = std::move(R),
                                       SelfId = actor_id(this)](td::Result<td::Ref<BlockData>> block_result) mutable {
    td::actor::send_closure(SelfId, &ValidatorManagerImpl::receiveLastBlock, std::move(block_result),
                            std::move(validatorInitResult));
  });

  td::actor::send_closure(actor_id(this), &ValidatorManagerImpl::get_block_data_from_db, blk, std::move(P));
}

void ValidatorManagerImpl::started(ValidatorManagerInitResult R, bool reinited) {
  if (!reinited) {
    last_masterchain_block_handle_ = std::move(R.handle);
    last_masterchain_block_id_ = last_masterchain_block_handle_->id();
    last_masterchain_seqno_ = last_masterchain_block_id_.id.seqno;
    last_masterchain_state_ = std::move(R.state);

    //new_masterchain_block();
    callback_->initial_read_complete(last_masterchain_block_handle_);
  } else {
    if (last_masterchain_block_id_ != R.handle->id()) {
      auto P = td::PromiseCreator::lambda(
          [SelfId = actor_id(this), validatorInitResult = std::move(R)](td::Result<td::Unit> f) mutable {
            td::actor::send_closure(SelfId, &ValidatorManagerImpl::started_final, std::move(validatorInitResult));
          });

      td::actor::send_closure(db_, &Db::reinit, std::move(P));
    }
  }
}

void ValidatorManagerImpl::update_shards() {
}

void ValidatorManagerImpl::update_shard_blocks() {
  if (!last_masterchain_block_handle_) {
    return;
  }
  if (!shard_blocks_raw_.empty()) {
    for (auto &raw : shard_blocks_raw_) {
      new_shard_block_description_broadcast(BlockIdExt{}, 0, std::move(raw));
    }
    shard_blocks_raw_.clear();
  }
  {
    auto it = shard_blocks_.begin();
    while (it != shard_blocks_.end()) {
      auto &B = *it;
      if (!B->may_be_valid(last_masterchain_block_handle_, last_masterchain_state_)) {
        auto it2 = it++;
        shard_blocks_.erase(it2);
      } else {
        ++it;
      }
    }
  }
  {
    auto it = out_shard_blocks_.begin();
    while (it != out_shard_blocks_.end()) {
      auto &B = *it;
      if (!B->may_be_valid(last_masterchain_block_handle_, last_masterchain_state_)) {
        auto it2 = it++;
        out_shard_blocks_.erase(it2);
      } else {
        ++it;
      }
    }
  }
}

void ValidatorManagerImpl::check_external_message(td::BufferSlice data, td::Promise<td::Ref<ExtMessage>> promise, bool from_ls) {
    auto state = last_masterchain_state_;  // todo: last_liteserver_state_
    if (state.is_null()) {
        promise.set_error(td::Status::Error(ErrorCode::notready, "not ready"));
        return;
    }

    auto R = create_ext_message(std::move(data), state->get_ext_msg_limits());
    if (R.is_error()) {
        promise.set_error(R.move_as_error_prefix("failed to parse external message: "));
        return;
    }
    auto message = R.move_as_ok();
    run_check_external_message(std::move(message), actor_id(this), std::move(promise), from_ls);
}

ValidatorSessionId ValidatorManagerImpl::get_validator_set_id(ShardIdFull shard, td::Ref<ValidatorSet> val_set) {
  return create_hash_tl_object<ton_api::tonNode_sessionId>(shard.workchain, shard.shard, val_set->get_catchain_seqno(),
                                                           td::Bits256::zero());
}

void ValidatorManagerImpl::update_shard_client_state(BlockIdExt masterchain_block_id, td::Promise<td::Unit> promise) {
  td::actor::send_closure(db_, &Db::update_shard_client_state, masterchain_block_id, std::move(promise));
}

void ValidatorManagerImpl::update_lite_server_state_final(ton::BlockIdExt shard_client,
                                                          td::Ref<MasterchainState> state) {
  LOG(INFO) << "New shard client available: " << shard_client;
  last_liteserver_block_id_ = shard_client;
  last_liteserver_state_ = std::move(state);
}

void ValidatorManagerImpl::update_lite_server_state(BlockIdExt shard_client, td::Ref<MasterchainState> state) {
  if (last_liteserver_state_.is_null() || shard_client.seqno() > last_liteserver_block_id_.seqno()) {
    auto P = td::PromiseCreator::lambda(
        [SelfId = actor_id(this), client = shard_client, s = std::move(state)](td::Result<td::Unit> R) {
          td::actor::send_closure(SelfId, &ValidatorManagerImpl::update_lite_server_state_final, client, s);
        });

    td::actor::send_closure(db_, &Db::reinit, std::move(P));
  }
}

void ValidatorManagerImpl::get_shard_client_state(bool from_db, td::Promise<BlockIdExt> promise) {
  td::actor::send_closure(db_, &Db::get_shard_client_state, std::move(promise));
}

void ValidatorManagerImpl::try_get_static_file(FileHash file_hash, td::Promise<td::BufferSlice> promise) {
  td::actor::send_closure(db_, &Db::try_get_static_file, file_hash, std::move(promise));
}

td::actor::ActorOwn<ValidatorManagerInterface> ValidatorManagerDiskFactory::create(
    PublicKeyHash id, td::Ref<ValidatorManagerOptions> opts, ShardIdFull shard, BlockIdExt shard_top_block_id,
    std::string db_root, td::actor::ActorId<keyring::Keyring> keyring, td::actor::ActorId<adnl::Adnl> adnl,
    td::actor::ActorId<rldp::Rldp> rldp, td::actor::ActorId<overlay::Overlays> overlays,
    td::actor::ActorId<liteserver::LiteServerLimiter> lslimiter, bool read_only_) {
  return td::actor::create_actor<validator::ValidatorManagerImpl>(
      "manager", id, std::move(opts), shard, shard_top_block_id, db_root, std::move(keyring), std::move(adnl),
      std::move(rldp), std::move(overlays), std::move(lslimiter), read_only_);
}

td::actor::ActorOwn<ValidatorManagerInterface> ValidatorManagerDiskFactory::create(
    PublicKeyHash id, td::Ref<ValidatorManagerOptions> opts, ShardIdFull shard, BlockIdExt shard_top_block_id,
    std::string db_root, bool read_only) {
  return td::actor::create_actor<validator::ValidatorManagerImpl>("manager", id, std::move(opts), shard,
                                                                  shard_top_block_id, db_root, read_only);
}


}  // namespace validator

}  // namespace ton
