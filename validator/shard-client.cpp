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
#include "common/delay.h"
#include "td/actor/MultiPromise.h"
#include "ton/ton-io.hpp"
#include "validator/block-propagation-trace.h"
#include "validator/downloaders/download-state.hpp"
#include "validator/fabric.h"

#include "shard-client.hpp"

namespace ton {

namespace validator {

namespace {
constexpr double SHARD_CLIENT_WAIT_STATE_TIMEOUT = 30.0;
constexpr double SHARD_CLIENT_APPLY_HANDOFF_TIMEOUT = 12.0;
}

void ShardClient::start_up() {
  if (init_mode_) {
    start_up_init_mode();
    return;
  }

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<BlockIdExt> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &ShardClient::got_state_from_db, R.move_as_ok());
  });
  td::actor::send_closure(manager_, &ValidatorManager::get_shard_client_state, true, std::move(P));
}

void ShardClient::start() {
  if (!started_) {
    started_ = true;
    saved_to_db();
  }
}

void ShardClient::got_state_from_db(BlockIdExt state) {
  CHECK(!init_mode_);

  CHECK(state.is_valid());

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<BlockHandle> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &ShardClient::got_init_handle_from_db, R.move_as_ok());
  });
  td::actor::send_closure(manager_, &ValidatorManager::get_block_handle, state, true, std::move(P));
}

void ShardClient::got_init_handle_from_db(BlockHandle handle) {
  masterchain_block_handle_ = std::move(handle);

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<ShardState>> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &ShardClient::got_init_state_from_db, td::Ref<MasterchainState>{R.move_as_ok()});
  });
  td::actor::send_closure(manager_, &ValidatorManager::get_shard_state_from_db, masterchain_block_handle_,
                          std::move(P));
}

void ShardClient::got_init_state_from_db(td::Ref<MasterchainState> state) {
  saved_to_db();
}

void ShardClient::start_up_init_mode() {
  std::vector<DownloadableShard> shards;
  for (const auto &s : masterchain_state_->get_shards()) {
    if (opts_->need_monitor(s->shard(), masterchain_state_)) {
      auto shard = s->top_block_id();
      shards.push_back({
          .shard = shard,
          .split_depth = masterchain_state_->persistent_state_split_depth(shard.shard_full().workchain),
      });
    }
  }
  download_shard_states(masterchain_block_handle_->id(), std::move(shards), 0);
}

void ShardClient::download_shard_states(BlockIdExt masterchain_block_id, std::vector<DownloadableShard> shards,
                                        size_t idx) {
  if (idx >= shards.size()) {
    LOG(WARNING) << "downloaded all shard states";
    applied_all_shards();
    return;
  }
  auto [block_id, split_depth] = shards[idx];
  td::actor::create_actor<DownloadShardState>(
      "downloadstate", block_id, masterchain_block_handle_->id(), split_depth, 2, manager_, td::Timestamp::in(3600 * 5),
      [=, SelfId = actor_id(this), shards = std::move(shards)](td::Result<td::Ref<ShardState>> R) {
        R.ensure();
        td::actor::send_closure(SelfId, &ShardClient::download_shard_states, masterchain_block_id, std::move(shards),
                                idx + 1);
      })
      .release();
}

void ShardClient::applied_all_shards() {
  apply_active_ = false;
  log_block_propagation_stage(masterchain_block_handle_->id(), BlockPropagationTrace{}, "shardclient.applied_all_shards",
                              "shardclient", false, false, "ok", {}, 0.0, true);
  LOG(WARNING) << "[shardclient-sync] stage=applied_all_shards mc=" << masterchain_block_handle_->id().to_str()
               << " result=ok";
  LOG(DEBUG) << "shardclient: " << masterchain_block_handle_->id() << " finished";
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &ShardClient::saved_to_db);
  });
  td::actor::send_closure(manager_, &ValidatorManager::update_shard_client_state, masterchain_block_handle_->id(),
                          std::move(P));
}

void ShardClient::saved_to_db() {
  CHECK(masterchain_block_handle_);
  log_block_propagation_stage(masterchain_block_handle_->id(), BlockPropagationTrace{}, "shardclient.saved_to_db",
                              "shardclient", false, false, "ok", {}, 0.0, true);
  LOG(WARNING) << "[shardclient-sync] stage=saved_to_db mc=" << masterchain_block_handle_->id().to_str()
               << " pending=" << pending_masterchain_notifications_.size()
               << " started=" << (started_ ? 1 : 0) << " result=ok";
  td::actor::send_closure(manager_, &ValidatorManager::update_shard_client_block_handle, masterchain_block_handle_,
                          std::move(masterchain_state_), [](td::Result<>) {});
  masterchain_state_.clear();
  if (promise_) {
    promise_.set_value(td::Unit());
  }
  if (init_mode_) {
    init_mode_ = false;
  }

  if (!started_) {
    return;
  }
  waiting_ = true;
  if (try_apply_pending_masterchain_block()) {
    return;
  }
  if (try_apply_latest_pending_masterchain_block("saved_to_db")) {
    return;
  }
  waiting_ = false;
  if (masterchain_block_handle_->inited_next_left()) {
    new_masterchain_block_id(masterchain_block_handle_->one_next(true));
  } else {
    waiting_ = true;
  }
}

void ShardClient::new_masterchain_block_id(BlockIdExt block_id) {
  LOG(WARNING) << "[shardclient-sync] stage=next_from_db mc=" << block_id.to_str() << " result=start";
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<BlockHandle> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &ShardClient::got_masterchain_block_handle, R.move_as_ok());
  });
  td::actor::send_closure(manager_, &ValidatorManager::get_block_handle, block_id, true, std::move(P));
}

void ShardClient::got_masterchain_block_handle(BlockHandle handle) {
  LOG(WARNING) << "[shardclient-sync] stage=got_mc_handle mc=" << handle->id().to_str() << " result=ok";
  masterchain_block_handle_ = std::move(handle);
  download_masterchain_state();
}

void ShardClient::download_masterchain_state() {
  auto mc = masterchain_block_handle_->id();
  LOG(WARNING) << "[shardclient-sync] stage=wait_mc_state.start mc=" << mc.to_str() << " result=start";
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), mc](td::Result<td::Ref<ShardState>> R) {
    if (R.is_error()) {
      auto error = R.move_as_error();
      LOG(WARNING) << "[shardclient-sync] stage=wait_mc_state.done mc=" << mc.to_str()
                   << " result=error reason=" << error.to_string();
      td::actor::send_closure(SelfId, &ShardClient::download_masterchain_state);
    } else {
      LOG(WARNING) << "[shardclient-sync] stage=wait_mc_state.done mc=" << mc.to_str() << " result=ok";
      td::actor::send_closure(SelfId, &ShardClient::got_masterchain_block_state,
                              td::Ref<MasterchainState>{R.move_as_ok()});
    }
  });
  td::actor::send_closure(manager_, &ValidatorManager::wait_block_state, masterchain_block_handle_,
                          shard_client_priority(), td::Timestamp::in(600), true, std::move(P));
}

void ShardClient::got_masterchain_block_state(td::Ref<MasterchainState> state) {
  masterchain_state_ = std::move(state);
  if (started_) {
    apply_all_shards();
  }
}

void ShardClient::apply_all_shards() {
  auto generation = ++apply_generation_;
  apply_active_ = true;
  apply_started_at_ = block_propagation_trace_now();
  applying_masterchain_block_id_ = masterchain_block_handle_->id();
  log_block_propagation_stage(masterchain_block_handle_->id(), BlockPropagationTrace{},
                              "shardclient.apply_all_shards.start", "shardclient", false, false, "ok", {}, 0.0, true);
  LOG(WARNING) << "[shardclient-sync] stage=apply_all_shards.start mc=" << masterchain_block_handle_->id().to_str()
               << " shards=" << masterchain_state_->get_shards().size()
               << " generation=" << generation
               << " handoff_timeout_ms=" << static_cast<int>(SHARD_CLIENT_APPLY_HANDOFF_TIMEOUT * 1000)
               << " result=start";
  LOG(DEBUG) << "shardclient: " << masterchain_block_handle_->id() << " started";

  auto mc = masterchain_block_handle_->id();
  delay_action([SelfId = actor_id(this), mc, generation]() {
    td::actor::send_closure(SelfId, &ShardClient::apply_all_shards_timed_out, mc, generation);
  }, td::Timestamp::in(SHARD_CLIENT_APPLY_HANDOFF_TIMEOUT));

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), mc, generation](td::Result<td::Unit> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &ShardClient::finish_apply_all_shards, mc, generation, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &ShardClient::finish_apply_all_shards, mc, generation, td::Status::OK());
    }
  });

  td::MultiPromise mp;
  auto ig = mp.init_guard();
  ig.add_promise(std::move(P));

  auto vec = masterchain_state_->get_shards();
  latest_shards_.clear();
  auto shards_info = masterchain_state_->get_shards();
  for (auto &shard : shards_info) {
    latest_shards_.push_back(shard->top_block_id());
  }

  std::set<WorkchainId> workchains;
  for (auto &shard : vec) {
    workchains.insert(shard->shard().workchain);
    if (opts_->need_monitor(shard->shard(), masterchain_state_)) {
      auto block_id = shard->top_block_id();
      auto wait_started_at = block_propagation_trace_now();
      log_block_propagation_stage(block_id, BlockPropagationTrace{}, "shardclient.wait_state.start", "shardclient",
                                  false, false, "ok", {}, 0.0, true);
      LOG(WARNING) << "[shardclient-sync] stage=wait_state.start mc=" << masterchain_block_handle_->id().to_str()
                   << " shard=" << shard->shard().to_str() << " block=" << block_id.to_str()
                   << " timeout_ms=" << static_cast<int>(SHARD_CLIENT_WAIT_STATE_TIMEOUT * 1000) << " result=start";
      auto Q = td::PromiseCreator::lambda([SelfId = actor_id(this), promise = ig.get_promise(),
                                           mc = masterchain_block_handle_->id(),
                                           shard = shard->shard(), block_id, wait_started_at, generation](
                                              td::Result<td::Ref<ShardState>> R) mutable {
        if (R.is_error()) {
          auto error = R.move_as_error_prefix(PSTRING() << "shard " << shard << ": ");
          log_block_propagation_stage(block_id, BlockPropagationTrace{}, "shardclient.wait_state.done", "shardclient",
                                      false, false, "error", error.to_string(), wait_started_at, true);
          LOG(WARNING) << "[shardclient-sync] stage=wait_state.done mc=" << mc.to_str()
                       << " shard=" << shard.to_str() << " block=" << block_id.to_str()
                       << " generation=" << generation
                       << " ms=" << block_propagation_trace_ms(wait_started_at, block_propagation_trace_now())
                       << " result=error reason=" << error.to_string();
          promise.set_error(std::move(error));
        } else {
          log_block_propagation_stage(block_id, BlockPropagationTrace{}, "shardclient.wait_state.done", "shardclient",
                                      false, false, "ok", {}, wait_started_at, true);
          LOG(WARNING) << "[shardclient-sync] stage=wait_state.done mc=" << mc.to_str()
                       << " shard=" << shard.to_str() << " block=" << block_id.to_str()
                       << " generation=" << generation
                       << " ms=" << block_propagation_trace_ms(wait_started_at, block_propagation_trace_now())
                       << " result=ok";
          td::actor::send_closure(SelfId, &ShardClient::downloaded_shard_state_for_masterchain, R.move_as_ok(), mc,
                                  generation, std::move(promise));
        }
      });
      td::actor::send_closure(manager_, &ValidatorManager::wait_block_state_short, block_id, shard_client_priority(),
                              td::Timestamp::in(SHARD_CLIENT_WAIT_STATE_TIMEOUT), true, std::move(Q));
    }
  }
  for (const auto &[wc, desc] : masterchain_state_->get_workchain_list()) {
    if (!workchains.count(wc) && desc->active && opts_->need_monitor(ShardIdFull{wc, shardIdAll}, masterchain_state_)) {
      auto block_id = BlockIdExt{wc, shardIdAll, 0, desc->zerostate_root_hash, desc->zerostate_file_hash};
      auto wait_started_at = block_propagation_trace_now();
      log_block_propagation_stage(block_id, BlockPropagationTrace{}, "shardclient.wait_state.start", "shardclient",
                                  false, false, "ok", {}, 0.0, true);
      LOG(WARNING) << "[shardclient-sync] stage=wait_state.start mc=" << masterchain_block_handle_->id().to_str()
                   << " workchain=" << wc << " block=" << block_id.to_str()
                   << " timeout_ms=" << static_cast<int>(SHARD_CLIENT_WAIT_STATE_TIMEOUT * 1000) << " result=start";
      auto Q = td::PromiseCreator::lambda([SelfId = actor_id(this), promise = ig.get_promise(),
                                           mc = masterchain_block_handle_->id(),
                                           workchain = wc, block_id, wait_started_at, generation](
                                              td::Result<td::Ref<ShardState>> R) mutable {
        if (R.is_error()) {
          auto error = R.move_as_error_prefix(PSTRING() << "workchain " << workchain << ": ");
          log_block_propagation_stage(block_id, BlockPropagationTrace{}, "shardclient.wait_state.done", "shardclient",
                                      false, false, "error", error.to_string(), wait_started_at, true);
          LOG(WARNING) << "[shardclient-sync] stage=wait_state.done mc=" << mc.to_str()
                       << " workchain=" << workchain << " block=" << block_id.to_str()
                       << " generation=" << generation
                       << " ms=" << block_propagation_trace_ms(wait_started_at, block_propagation_trace_now())
                       << " result=error reason=" << error.to_string();
          promise.set_error(std::move(error));
        } else {
          log_block_propagation_stage(block_id, BlockPropagationTrace{}, "shardclient.wait_state.done", "shardclient",
                                      false, false, "ok", {}, wait_started_at, true);
          LOG(WARNING) << "[shardclient-sync] stage=wait_state.done mc=" << mc.to_str()
                       << " workchain=" << workchain << " block=" << block_id.to_str()
                       << " generation=" << generation
                       << " ms=" << block_propagation_trace_ms(wait_started_at, block_propagation_trace_now())
                       << " result=ok";
          td::actor::send_closure(SelfId, &ShardClient::downloaded_shard_state_for_masterchain, R.move_as_ok(), mc,
                                  generation, std::move(promise));
        }
      });
      td::actor::send_closure(manager_, &ValidatorManager::wait_block_state_short, block_id, shard_client_priority(),
                              td::Timestamp::in(SHARD_CLIENT_WAIT_STATE_TIMEOUT), true, std::move(Q));
    }
  }
}

void ShardClient::finish_apply_all_shards(BlockIdExt masterchain_block_id, std::uint64_t generation,
                                          td::Status status) {
  if (generation != apply_generation_ || !apply_active_ || applying_masterchain_block_id_ != masterchain_block_id) {
    LOG(WARNING) << "[shardclient-sync] stage=apply_all_shards.done mc=" << masterchain_block_id.to_str()
                 << " generation=" << generation << " current_generation=" << apply_generation_
                 << " result=stale reason=handoff";
    return;
  }
  if (status.is_error()) {
    auto reason = status.to_string();
    apply_active_ = false;
    LOG(WARNING) << "[shardclient-sync] stage=apply_all_shards.done mc=" << masterchain_block_id.to_str()
                 << " generation=" << generation << " result=error reason=" << reason;
    apply_all_shards();
  } else {
    LOG(WARNING) << "[shardclient-sync] stage=apply_all_shards.done mc=" << masterchain_block_id.to_str()
                 << " generation=" << generation << " result=ok";
    applied_all_shards();
  }
}

void ShardClient::get_current_shards(td::Promise<std::vector<BlockIdExt>> promise) {
  if (!latest_shards_.empty()){
    std::vector<BlockIdExt> latest_shards_copy = latest_shards_;
    promise.set_value(std::move(latest_shards_copy));
  } else {
    std::vector<BlockIdExt> answer;
    promise.set_value(std::move(answer));
  }

}

void ShardClient::downloaded_shard_state(td::Ref<ShardState> state, td::Promise<td::Unit> promise) {
  run_apply_block_query(state->get_block_id(), td::Ref<BlockData>{}, masterchain_block_handle_->id(), manager_,
                        td::Timestamp::in(600), std::move(promise));
}

void ShardClient::downloaded_shard_state_for_masterchain(td::Ref<ShardState> state, BlockIdExt masterchain_block_id,
                                                         std::uint64_t generation, td::Promise<td::Unit> promise) {
  if (generation != apply_generation_ || !apply_active_ || applying_masterchain_block_id_ != masterchain_block_id) {
    LOG(WARNING) << "[shardclient-sync] stage=downloaded_shard_state mc=" << masterchain_block_id.to_str()
                 << " block=" << state->get_block_id().to_str()
                 << " generation=" << generation << " current_generation=" << apply_generation_
                 << " result=stale reason=handoff";
    promise.set_error(td::Status::Error(ErrorCode::notready, "stale shard-client apply generation"));
    return;
  }
  run_apply_block_query(state->get_block_id(), td::Ref<BlockData>{}, masterchain_block_id, manager_,
                        td::Timestamp::in(600), std::move(promise));
}

void ShardClient::new_masterchain_block_notification(BlockHandle handle, td::Ref<MasterchainState> state) {
  log_block_propagation_stage(handle->id(), BlockPropagationTrace{}, "shardclient.mc_notification", "shardclient",
                              false, false, "ok", {}, 0.0, true);
  LOG(WARNING) << "[shardclient-sync] stage=mc_notification mc=" << handle->id().to_str()
               << " current=" << (masterchain_block_handle_ ? masterchain_block_handle_->id().to_str() : "none")
               << " waiting=" << (waiting_ ? 1 : 0)
               << " pending=" << pending_masterchain_notifications_.size()
               << " started=" << (started_ ? 1 : 0)
               << " apply_active=" << (apply_active_ ? 1 : 0) << " result=ok";
  if (!masterchain_block_handle_ || handle->id().id.seqno <= masterchain_block_handle_->id().id.seqno) {
    LOG(WARNING) << "[shardclient-sync] stage=mc_notification.drop mc=" << handle->id().to_str()
                 << " current=" << (masterchain_block_handle_ ? masterchain_block_handle_->id().to_str() : "none")
                 << " result=drop reason="
                 << (!masterchain_block_handle_ ? "no_current_masterchain" : "old_masterchain");
    return;
  }
  if (started_ && waiting_) {
    LOG(WARNING) << "[shardclient-sync] stage=use_live_notification mc=" << handle->id().to_str()
                 << " current=" << masterchain_block_handle_->id().to_str()
                 << " pending=" << pending_masterchain_notifications_.size()
                 << " apply_active=" << (apply_active_ ? 1 : 0)
                 << " result=ok reason=live_handoff";
    masterchain_block_handle_ = std::move(handle);
    masterchain_state_ = std::move(state);
    pending_masterchain_notifications_.clear();
    waiting_ = false;
    apply_active_ = false;
    apply_all_shards();
    return;
  }
  pending_masterchain_notifications_[handle->id().id.seqno] = std::make_pair(std::move(handle), std::move(state));
  prune_pending_masterchain_notifications();
  if (pending_masterchain_notifications_.empty()) {
    LOG(WARNING) << "[shardclient-sync] stage=buffer_pending mc=none current="
                 << masterchain_block_handle_->id().to_str()
                 << " pending=0 result=drop reason=pruned";
    return;
  }
  LOG(WARNING) << "[shardclient-sync] stage=buffer_pending mc="
               << pending_masterchain_notifications_.rbegin()->second.first->id().to_str()
               << " current=" << masterchain_block_handle_->id().to_str()
               << " pending=" << pending_masterchain_notifications_.size() << " result=ok";
  if (waiting_) {
    if (!try_apply_pending_masterchain_block()) {
      if (!apply_active_ && try_apply_latest_pending_masterchain_block("idle_notification")) {
        return;
      }
      try_apply_next_masterchain_block_from_db();
    }
  }
}

bool ShardClient::try_apply_pending_masterchain_block() {
  if (!waiting_ || !masterchain_block_handle_ || !masterchain_block_handle_->inited_next_left()) {
    return false;
  }
  auto next_id = masterchain_block_handle_->one_next(true);
  auto it = pending_masterchain_notifications_.find(next_id.id.seqno);
  if (it == pending_masterchain_notifications_.end()) {
    return false;
  }
  if (it->second.first->id() != next_id) {
    LOG(WARNING) << "dropping buffered masterchain notification " << it->second.first->id().to_str()
                 << ", expected " << next_id.to_str();
    pending_masterchain_notifications_.erase(it);
    return false;
  }
  LOG(WARNING) << "[shardclient-sync] stage=use_pending mc=" << next_id.to_str()
               << " pending=" << pending_masterchain_notifications_.size() << " result=ok";
  masterchain_block_handle_ = std::move(it->second.first);
  masterchain_state_ = std::move(it->second.second);
  pending_masterchain_notifications_.erase(it);
  waiting_ = false;
  apply_all_shards();
  return true;
}

bool ShardClient::try_apply_latest_pending_masterchain_block(const char *reason) {
  if (!waiting_ || !masterchain_block_handle_ || pending_masterchain_notifications_.empty()) {
    return false;
  }
  auto it = std::prev(pending_masterchain_notifications_.end());
  if (it->second.first->id().id.seqno <= masterchain_block_handle_->id().id.seqno) {
    pending_masterchain_notifications_.clear();
    return false;
  }
  auto pending_before = pending_masterchain_notifications_.size();
  auto next_id = it->second.first->id();
  LOG(WARNING) << "[shardclient-sync] stage=use_latest_pending mc=" << next_id.to_str()
               << " current=" << masterchain_block_handle_->id().to_str()
               << " pending=" << pending_before << " result=ok reason=" << reason;
  masterchain_block_handle_ = std::move(it->second.first);
  masterchain_state_ = std::move(it->second.second);
  pending_masterchain_notifications_.clear();
  waiting_ = false;
  apply_active_ = false;
  apply_all_shards();
  return true;
}

bool ShardClient::try_apply_next_masterchain_block_from_db() {
  if (!waiting_ || !masterchain_block_handle_ || !masterchain_block_handle_->inited_next_left()) {
    return false;
  }
  auto next_id = masterchain_block_handle_->one_next(true);
  LOG(WARNING) << "[shardclient-sync] stage=notification_gap mc=" << next_id.to_str()
               << " pending=" << pending_masterchain_notifications_.size() << " result=fallback_db";
  waiting_ = false;
  new_masterchain_block_id(next_id);
  return true;
}

void ShardClient::prune_pending_masterchain_notifications() {
  if (masterchain_block_handle_) {
    auto current_seqno = masterchain_block_handle_->id().id.seqno;
    while (!pending_masterchain_notifications_.empty() &&
           pending_masterchain_notifications_.begin()->first <= current_seqno) {
      pending_masterchain_notifications_.erase(pending_masterchain_notifications_.begin());
    }
  }
  while (pending_masterchain_notifications_.size() > MAX_PENDING_MASTERCHAIN_NOTIFICATIONS) {
    pending_masterchain_notifications_.erase(pending_masterchain_notifications_.begin());
  }
}

void ShardClient::apply_all_shards_timed_out(BlockIdExt masterchain_block_id, std::uint64_t generation) {
  if (generation != apply_generation_ || !apply_active_ || applying_masterchain_block_id_ != masterchain_block_id) {
    return;
  }
  waiting_ = true;
  auto elapsed_ms = block_propagation_trace_ms(apply_started_at_, block_propagation_trace_now());
  LOG(WARNING) << "[shardclient-sync] stage=apply_all_shards.timeout mc=" << masterchain_block_id.to_str()
               << " generation=" << generation << " pending=" << pending_masterchain_notifications_.size()
               << " elapsed_ms=" << elapsed_ms << " result=handoff";
  if (try_apply_latest_pending_masterchain_block("apply_timeout")) {
    return;
  }
  if (try_apply_next_masterchain_block_from_db()) {
    return;
  }
  delay_action([SelfId = actor_id(this), masterchain_block_id, generation]() {
    td::actor::send_closure(SelfId, &ShardClient::apply_all_shards_timed_out, masterchain_block_id, generation);
  }, td::Timestamp::in(SHARD_CLIENT_APPLY_HANDOFF_TIMEOUT));
}

void ShardClient::get_processed_masterchain_block(td::Promise<BlockSeqno> promise) {
  auto seqno = masterchain_block_handle_ ? masterchain_block_handle_->id().id.seqno : 0;
  if (seqno > 0 && !waiting_) {
    seqno--;
  }
  promise.set_result(seqno);
}

void ShardClient::get_processed_masterchain_block_id(td::Promise<BlockIdExt> promise) {
  if (masterchain_block_handle_) {
    promise.set_result(masterchain_block_handle_->id());
  } else {
    promise.set_error(td::Status::Error(ErrorCode::notready, "shard client not started"));
  }
}

void ShardClient::force_update_shard_client(BlockHandle handle, td::Promise<td::Unit> promise) {
  CHECK(!init_mode_);
  CHECK(!started_);

  if (masterchain_block_handle_->id().seqno() >= handle->id().seqno()) {
    promise.set_value(td::Unit());
    return;
  }

  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), handle, promise = std::move(promise)](td::Result<td::Ref<ShardState>> R) mutable {
        R.ensure();
        td::actor::send_closure(SelfId, &ShardClient::force_update_shard_client_ex, std::move(handle),
                                td::Ref<MasterchainState>{R.move_as_ok()}, std::move(promise));
      });
  td::actor::send_closure(manager_, &ValidatorManager::get_shard_state_from_db, std::move(handle), std::move(P));
}

void ShardClient::force_update_shard_client_ex(BlockHandle handle, td::Ref<MasterchainState> state,
                                               td::Promise<td::Unit> promise) {
  CHECK(!init_mode_);
  CHECK(!started_);

  if (masterchain_block_handle_->id().seqno() >= handle->id().seqno()) {
    promise.set_value(td::Unit());
    return;
  }
  masterchain_block_handle_ = std::move(handle);
  masterchain_state_ = std::move(state);
  promise_ = std::move(promise);
  applied_all_shards();
}

void ShardClient::update_options(td::Ref<ValidatorManagerOptions> opts) {
  opts_ = std::move(opts);
}

}  // namespace validator

}  // namespace ton
