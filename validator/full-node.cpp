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
#include "impl/out-msg-queue-proof.hpp"
#include "interfaces/validator-full-id.h"
#include "td/actor/MultiPromise.h"
#include "td/actor/coro_utils.h"
#include "td/utils/Random.h"
#include "ton/ton-io.hpp"
#include "ton/ton-tl.hpp"

#include <algorithm>
#include <cstring>
#include <mutex>

#include "block-propagation-trace.h"
#include "custom-overlay-metrics.h"
#include "full-node.h"
#include "full-node.hpp"
#include "overlay-gap-diagnostics.h"

namespace ton {

namespace validator {

namespace fullnode {

static const double INACTIVE_SHARD_TTL = (double)overlay::Overlays::overlay_peer_ttl() + 60.0;
constexpr long long CUSTOM_OVERLAY_SYNC_SLOW_LOG_MS = 800;
constexpr double CUSTOM_OVERLAY_NEXT_BLOCK_GRACE_SEC = 0.50;

bool log_value_is(const char *value, const char *expected) {
  return std::strcmp(value, expected) == 0;
}

bool should_log_fullnode_overlay_sync_stage(const char *stage, const char *source, const char *result, long long ms) {
  if (block_propagation_trace_enabled()) {
    return true;
  }
  if (log_value_is(result, "error") || log_value_is(result, "timeout")) {
    return true;
  }
  if (log_value_is(stage, "fullnode.custom_done")) {
    return true;
  }
  if (log_value_is(stage, "fullnode.race_done")) {
    return log_value_is(source, "public") || ms >= CUSTOM_OVERLAY_SYNC_SLOW_LOG_MS;
  }
  if (log_value_is(stage, "fullnode.public") && log_value_is(result, "direct")) {
    return true;
  }
  return ms >= CUSTOM_OVERLAY_SYNC_SLOW_LOG_MS;
}

void log_fullnode_overlay_sync_stage(CustomOverlaySyncKind kind, const BlockIdExt &id, const char *stage,
                                     const char *source, const char *result, std::string reason,
                                     std::string overlay = "-", std::string local = "-", double started_at = 0.0) {
  auto now = block_propagation_trace_now();
  auto ms = started_at > 0.0 ? block_propagation_trace_ms(started_at, now) : -1;
  if (!should_log_fullnode_overlay_sync_stage(stage, source, result, ms)) {
    return;
  }
  LOG(WARNING) << "[custom-overlay-sync]"
               << " stage=" << stage
               << " kind=" << custom_overlay_sync_kind_label(metric_index(kind))
               << " source=" << source
               << " sender=-"
               << " overlay=" << block_propagation_trace_sanitize(std::move(overlay))
               << " local=" << block_propagation_trace_sanitize(std::move(local))
               << " peer=-"
               << " block=" << id.to_str()
               << " wc=" << id.id.workchain
               << " shard=" << id.id.shard
               << " seqno=" << id.id.seqno
               << " peers=0"
               << " ms=" << ms
               << " result=" << result
               << " reason=" << block_propagation_trace_sanitize(std::move(reason));
}

struct PublicFallbackRaceState {
  PublicFallbackRaceState(CustomOverlaySyncKind kind, BlockIdExt block_id, td::Promise<ReceivedBlock> promise)
      : kind(kind), block_id(block_id), promise(std::move(promise)) {
  }

  CustomOverlaySyncKind kind;
  BlockIdExt block_id;
  td::Promise<ReceivedBlock> promise;
  std::mutex mutex;
  std::size_t pending{2};
  bool done{false};
  std::string last_error;
  double started_at{block_propagation_trace_now()};
};

struct PublicArchiveFallbackRaceState {
  PublicArchiveFallbackRaceState(BlockSeqno masterchain_seqno, ShardIdFull shard_prefix,
                                 td::Promise<std::string> promise)
      : masterchain_seqno(masterchain_seqno), shard_prefix(shard_prefix), promise(std::move(promise)) {
  }

  BlockSeqno masterchain_seqno;
  ShardIdFull shard_prefix;
  td::Promise<std::string> promise;
  std::mutex mutex;
  std::size_t pending{2};
  bool done{false};
  std::string last_error;
};

void finish_public_fallback_race(std::shared_ptr<PublicFallbackRaceState> state, const char *source,
                                 td::Result<ReceivedBlock> R) {
  if (R.is_ok()) {
    auto value = R.move_as_ok();
    td::Promise<ReceivedBlock> promise;
    bool should_finish = false;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (!state->done) {
        state->done = true;
        promise = std::move(state->promise);
        should_finish = true;
      }
    }
    if (should_finish) {
      log_fullnode_overlay_sync_stage(state->kind, state->block_id, "fullnode.race_done", source, "ok", {}, "-", "-",
                                      state->started_at);
      promise.set_value(std::move(value));
    }
    return;
  }

  auto error = R.move_as_error();
  td::Promise<ReceivedBlock> promise;
  bool should_finish = false;
  auto error_string = error.to_string();
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->done) {
      return;
    }
    state->last_error = PSTRING() << source << ": " << error_string;
    CHECK(state->pending > 0);
    state->pending--;
    if (state->pending == 0) {
      state->done = true;
      promise = std::move(state->promise);
      should_finish = true;
      error_string = state->last_error;
    }
  }
  if (should_finish) {
    log_fullnode_overlay_sync_stage(state->kind, state->block_id, "fullnode.race_done", source, "error",
                                    error_string, "-", "-", state->started_at);
    promise.set_error(td::Status::Error(ErrorCode::notready, PSTRING() << "custom and public overlay failed: "
                                                                       << error_string));
  }
}

void finish_public_archive_fallback_race(std::shared_ptr<PublicArchiveFallbackRaceState> state, const char *source,
                                         td::Result<std::string> R) {
  if (R.is_ok()) {
    auto value = R.move_as_ok();
    td::Promise<std::string> promise;
    bool should_finish = false;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (!state->done) {
        state->done = true;
        promise = std::move(state->promise);
        should_finish = true;
      }
    }
    if (should_finish) {
      LOG(WARNING) << "[archive-sync] stage=race_done source=" << source
                   << " seqno=" << state->masterchain_seqno
                   << " shard=" << state->shard_prefix.to_str()
                   << " result=ok";
      promise.set_value(std::move(value));
    }
    return;
  }

  auto error = R.move_as_error();
  td::Promise<std::string> promise;
  bool should_finish = false;
  auto error_string = error.to_string();
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->done) {
      return;
    }
    state->last_error = PSTRING() << source << ": " << error_string;
    CHECK(state->pending > 0);
    state->pending--;
    if (state->pending == 0) {
      state->done = true;
      promise = std::move(state->promise);
      should_finish = true;
      error_string = state->last_error;
    }
  }
  if (should_finish) {
    LOG(WARNING) << "[archive-sync] stage=race_done source=" << source
                 << " seqno=" << state->masterchain_seqno
                 << " shard=" << state->shard_prefix.to_str()
                 << " result=error reason=" << error_string;
    promise.set_error(td::Status::Error(ErrorCode::notready, PSTRING() << "custom and public archive overlay failed: "
                                                                       << error_string));
  }
}

void FullNodeImpl::add_permanent_key(PublicKeyHash key, td::Promise<td::Unit> promise) {
  if (local_keys_.count(key)) {
    promise.set_value(td::Unit());
    return;
  }

  local_keys_.insert(key);
  for (auto &p : custom_overlays_) {
    update_custom_overlay(p.second);
  }

  if (!sign_cert_by_.is_zero()) {
    promise.set_value(td::Unit());
    return;
  }

  for (auto &x : all_validators_) {
    if (x == key) {
      sign_cert_by_ = key;
    }
  }

  for (auto &shard : shards_) {
    if (!shard.second.actor.empty()) {
      td::actor::send_closure(shard.second.actor, &FullNodeShard::update_validators, all_validators_, sign_cert_by_);
    }
  }
  promise.set_value(td::Unit());
}

void FullNodeImpl::del_permanent_key(PublicKeyHash key, td::Promise<td::Unit> promise) {
  if (!local_keys_.count(key)) {
    promise.set_value(td::Unit());
    return;
  }
  local_keys_.erase(key);
  update_validator_telemetry_collector();
  for (auto &p : custom_overlays_) {
    update_custom_overlay(p.second);
  }

  if (sign_cert_by_ != key) {
    promise.set_value(td::Unit());
    return;
  }
  sign_cert_by_ = PublicKeyHash::zero();

  for (auto &x : all_validators_) {
    if (local_keys_.count(x)) {
      sign_cert_by_ = x;
    }
  }

  for (auto &shard : shards_) {
    if (!shard.second.actor.empty()) {
      td::actor::send_closure(shard.second.actor, &FullNodeShard::update_validators, all_validators_, sign_cert_by_);
    }
  }
  promise.set_value(td::Unit());
}

void FullNodeImpl::add_collator_adnl_id(adnl::AdnlNodeIdShort id) {
  ++local_collator_nodes_[id];
}

void FullNodeImpl::del_collator_adnl_id(adnl::AdnlNodeIdShort id) {
  if (--local_collator_nodes_[id] == 0) {
    local_collator_nodes_.erase(id);
  }
}

void FullNodeImpl::sign_shard_overlay_certificate(ShardIdFull shard_id, PublicKeyHash signed_key, td::uint32 expiry_at,
                                                  td::uint32 max_size, td::Promise<td::BufferSlice> promise) {
  auto it = shards_.find(shard_id);
  if (it == shards_.end() || it->second.actor.empty()) {
    promise.set_error(td::Status::Error(ErrorCode::error, "shard not found"));
    return;
  }
  td::actor::send_closure(it->second.actor, &FullNodeShard::sign_overlay_certificate, signed_key, expiry_at, max_size,
                          std::move(promise));
}

void FullNodeImpl::import_shard_overlay_certificate(ShardIdFull shard_id, PublicKeyHash signed_key,
                                                    std::shared_ptr<ton::overlay::Certificate> cert,
                                                    td::Promise<td::Unit> promise) {
  auto it = shards_.find(shard_id);
  if (it == shards_.end() || it->second.actor.empty()) {
    promise.set_error(td::Status::Error(ErrorCode::error, "shard not found"));
    return;
  }
  td::actor::send_closure(it->second.actor, &FullNodeShard::import_overlay_certificate, signed_key, cert,
                          std::move(promise));
}

void FullNodeImpl::update_adnl_id(adnl::AdnlNodeIdShort adnl_id, td::Promise<td::Unit> promise) {
  adnl_id_ = adnl_id;

  td::MultiPromise mp;
  auto ig = mp.init_guard();
  ig.add_promise(std::move(promise));

  for (auto &s : shards_) {
    if (!s.second.actor.empty()) {
      td::actor::send_closure(s.second.actor, &FullNodeShard::update_adnl_id, adnl_id, ig.get_promise());
    }
  }

  for (auto &p : custom_overlays_) {
    update_custom_overlay(p.second);
  }
}

void FullNodeImpl::set_config(FullNodeConfig config) {
  opts_.config_ = config;
  for (auto &s : shards_) {
    if (!s.second.actor.empty()) {
      td::actor::send_closure(s.second.actor, &FullNodeShard::set_config, config);
    }
  }
  for (auto &overlay : custom_overlays_) {
    for (auto &actor : overlay.second.actors_) {
      td::actor::send_closure(actor.second, &FullNodeCustomOverlay::set_config, config);
    }
  }
}

void FullNodeImpl::add_custom_overlay(CustomOverlayParams params, td::Promise<td::Unit> promise) {
  if (params.nodes_.empty()) {
    promise.set_error(td::Status::Error("list of nodes is empty"));
    return;
  }
  std::string name = params.name_;
  if (custom_overlays_.count(name)) {
    promise.set_error(td::Status::Error(PSTRING() << "duplicate custom overlay name \"" << name << "\""));
    return;
  }
  VLOG(FULL_NODE_WARNING) << "Adding custom overlay \"" << name << "\", " << params.nodes_.size() << " nodes";
  auto &p = custom_overlays_[name];
  p.params_ = std::move(params);
  update_custom_overlay(p);
  promise.set_result(td::Unit());
}

void FullNodeImpl::del_custom_overlay(std::string name, td::Promise<td::Unit> promise) {
  auto it = custom_overlays_.find(name);
  if (it == custom_overlays_.end()) {
    promise.set_error(td::Status::Error(PSTRING() << "no such overlay \"" << name << "\""));
    return;
  }
  custom_overlays_.erase(it);
  promise.set_result(td::Unit());
}

void FullNodeImpl::initial_read_complete(BlockHandle top_handle) {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &FullNodeImpl::sync_completed);
  });
  auto it = shards_.find(ShardIdFull{masterchainId});
  CHECK(it != shards_.end() && !it->second.actor.empty());
  td::actor::send_closure(it->second.actor, &FullNodeShard::set_handle, top_handle, std::move(P));
}

void FullNodeImpl::on_new_masterchain_block(td::Ref<MasterchainState> state, std::set<ShardIdFull> shards_to_monitor) {
  CHECK(shards_to_monitor.count(ShardIdFull(masterchainId)));
  bool join_all_overlays = !sign_cert_by_.is_zero();
  std::set<ShardIdFull> all_shards;
  std::set<ShardIdFull> new_active;
  all_shards.insert(ShardIdFull(masterchainId));
  std::set<WorkchainId> workchains;
  wc_monitor_min_split_ = state->monitor_min_split_depth(basechainId);
  auto cut_shard = [&](ShardIdFull shard) -> ShardIdFull {
    return wc_monitor_min_split_ < shard.pfx_len() ? shard_prefix(shard, wc_monitor_min_split_) : shard;
  };
  for (auto &info : state->get_shards()) {
    workchains.insert(info->shard().workchain);
    ShardIdFull shard = cut_shard(info->shard());
    while (true) {
      all_shards.insert(shard);
      if (shard.pfx_len() == 0) {
        break;
      }
      shard = shard_parent(shard);
    }
  }
  for (const auto &[wc, winfo] : state->get_workchain_list()) {
    if (workchains.find(wc) == workchains.end() && winfo->active && winfo->enabled_since <= state->get_unix_time()) {
      all_shards.insert(ShardIdFull(wc));
    }
  }
  for (ShardIdFull shard : shards_to_monitor) {
    shard = cut_shard(shard);
    while (true) {
      new_active.insert(shard);
      if (shard.pfx_len() == 0) {
        break;
      }
      shard = shard_parent(shard);
    }
  }

  for (auto it = shards_.begin(); it != shards_.end();) {
    if (all_shards.contains(it->first)) {
      ++it;
    } else {
      it = shards_.erase(it);
    }
  }
  for (ShardIdFull shard : all_shards) {
    bool active = new_active.find(shard) != new_active.end();
    bool overlay_exists = !shards_[shard].actor.empty();
    if (active || join_all_overlays || overlay_exists) {
      update_shard_actor(shard, active);
    }
  }

  for (auto &[_, shard_info] : shards_) {
    if (!shard_info.active && shard_info.delete_at && shard_info.delete_at.is_in_past() && !join_all_overlays) {
      shard_info.actor = {};
      shard_info.delete_at = td::Timestamp::never();
    }
  }

  std::set<adnl::AdnlNodeIdShort> my_adnl_ids;
  my_adnl_ids.insert(adnl_id_);
  for (const auto &[adnl_id, _] : local_collator_nodes_) {
    my_adnl_ids.insert(adnl_id);
  }
  for (auto key : local_keys_) {
    auto it = current_validators_.find(key);
    if (it != current_validators_.end()) {
      my_adnl_ids.insert(it->second);
    }
  }
  std::set<ShardIdFull> monitoring_shards;
  for (ShardIdFull shard : shards_to_monitor) {
    monitoring_shards.insert(cut_shard(shard));
  }
  fast_sync_overlays_.update_overlays(state, std::move(my_adnl_ids), std::move(monitoring_shards),
                                      zero_state_file_hash_, opts_.fast_sync_broadcast_speed_multiplier_, keyring_,
                                      adnl_, rldp2_, quic_, overlays_, validator_manager_, actor_id(this));
  update_validator_telemetry_collector();
}

void FullNodeImpl::update_shard_actor(ShardIdFull shard, bool active) {
  ShardInfo &info = shards_[shard];
  if (info.actor.empty()) {
    info.actor = FullNodeShard::create(shard, local_id_, adnl_id_, zero_state_file_hash_, opts_, limiter_, keyring_,
                                       adnl_, rldp2_, overlays_, validator_manager_, client_, actor_id(this), active);
    if (!all_validators_.empty()) {
      td::actor::send_closure(info.actor, &FullNodeShard::update_validators, all_validators_, sign_cert_by_);
    }
  } else if (info.active != active) {
    td::actor::send_closure(info.actor, &FullNodeShard::set_active, active);
  }
  info.active = active;
  info.delete_at = active ? td::Timestamp::never() : td::Timestamp::in(INACTIVE_SHARD_TTL);
}

void FullNodeImpl::sync_completed() {
  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::sync_complete, [](td::Result<>) {});
}

void FullNodeImpl::send_ihr_message(AccountIdPrefixFull dst, td::BufferSlice data) {
  auto shard = get_shard(dst);
  if (shard.empty()) {
    VLOG(FULL_NODE_WARNING) << "dropping OUT ihr message to unknown shard";
    return;
  }
  td::actor::send_closure(shard, &FullNodeShard::send_ihr_message, std::move(data));
}

void FullNodeImpl::send_ext_message(AccountIdPrefixFull dst, td::BufferSlice data) {
  send_ext_message_impl(dst, std::move(data), false);
}

void FullNodeImpl::send_ext_message_relay_all(AccountIdPrefixFull dst, td::BufferSlice data) {
  send_ext_message_impl(dst, std::move(data), true);
}

void FullNodeImpl::send_ext_message_raw_all(td::BufferSlice data) {
  bool sent_private = false;
  for (auto &[_, private_overlay] : custom_overlays_) {
    for (auto &[local_id, actor] : private_overlay.actors_) {
      if (private_overlay.params_.msg_senders_.find(local_id) != private_overlay.params_.msg_senders_.end()) {
        sent_private = true;
        td::actor::send_closure(actor, &FullNodeCustomOverlay::send_external_message, data.clone());
      }
    }
  }

  bool sent_public = false;
  for (auto &[_, shard] : shards_) {
    if (!shard.actor.empty()) {
      sent_public = true;
      td::actor::send_closure(shard.actor, &FullNodeShard::send_external_message, data.clone());
    }
  }
  VLOG(FULL_NODE_DEBUG) << "Relayed raw external message"
                        << " private=" << (sent_private ? 1 : 0)
                        << " public=" << (sent_public ? 1 : 0)
                        << " size=" << data.size();
}

void FullNodeImpl::send_ext_message_impl(AccountIdPrefixFull dst, td::BufferSlice data, bool force_public) {
  bool skip_public = false;
  for (auto &[_, private_overlay] : custom_overlays_) {
    if (private_overlay.params_.send_shard(dst.as_leaf_shard())) {
      for (auto &[local_id, actor] : private_overlay.actors_) {
        if (private_overlay.params_.msg_senders_.find(local_id) != private_overlay.params_.msg_senders_.end()) {
          td::actor::send_closure(actor, &FullNodeCustomOverlay::send_external_message, data.clone());
          if (!force_public && private_overlay.params_.skip_public_msg_send_) {
            skip_public = true;
          }
        }
      }
    }
  }

  if (!skip_public) {
    auto shard = get_shard(dst);
    if (shard.empty()) {
      VLOG(FULL_NODE_WARNING) << "dropping OUT ext message to unknown shard";
      return;
    }
    td::actor::send_closure(shard, &FullNodeShard::send_external_message, std::move(data));
  }
}

void FullNodeImpl::send_shard_block_info(BlockIdExt block_id, CatchainSeqno cc_seqno, td::BufferSlice data) {
  send_shard_block_info_to_custom_overlays(block_id, cc_seqno, data);
  auto shard = get_shard(ShardIdFull{masterchainId});
  if (shard.empty()) {
    VLOG(FULL_NODE_WARNING) << "dropping OUT shard block info message to unknown shard";
    return;
  }
  auto fast_sync_overlay = fast_sync_overlays_.choose_overlay(ShardIdFull(masterchainId)).first;
  if (!fast_sync_overlay.empty()) {
    td::actor::send_closure(fast_sync_overlay, &FullNodeFastSyncOverlay::send_shard_block_info, block_id, cc_seqno,
                            data.clone());
  }
  td::actor::send_closure(shard, &FullNodeShard::send_shard_block_info, block_id, cc_seqno, std::move(data));
}

void FullNodeImpl::send_block_candidate(BlockIdExt block_id, CatchainSeqno cc_seqno, td::uint32 validator_set_hash,
                                        td::BufferSlice data, int mode) {
  if (mode & broadcast_mode_custom) {
    send_block_candidate_broadcast_to_custom_overlays(block_id, cc_seqno, validator_set_hash, data);
  }
  if (mode & broadcast_mode_fast_sync) {
    auto fast_sync_overlay = fast_sync_overlays_.choose_overlay(block_id.shard_full()).first;
    if (!fast_sync_overlay.empty()) {
      td::actor::send_closure(fast_sync_overlay, &FullNodeFastSyncOverlay::send_block_candidate, block_id, cc_seqno,
                              validator_set_hash, data.clone());
    }
  }
  if (mode & broadcast_mode_public) {
    auto shard = get_shard(ShardIdFull{masterchainId, shardIdAll});
    if (shard.empty()) {
      VLOG(FULL_NODE_WARNING) << "dropping OUT shard block info message to unknown shard";
      return;
    }
    td::actor::send_closure(shard, &FullNodeShard::send_block_candidate, block_id, cc_seqno, validator_set_hash,
                            std::move(data));
  }
}

void FullNodeImpl::send_out_msg_queue_proof_broadcast(td::Ref<OutMsgQueueProofBroadcast> broadcast) {
  auto fast_sync_overlay = fast_sync_overlays_.choose_overlay(broadcast->dst_shard).first;
  if (!fast_sync_overlay.empty()) {
    td::actor::send_closure(fast_sync_overlay, &FullNodeFastSyncOverlay::send_out_msg_queue_proof_broadcast,
                            std::move(broadcast));
  }
}

void FullNodeImpl::send_broadcast(BlockBroadcast broadcast, int mode) {
  if (mode & broadcast_mode_custom) {
    send_block_broadcast_to_custom_overlays(broadcast);
  }
  if (mode & broadcast_mode_fast_sync) {
    auto fast_sync_overlay = fast_sync_overlays_.choose_overlay(broadcast.block_id.shard_full()).first;
    if (!fast_sync_overlay.empty()) {
      td::actor::send_closure(fast_sync_overlay, &FullNodeFastSyncOverlay::send_broadcast, broadcast.clone());
    }
  }
  if (mode & broadcast_mode_public) {
    auto shard = get_shard(broadcast.block_id.shard_full());
    if (shard.empty()) {
      VLOG(FULL_NODE_WARNING) << "dropping OUT broadcast to unknown shard";
      return;
    }
    td::actor::send_closure(shard, &FullNodeShard::send_broadcast, std::move(broadcast));
  }
}

void FullNodeImpl::download_block(BlockIdExt id, td::uint32 priority, td::Timestamp timeout,
                                  td::Promise<ReceivedBlock> promise) {
  bool has_custom_overlay = !custom_overlays_.empty();
  bool shard_served_by_custom_overlay = false;
  for (auto &[name, custom_overlay] : custom_overlays_) {
    if (!custom_overlay.params_.send_shard(id.shard_full())) {
      continue;
    }
    shard_served_by_custom_overlay = true;
    for (auto &[local_id, actor] : custom_overlay.actors_) {
      log_fullnode_overlay_sync_stage(CustomOverlaySyncKind::Block, id, "fullnode.custom_select", "custom", "attempt",
                                      {}, name, PSTRING() << local_id);
      auto state = std::make_shared<PublicFallbackRaceState>(CustomOverlaySyncKind::Block, id, std::move(promise));
      auto P = td::PromiseCreator::lambda(
          [state, name](td::Result<ReceivedBlock> R) mutable {
            if (R.is_error()) {
              auto error = R.error().to_string();
              VLOG(FULL_NODE_DEBUG) << "failed to download block " << state->block_id.to_str()
                                    << " from custom overlay \"" << name << "\": " << error
                                    << "; public overlay fallback is already racing";
              record_custom_overlay_sync_fallback(CustomOverlaySyncKind::Block,
                                                  CustomOverlaySyncFallbackReason::CustomError);
              log_fullnode_overlay_sync_stage(CustomOverlaySyncKind::Block, state->block_id, "fullnode.custom_done",
                                              "custom", "error", error, name);
            }
            finish_public_fallback_race(std::move(state), "custom", std::move(R));
          });
      auto PublicP = td::PromiseCreator::lambda([state](td::Result<ReceivedBlock> R) mutable {
        finish_public_fallback_race(std::move(state), "public", std::move(R));
      });
      record_public_overlay_sync_download(CustomOverlaySyncKind::Block, PublicOverlaySyncReason::Fallback);
      log_fullnode_overlay_sync_stage(CustomOverlaySyncKind::Block, id, "fullnode.public", "public", "fallback",
                                      "race_custom_overlay", name);
      td::actor::send_closure(actor, &FullNodeCustomOverlay::download_block, id, priority, timeout, std::move(P));
      download_block_from_public_overlay(id, priority, timeout, std::move(PublicP));
      return;
    }
  }
  record_custom_overlay_sync_fallback(
      CustomOverlaySyncKind::Block,
      shard_served_by_custom_overlay
          ? CustomOverlaySyncFallbackReason::NoLocalActor
          : (has_custom_overlay ? CustomOverlaySyncFallbackReason::ShardNotServed
                                : CustomOverlaySyncFallbackReason::NoCustomOverlay));
  record_public_overlay_sync_download(CustomOverlaySyncKind::Block, PublicOverlaySyncReason::Direct);
  log_fullnode_overlay_sync_stage(
      CustomOverlaySyncKind::Block, id, "fullnode.public", "public", "direct",
      shard_served_by_custom_overlay ? "no_local_actor" : (has_custom_overlay ? "shard_not_served" : "no_custom_overlay"));
  download_block_from_public_overlay(id, priority, timeout, std::move(promise));
}

void FullNodeImpl::download_next_block(BlockIdExt prev_id, td::uint32 priority, td::Timestamp timeout,
                                       td::Promise<ReceivedBlock> promise) {
  bool shard_served_by_custom_overlay = false;
  for (auto &[name, custom_overlay] : custom_overlays_) {
    if (!custom_overlay.params_.send_shard(prev_id.shard_full())) {
      continue;
    }
    shard_served_by_custom_overlay = !custom_overlay.actors_.empty();
    if (shard_served_by_custom_overlay) {
      break;
    }
  }
  if (shard_served_by_custom_overlay) {
    LOG(WARNING) << "[overlay-gap] event=download_next_defer"
                 << " prev=" << prev_id.to_str()
                 << " wc=" << prev_id.id.workchain
                 << " shard=" << prev_id.id.shard
                 << " prev_seqno=" << prev_id.id.seqno
                 << " expected_seqno=" << prev_id.id.seqno + 1
                 << " delay_ms=" << static_cast<long long>(CUSTOM_OVERLAY_NEXT_BLOCK_GRACE_SEC * 1000)
                 << " reason=custom_overlay_grace";
    delay_action(
        [SelfId = actor_id(this), prev_id, priority, timeout, promise = std::move(promise)]() mutable {
          td::actor::send_closure(SelfId, &FullNodeImpl::download_next_block_now, prev_id, priority, timeout,
                                  std::move(promise));
        },
        td::Timestamp::in(CUSTOM_OVERLAY_NEXT_BLOCK_GRACE_SEC));
    return;
  }
  download_next_block_now(prev_id, priority, timeout, std::move(promise));
}

void FullNodeImpl::download_next_block_now(BlockIdExt prev_id, td::uint32 priority, td::Timestamp timeout,
                                           td::Promise<ReceivedBlock> promise) {
  bool has_custom_overlay = !custom_overlays_.empty();
  bool shard_served_by_custom_overlay = false;
  for (auto &[name, custom_overlay] : custom_overlays_) {
    if (!custom_overlay.params_.send_shard(prev_id.shard_full())) {
      continue;
    }
    shard_served_by_custom_overlay = true;
    for (auto &[local_id, actor] : custom_overlay.actors_) {
      overlay_gap::log_download_next_decision(prev_id, "custom", name.c_str(), "race_public_fallback");
      log_fullnode_overlay_sync_stage(CustomOverlaySyncKind::NextBlock, prev_id, "fullnode.custom_select", "custom",
                                      "attempt", {}, name, PSTRING() << local_id);
      auto state =
          std::make_shared<PublicFallbackRaceState>(CustomOverlaySyncKind::NextBlock, prev_id, std::move(promise));
      auto P = td::PromiseCreator::lambda(
          [state, name](td::Result<ReceivedBlock> R) mutable {
            if (R.is_error()) {
              auto error = R.error().to_string();
              VLOG(FULL_NODE_DEBUG) << "failed to download next block after " << state->block_id.to_str()
                                    << " from custom overlay \"" << name << "\": " << error
                                    << "; public overlay fallback is already racing";
              record_custom_overlay_sync_fallback(CustomOverlaySyncKind::NextBlock,
                                                  CustomOverlaySyncFallbackReason::CustomError);
              log_fullnode_overlay_sync_stage(CustomOverlaySyncKind::NextBlock, state->block_id,
                                              "fullnode.custom_done", "custom", "error", error, name);
            }
            finish_public_fallback_race(std::move(state), "custom", std::move(R));
          });
      auto PublicP = td::PromiseCreator::lambda([state](td::Result<ReceivedBlock> R) mutable {
        finish_public_fallback_race(std::move(state), "public", std::move(R));
      });
      record_public_overlay_sync_download(CustomOverlaySyncKind::NextBlock, PublicOverlaySyncReason::Fallback);
      overlay_gap::log_download_next_decision(prev_id, "public", name.c_str(), "race_custom_overlay");
      log_fullnode_overlay_sync_stage(CustomOverlaySyncKind::NextBlock, prev_id, "fullnode.public", "public",
                                      "fallback", "race_custom_overlay", name);
      td::actor::send_closure(actor, &FullNodeCustomOverlay::download_next_block, prev_id, priority, timeout,
                              std::move(P));
      download_next_block_from_public_overlay(prev_id, priority, timeout, std::move(PublicP));
      return;
    }
  }
  record_custom_overlay_sync_fallback(
      CustomOverlaySyncKind::NextBlock,
      shard_served_by_custom_overlay
          ? CustomOverlaySyncFallbackReason::NoLocalActor
                                : (has_custom_overlay ? CustomOverlaySyncFallbackReason::ShardNotServed
                                                      : CustomOverlaySyncFallbackReason::NoCustomOverlay));
  record_public_overlay_sync_download(CustomOverlaySyncKind::NextBlock, PublicOverlaySyncReason::Direct);
  overlay_gap::log_download_next_decision(
      prev_id, "public", "-", shard_served_by_custom_overlay
                                ? "no_local_actor"
                                : (has_custom_overlay ? "shard_not_served" : "no_custom_overlay"));
  log_fullnode_overlay_sync_stage(CustomOverlaySyncKind::NextBlock, prev_id, "fullnode.public", "public", "direct",
                                  shard_served_by_custom_overlay
                                      ? "no_local_actor"
                                      : (has_custom_overlay ? "shard_not_served" : "no_custom_overlay"));
  download_next_block_from_public_overlay(prev_id, priority, timeout, std::move(promise));
}

void FullNodeImpl::download_block_from_public_overlay(BlockIdExt id, td::uint32 priority, td::Timestamp timeout,
                                                      td::Promise<ReceivedBlock> promise) {
  auto shard = get_shard(id.shard_full());
  if (shard.empty()) {
    VLOG(FULL_NODE_WARNING) << "dropping download block query to unknown shard";
    promise.set_error(td::Status::Error(ErrorCode::notready, "shard not ready"));
    return;
  }
  td::actor::send_closure(shard, &FullNodeShard::download_block, id, priority, timeout, std::move(promise));
}

void FullNodeImpl::download_next_block_from_public_overlay(BlockIdExt prev_id, td::uint32 priority,
                                                           td::Timestamp timeout,
                                                           td::Promise<ReceivedBlock> promise) {
  auto shard = get_shard(prev_id.shard_full());
  if (shard.empty()) {
    VLOG(FULL_NODE_WARNING) << "dropping download next block query to unknown shard";
    promise.set_error(td::Status::Error(ErrorCode::notready, "shard not ready"));
    return;
  }
  td::actor::send_closure(shard, &FullNodeShard::download_next_block, prev_id, priority, timeout, std::move(promise));
}

void FullNodeImpl::download_zero_state(BlockIdExt id, td::uint32 priority, td::Timestamp timeout,
                                       td::Promise<td::BufferSlice> promise) {
  auto shard = get_shard(id.shard_full());
  if (shard.empty()) {
    VLOG(FULL_NODE_WARNING) << "dropping download state query to unknown shard";
    promise.set_error(td::Status::Error(ErrorCode::notready, "shard not ready"));
    return;
  }
  td::actor::send_closure(shard, &FullNodeShard::download_zero_state, id, priority, timeout, std::move(promise));
}

void FullNodeImpl::download_persistent_state(BlockIdExt id, BlockIdExt masterchain_block_id, PersistentStateType type,
                                             td::uint32 priority, td::Timestamp timeout,
                                             td::Promise<td::BufferSlice> promise) {
  auto shard = get_shard(id.shard_full(), /* historical = */ true);
  if (shard.empty()) {
    VLOG(FULL_NODE_WARNING) << "dropping download state diff query to unknown shard";
    promise.set_error(td::Status::Error(ErrorCode::notready, "shard not ready"));
    return;
  }
  td::actor::send_closure(shard, &FullNodeShard::download_persistent_state, id, masterchain_block_id, type, priority,
                          timeout, std::move(promise));
}

void FullNodeImpl::download_block_proof(BlockIdExt block_id, td::uint32 priority, td::Timestamp timeout,
                                        td::Promise<td::BufferSlice> promise) {
  for (auto &[name, custom_overlay] : custom_overlays_) {
    if (!custom_overlay.params_.send_shard(block_id.shard_full())) {
      continue;
    }
    for (auto &[local_id, actor] : custom_overlay.actors_) {
      auto P = td::PromiseCreator::lambda(
          [SelfId = actor_id(this), block_id, priority, timeout, promise = std::move(promise),
           name](td::Result<td::BufferSlice> R) mutable {
            if (R.is_ok()) {
              promise.set_value(R.move_as_ok());
              return;
            }
            VLOG(FULL_NODE_DEBUG) << "failed to download block proof " << block_id.to_str()
                                  << " from custom overlay \"" << name << "\": " << R.move_as_error()
                                  << "; falling back to public overlay";
            td::actor::send_closure(SelfId, &FullNodeImpl::download_block_proof_from_public_overlay, block_id,
                                    priority, timeout, std::move(promise));
          });
      td::actor::send_closure(actor, &FullNodeCustomOverlay::download_block_proof, block_id, priority, timeout,
                              std::move(P));
      return;
    }
  }
  download_block_proof_from_public_overlay(block_id, priority, timeout, std::move(promise));
}

void FullNodeImpl::download_block_proof_from_public_overlay(BlockIdExt block_id, td::uint32 priority,
                                                            td::Timestamp timeout,
                                                            td::Promise<td::BufferSlice> promise) {
  auto shard = get_shard(block_id.shard_full());
  if (shard.empty()) {
    VLOG(FULL_NODE_WARNING) << "dropping download proof query to unknown shard";
    promise.set_error(td::Status::Error(ErrorCode::notready, "shard not ready"));
    return;
  }
  td::actor::send_closure(shard, &FullNodeShard::download_block_proof, block_id, priority, timeout, std::move(promise));
}

void FullNodeImpl::download_block_proof_link(BlockIdExt block_id, td::uint32 priority, td::Timestamp timeout,
                                             td::Promise<td::BufferSlice> promise) {
  for (auto &[name, custom_overlay] : custom_overlays_) {
    if (!custom_overlay.params_.send_shard(block_id.shard_full())) {
      continue;
    }
    for (auto &[local_id, actor] : custom_overlay.actors_) {
      auto P = td::PromiseCreator::lambda(
          [SelfId = actor_id(this), block_id, priority, timeout, promise = std::move(promise),
           name](td::Result<td::BufferSlice> R) mutable {
            if (R.is_ok()) {
              promise.set_value(R.move_as_ok());
              return;
            }
            VLOG(FULL_NODE_DEBUG) << "failed to download block proof link " << block_id.to_str()
                                  << " from custom overlay \"" << name << "\": " << R.move_as_error()
                                  << "; falling back to public overlay";
            td::actor::send_closure(SelfId, &FullNodeImpl::download_block_proof_link_from_public_overlay, block_id,
                                    priority, timeout, std::move(promise));
          });
      td::actor::send_closure(actor, &FullNodeCustomOverlay::download_block_proof_link, block_id, priority, timeout,
                              std::move(P));
      return;
    }
  }
  download_block_proof_link_from_public_overlay(block_id, priority, timeout, std::move(promise));
}

void FullNodeImpl::download_block_proof_link_from_public_overlay(BlockIdExt block_id, td::uint32 priority,
                                                                 td::Timestamp timeout,
                                                                 td::Promise<td::BufferSlice> promise) {
  auto shard = get_shard(block_id.shard_full(), /* historical = */ true);
  if (shard.empty()) {
    VLOG(FULL_NODE_WARNING) << "dropping download proof link query to unknown shard";
    promise.set_error(td::Status::Error(ErrorCode::notready, "shard not ready"));
    return;
  }
  td::actor::send_closure(shard, &FullNodeShard::download_block_proof_link, block_id, priority, timeout,
                          std::move(promise));
}

void FullNodeImpl::get_next_key_blocks(BlockIdExt block_id, td::Timestamp timeout,
                                       td::Promise<std::vector<BlockIdExt>> promise) {
  auto shard = get_shard(block_id.shard_full());
  if (shard.empty()) {
    VLOG(FULL_NODE_WARNING) << "dropping download proof link query to unknown shard";
    promise.set_error(td::Status::Error(ErrorCode::notready, "shard not ready"));
    return;
  }
  td::actor::send_closure(shard, &FullNodeShard::get_next_key_blocks, block_id, timeout, std::move(promise));
}

void FullNodeImpl::download_archive(BlockSeqno masterchain_seqno, ShardIdFull shard_prefix, std::string tmp_dir,
                                    td::Timestamp timeout, bool allow_custom_overlay,
                                    td::Promise<std::string> promise) {
  auto public_archive_hint_peers = [&]() {
    std::vector<adnl::AdnlNodeIdShort> peers;
    for (const auto &[_, custom_overlay] : custom_overlays_) {
      if (!custom_overlay.params_.send_shard(shard_prefix)) {
        continue;
      }
      for (const auto &peer : custom_overlay.params_.nodes_) {
        if (peer == adnl_id_ || std::find(peers.begin(), peers.end(), peer) != peers.end()) {
          continue;
        }
        peers.push_back(peer);
      }
    }
    return peers;
  }();
  if (!allow_custom_overlay) {
    record_custom_overlay_sync_fallback(CustomOverlaySyncKind::Archive,
                                        CustomOverlaySyncFallbackReason::BadArchiveImport);
    record_public_overlay_sync_download(CustomOverlaySyncKind::Archive, PublicOverlaySyncReason::Fallback);
    LOG(INFO) << "forcing public overlay archive slice #" << masterchain_seqno << " " << shard_prefix.to_str()
              << " after custom archive import failure";
    download_archive_from_public_overlay(masterchain_seqno, shard_prefix, std::move(tmp_dir), timeout,
                                         std::move(promise), std::move(public_archive_hint_peers));
    return;
  }
  if (client_.empty()) {
    bool has_custom_overlay = !custom_overlays_.empty();
    bool shard_served_by_custom_overlay = false;
    for (auto &[name, custom_overlay] : custom_overlays_) {
      if (!custom_overlay.params_.send_shard(shard_prefix)) {
        continue;
      }
      shard_served_by_custom_overlay = true;
      for (auto &[local_id, actor] : custom_overlay.actors_) {
        auto state = std::make_shared<PublicArchiveFallbackRaceState>(masterchain_seqno, shard_prefix, std::move(promise));
        auto P = td::PromiseCreator::lambda(
            [state, masterchain_seqno, shard_prefix, name](td::Result<std::string> R) mutable {
              if (R.is_error()) {
                auto error = R.error().to_string();
                LOG(INFO) << "failed to download archive slice #" << masterchain_seqno << " " << shard_prefix.to_str()
                          << " from custom overlay \"" << name << "\": " << error
                          << "; public overlay fallback is already racing";
                record_custom_overlay_sync_fallback(CustomOverlaySyncKind::Archive,
                                                    CustomOverlaySyncFallbackReason::CustomError);
              }
              finish_public_archive_fallback_race(std::move(state), "custom", std::move(R));
            });
        auto PublicP = td::PromiseCreator::lambda([state](td::Result<std::string> R) mutable {
          finish_public_archive_fallback_race(std::move(state), "public", std::move(R));
        });
        record_public_overlay_sync_download(CustomOverlaySyncKind::Archive, PublicOverlaySyncReason::Fallback);
        LOG(WARNING) << "[archive-sync] stage=race_start seqno=" << masterchain_seqno
                     << " shard=" << shard_prefix.to_str()
                     << " overlay=" << name
                     << " local=" << local_id
                     << " result=ok";
        td::actor::send_closure(actor, &FullNodeCustomOverlay::download_archive, masterchain_seqno, shard_prefix,
                                tmp_dir, timeout, std::move(P));
        download_archive_from_public_overlay(masterchain_seqno, shard_prefix, std::move(tmp_dir), timeout,
                                             std::move(PublicP), std::move(public_archive_hint_peers));
        return;
      }
    }
    record_custom_overlay_sync_fallback(
        CustomOverlaySyncKind::Archive,
        shard_served_by_custom_overlay
            ? CustomOverlaySyncFallbackReason::NoLocalActor
            : (has_custom_overlay ? CustomOverlaySyncFallbackReason::ShardNotServed
                                  : CustomOverlaySyncFallbackReason::NoCustomOverlay));
  }
  record_public_overlay_sync_download(CustomOverlaySyncKind::Archive, PublicOverlaySyncReason::Direct);
  download_archive_from_public_overlay(masterchain_seqno, shard_prefix, std::move(tmp_dir), timeout, std::move(promise),
                                       std::move(public_archive_hint_peers));
}

void FullNodeImpl::download_archive_from_public_overlay(BlockSeqno masterchain_seqno, ShardIdFull shard_prefix,
                                                        std::string tmp_dir, td::Timestamp timeout,
                                                        td::Promise<std::string> promise,
                                                        std::vector<adnl::AdnlNodeIdShort> hint_peers) {
  auto shard = get_shard(shard_prefix, /* historical = */ true);
  if (shard.empty()) {
    VLOG(FULL_NODE_WARNING) << "dropping download archive query to unknown shard";
    promise.set_error(td::Status::Error(ErrorCode::notready, "shard not ready"));
    return;
  }
  CHECK(!shard.empty());
  td::actor::send_closure(shard, &FullNodeShard::download_archive, masterchain_seqno, shard_prefix, std::move(tmp_dir),
                          timeout, std::move(promise), std::move(hint_peers));
}

void FullNodeImpl::download_out_msg_queue_proof(ShardIdFull dst_shard, std::vector<BlockIdExt> blocks,
                                                block::ImportedMsgQueueLimits limits, td::Timestamp timeout,
                                                td::Promise<std::vector<td::Ref<OutMsgQueueProof>>> promise) {
  if (blocks.empty()) {
    promise.set_value({});
    return;
  }
  // All blocks are expected to have the same minsplit shard prefix
  auto shard = get_shard(blocks[0].shard_full());
  if (shard.empty()) {
    VLOG(FULL_NODE_WARNING) << "dropping download msg queue query to unknown shard";
    promise.set_error(td::Status::Error(ErrorCode::notready, "shard not ready"));
    return;
  }
  td::actor::send_closure(shard, &FullNodeShard::download_out_msg_queue_proof, dst_shard, std::move(blocks), limits,
                          timeout, std::move(promise));
}

td::actor::ActorId<FullNodeShard> FullNodeImpl::get_shard(ShardIdFull shard, bool historical) {
  if (shard.is_masterchain()) {
    return shards_[ShardIdFull{masterchainId}].actor.get();
  }
  if (shard.workchain != basechainId) {
    return {};
  }
  int pfx_len = shard.pfx_len();
  int min_split = wc_monitor_min_split_;
  if (historical) {
    min_split = td::Random::fast(0, min_split);
  }
  if (pfx_len > min_split) {
    shard = shard_prefix(shard, min_split);
  }
  while (true) {
    auto it = shards_.find(shard);
    if (it != shards_.end()) {
      update_shard_actor(shard, it->second.active);
      return it->second.actor.get();
    }
    if (shard.pfx_len() == 0) {
      break;
    }
    shard = shard_parent(shard);
  }

  // Special case if shards_ was not yet initialized.
  // This can happen briefly on node startup.
  return shards_[ShardIdFull{masterchainId}].actor.get();
}

td::actor::ActorId<FullNodeShard> FullNodeImpl::get_shard(AccountIdPrefixFull dst) {
  return get_shard(shard_prefix(dst, max_shard_pfx_len));
}

void FullNodeImpl::got_key_block_config(td::Ref<ConfigHolder> config) {
  PublicKeyHash l = PublicKeyHash::zero();
  std::vector<PublicKeyHash> keys;
  std::map<PublicKeyHash, adnl::AdnlNodeIdShort> current_validators;
  for (td::int32 i = -1; i <= 1; i++) {
    auto r = config->get_total_validator_set(i < 0 ? i : 1 - i);
    if (r.not_null()) {
      auto vec = r->export_vector();
      for (auto &el : vec) {
        auto key = ValidatorFullId{el.key}.compute_short_id();
        keys.push_back(key);
        if (local_keys_.count(key)) {
          l = key;
        }
        if (i == 1) {
          current_validators[key] = adnl::AdnlNodeIdShort{el.addr.is_zero() ? key.bits256_value() : el.addr};
        }
      }
    }
  }

  if (current_validators != current_validators_) {
    current_validators_ = std::move(current_validators);
    update_private_overlays();
  }

  // Let's turn off this optimization, since keyblocks are rare enough to update on each keyblock
  // if (keys == all_validators_) {
  //   return;
  // }

  all_validators_ = keys;
  sign_cert_by_ = l;
  CHECK(all_validators_.size() > 0);

  for (auto &shard : shards_) {
    if (!shard.second.actor.empty()) {
      td::actor::send_closure(shard.second.actor, &FullNodeShard::update_validators, all_validators_, sign_cert_by_);
    }
  }
}

void FullNodeImpl::new_key_block(BlockHandle handle) {
  if (handle->id().seqno() == 0) {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<ShardState>> R) {
      if (R.is_error()) {
        VLOG(FULL_NODE_WARNING) << "failed to get zero state: " << R.move_as_error();
      } else {
        auto s = td::Ref<MasterchainState>{R.move_as_ok()};
        CHECK(s.not_null());
        td::actor::send_closure(SelfId, &FullNodeImpl::got_key_block_config, s->get_config_holder().move_as_ok());
      }
    });
    td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::get_shard_state_from_db, handle,
                            std::move(P));
  } else {
    CHECK(handle->is_key_block());
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<ProofLink>> R) {
      if (R.is_error()) {
        VLOG(FULL_NODE_WARNING) << "failed to get key block proof: " << R.move_as_error();
      } else {
        td::actor::send_closure(SelfId, &FullNodeImpl::got_key_block_config,
                                R.ok()->get_key_block_config().move_as_ok());
      }
    });
    td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::get_block_proof_link_from_db, handle,
                            std::move(P));
  }
}

void FullNodeImpl::process_block_broadcast(BlockBroadcast broadcast, bool signatures_checked, bool from_custom_overlay) {
  const auto block_id = broadcast.block_id;
  const auto trace = broadcast.trace;
  const bool final_known = !broadcast.sig_set.is_null();
  const bool final = final_known && broadcast.sig_set->is_final();
  const char *source = from_custom_overlay ? "custom" : "public";
  if (from_custom_overlay) {
    overlay_gap::remember(block_id, "fullnode.process", trace.overlay_name, trace.src_adnl, {}, final_known && final);
    log_block_propagation_stage(broadcast, "fullnode.process", source, "ok", {}, trace.custom_deserialized_at);
  }
  if (from_custom_overlay) {
    record_custom_overlay_block_broadcast_received();
  }
  send_block_broadcast_to_custom_overlays(broadcast);
  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::new_block_broadcast, std::move(broadcast),
                          signatures_checked, [block_id, trace, final_known, final, source](td::Result<td::Unit> R) {
                            if (R.is_error()) {
                              auto error = R.move_as_error();
                              if (source == std::string{"custom"}) {
                                overlay_gap::remember(block_id, "fullnode.process", trace.overlay_name, trace.src_adnl,
                                                      error.to_string(), final_known && final);
                              }
                              log_block_propagation_stage(block_id, trace, "fullnode.process", source, final_known, final,
                                                          error.code() == ErrorCode::notready ? "drop" : "error",
                                                          error.to_string(), trace.custom_deserialized_at);
                              if (error.code() == ErrorCode::notready) {
                                LOG(DEBUG) << "dropped broadcast: " << error;
                              } else {
                                LOG(INFO) << "dropped broadcast: " << error;
                              }
                            }
                          },
                          from_custom_overlay);
}

void FullNodeImpl::process_block_candidate_broadcast(BlockIdExt block_id, CatchainSeqno cc_seqno,
                                                     td::uint32 validator_set_hash, td::BufferSlice data) {
  send_block_candidate_broadcast_to_custom_overlays(block_id, cc_seqno, validator_set_hash, data);
  td::actor::ask(validator_manager_, &ValidatorManagerInterface::new_block_candidate_broadcast, block_id, cc_seqno,
                 std::move(data))
      .detach();
}

void FullNodeImpl::process_shard_block_info_broadcast(BlockIdExt block_id, CatchainSeqno cc_seqno,
                                                      td::BufferSlice data) {
  send_shard_block_info_to_custom_overlays(block_id, cc_seqno, data);
  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::new_shard_block_description_broadcast,
                          block_id, cc_seqno, std::move(data));
}

void FullNodeImpl::get_out_msg_queue_query_token(td::Promise<std::unique_ptr<ActionToken>> promise) {
  td::actor::send_closure(out_msg_queue_query_token_manager_, &TokenManager::get_token, 1, 0, td::Timestamp::in(10.0),
                          std::move(promise));
}

void FullNodeImpl::set_validator_telemetry_filename(std::string value) {
  validator_telemetry_filename_ = std::move(value);
  update_validator_telemetry_collector();
}

void FullNodeImpl::update_validator_telemetry_collector() {
  if (validator_telemetry_filename_.empty()) {
    validator_telemetry_collector_key_ = PublicKeyHash::zero();
    return;
  }
  if (fast_sync_overlays_.get_masterchain_overlay_for(adnl::AdnlNodeIdShort{validator_telemetry_collector_key_})
          .empty()) {
    auto [actor, adnl_id] = fast_sync_overlays_.choose_overlay(ShardIdFull{masterchainId});
    validator_telemetry_collector_key_ = adnl_id.pubkey_hash();
    if (!actor.empty()) {
      td::actor::send_closure(actor, &FullNodeFastSyncOverlay::collect_validator_telemetry,
                              validator_telemetry_filename_);
    }
  }
}

void FullNodeImpl::start_up() {
  update_shard_actor(ShardIdFull{masterchainId}, true);
  if (local_id_.is_zero()) {
    if (adnl_id_.is_zero()) {
      auto pk = ton::PrivateKey{ton::privkeys::Ed25519::random()};
      local_id_ = pk.compute_short_id();

      td::actor::send_closure(keyring_, &ton::keyring::Keyring::add_key, std::move(pk), true, [](td::Result<>) {});
    } else {
      local_id_ = adnl_id_.pubkey_hash();
    }
  }
  class Callback : public ValidatorManagerInterface::Callback {
   public:
    void initial_read_complete(BlockHandle handle) override {
      td::actor::send_closure(id_, &FullNodeImpl::initial_read_complete, handle);
    }
    void on_new_masterchain_block(td::Ref<MasterchainState> state, std::set<ShardIdFull> shards_to_monitor) override {
      td::actor::send_closure(id_, &FullNodeImpl::on_new_masterchain_block, std::move(state),
                              std::move(shards_to_monitor));
    }
    void send_ihr_message(AccountIdPrefixFull dst, td::BufferSlice data) override {
      td::actor::send_closure(id_, &FullNodeImpl::send_ihr_message, dst, std::move(data));
    }
    void send_ext_message(AccountIdPrefixFull dst, td::BufferSlice data) override {
      td::actor::send_closure(id_, &FullNodeImpl::send_ext_message, dst, std::move(data));
    }
    void send_ext_message_relay_all(AccountIdPrefixFull dst, td::BufferSlice data) override {
      td::actor::send_closure(id_, &FullNodeImpl::send_ext_message_relay_all, dst, std::move(data));
    }
    void send_ext_message_raw_all(td::BufferSlice data) override {
      td::actor::send_closure(id_, &FullNodeImpl::send_ext_message_raw_all, std::move(data));
    }
    void send_shard_block_info(BlockIdExt block_id, CatchainSeqno cc_seqno, td::BufferSlice data) override {
      td::actor::send_closure(id_, &FullNodeImpl::send_shard_block_info, block_id, cc_seqno, std::move(data));
    }
    void send_block_candidate(BlockIdExt block_id, CatchainSeqno cc_seqno, td::uint32 validator_set_hash,
                              td::BufferSlice data, int mode) override {
      td::actor::send_closure(id_, &FullNodeImpl::send_block_candidate, block_id, cc_seqno, validator_set_hash,
                              std::move(data), mode);
    }
    void send_out_msg_queue_proof_broadcast(td::Ref<OutMsgQueueProofBroadcast> broadcast) override {
      td::actor::send_closure(id_, &FullNodeImpl::send_out_msg_queue_proof_broadcast, std::move(broadcast));
    }
    void send_broadcast(validator::BlockBroadcast broadcast, int mode) override {
      td::actor::send_closure(id_, &FullNodeImpl::send_broadcast, std::move(broadcast), mode);
    }
    void download_block(BlockIdExt id, td::uint32 priority, td::Timestamp timeout,
                        td::Promise<validator::ReceivedBlock> promise) override {
      td::actor::send_closure(id_, &FullNodeImpl::download_block, id, priority, timeout, std::move(promise));
    }
    void download_zero_state(BlockIdExt id, td::uint32 priority, td::Timestamp timeout,
                             td::Promise<td::BufferSlice> promise) override {
      td::actor::send_closure(id_, &FullNodeImpl::download_zero_state, id, priority, timeout, std::move(promise));
    }
    void download_persistent_state(BlockIdExt id, BlockIdExt masterchain_block_id, PersistentStateType type,
                                   td::uint32 priority, td::Timestamp timeout,
                                   td::Promise<td::BufferSlice> promise) override {
      td::actor::send_closure(id_, &FullNodeImpl::download_persistent_state, id, masterchain_block_id, type, priority,
                              timeout, std::move(promise));
    }
    void download_block_proof(BlockIdExt block_id, td::uint32 priority, td::Timestamp timeout,
                              td::Promise<td::BufferSlice> promise) override {
      td::actor::send_closure(id_, &FullNodeImpl::download_block_proof, block_id, priority, timeout,
                              std::move(promise));
    }
    void download_block_proof_link(BlockIdExt block_id, td::uint32 priority, td::Timestamp timeout,
                                   td::Promise<td::BufferSlice> promise) override {
      td::actor::send_closure(id_, &FullNodeImpl::download_block_proof_link, block_id, priority, timeout,
                              std::move(promise));
    }
    void get_next_key_blocks(BlockIdExt block_id, td::Timestamp timeout,
                             td::Promise<std::vector<BlockIdExt>> promise) override {
      td::actor::send_closure(id_, &FullNodeImpl::get_next_key_blocks, block_id, timeout, std::move(promise));
    }
    void download_archive(BlockSeqno masterchain_seqno, ShardIdFull shard_prefix, std::string tmp_dir,
                          td::Timestamp timeout, bool allow_custom_overlay,
                          td::Promise<std::string> promise) override {
      td::actor::send_closure(id_, &FullNodeImpl::download_archive, masterchain_seqno, shard_prefix, std::move(tmp_dir),
                              timeout, allow_custom_overlay, std::move(promise));
    }
    void download_out_msg_queue_proof(ShardIdFull dst_shard, std::vector<BlockIdExt> blocks,
                                      block::ImportedMsgQueueLimits limits, td::Timestamp timeout,
                                      td::Promise<std::vector<td::Ref<OutMsgQueueProof>>> promise) override {
      td::actor::send_closure(id_, &FullNodeImpl::download_out_msg_queue_proof, dst_shard, std::move(blocks), limits,
                              timeout, std::move(promise));
    }

    void new_key_block(BlockHandle handle) override {
      td::actor::send_closure(id_, &FullNodeImpl::new_key_block, std::move(handle));
    }

    explicit Callback(td::actor::ActorId<FullNodeImpl> id) : id_(id) {
    }

   private:
    td::actor::ActorId<FullNodeImpl> id_;
  };

  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::install_callback,
                          std::make_unique<Callback>(actor_id(this)), std::move(started_promise_));
}

void FullNodeImpl::update_private_overlays() {
  for (auto &p : custom_overlays_) {
    update_custom_overlay(p.second);
  }

  update_validator_telemetry_collector();
  if (local_keys_.empty()) {
    return;
  }
}

void FullNodeImpl::update_custom_overlay(CustomOverlayInfo &overlay) {
  auto old_actors = std::move(overlay.actors_);
  overlay.actors_.clear();
  CustomOverlayParams &params = overlay.params_;
  auto try_local_id = [&](const adnl::AdnlNodeIdShort &local_id) {
    if (std::find(params.nodes_.begin(), params.nodes_.end(), local_id) != params.nodes_.end()) {
      auto it = old_actors.find(local_id);
      if (it != old_actors.end()) {
        overlay.actors_[local_id] = std::move(it->second);
        old_actors.erase(it);
      } else {
        auto adnl_sender = (params.use_quic_ ? td::actor::ActorId<adnl::AdnlSenderEx>{quic_} : rldp2_);
        overlay.actors_[local_id] = td::actor::create_actor<FullNodeCustomOverlay>(
            "CustomOverlay", local_id, params, zero_state_file_hash_, opts_, keyring_, adnl_, adnl_sender, overlays_,
            validator_manager_, actor_id(this), limiter_);
      }
    }
  };
  try_local_id(adnl_id_);
  for (const PublicKeyHash &local_key : local_keys_) {
    auto it = current_validators_.find(local_key);
    if (it != current_validators_.end()) {
      try_local_id(it->second);
    }
  }
}

void FullNodeImpl::send_block_broadcast_to_custom_overlays(const BlockBroadcast &broadcast) {
  constexpr td::uint32 sent_non_final_broadcast = 1;
  constexpr td::uint32 sent_final_broadcast = 2;
  auto sent_flag = !broadcast.sig_set.is_null() && broadcast.sig_set->is_final() ? sent_final_broadcast
                                                                                 : sent_non_final_broadcast;
  auto *sent_flags = custom_overlays_sent_broadcasts_.get_if_exists(broadcast.block_id);
  if (sent_flags && (*sent_flags & sent_flag)) {
    return;
  }
  custom_overlays_sent_broadcasts_.put(broadcast.block_id, (sent_flags ? *sent_flags : 0) | sent_flag);
  for (auto &[_, private_overlay] : custom_overlays_) {
    if (private_overlay.params_.send_shard(broadcast.block_id.shard_full())) {
      for (auto &[local_id, actor] : private_overlay.actors_) {
        if (private_overlay.params_.block_senders_.find(local_id) != private_overlay.params_.block_senders_.end()) {
          auto trace = broadcast.trace;
          if (trace.overlay_name.empty()) {
            trace.overlay_name = private_overlay.params_.name_;
          }
          if (trace.src_adnl.empty()) {
            trace.src_adnl = local_id.bits256_value().to_hex();
          }
          log_block_propagation_stage(broadcast.block_id, trace, "fullnode.send_custom",
                                      broadcast.trace.enabled ? "custom" : "public", !broadcast.sig_set.is_null(),
                                      !broadcast.sig_set.is_null() && broadcast.sig_set->is_final(), "ok", {}, 0.0,
                                      true);
          td::actor::send_closure(actor, &FullNodeCustomOverlay::send_broadcast, broadcast.clone());
        }
      }
    }
  }
}

void FullNodeImpl::send_block_candidate_broadcast_to_custom_overlays(const BlockIdExt &block_id, CatchainSeqno cc_seqno,
                                                                     td::uint32 validator_set_hash,
                                                                     const td::BufferSlice &data) {
  if (custom_overlays_sent_block_candidates_.contains(block_id)) {
    return;
  }
  custom_overlays_sent_block_candidates_.put(block_id, {});
  for (auto &[_, private_overlay] : custom_overlays_) {
    if (private_overlay.params_.send_shard(block_id.shard_full())) {
      for (auto &[local_id, actor] : private_overlay.actors_) {
        if (private_overlay.params_.block_senders_.find(local_id) != private_overlay.params_.block_senders_.end()) {
          td::actor::send_closure(actor, &FullNodeCustomOverlay::send_block_candidate, block_id, cc_seqno,
                                  validator_set_hash, data.clone());
        }
      }
    }
  }
}

void FullNodeImpl::send_shard_block_info_to_custom_overlays(BlockIdExt block_id, CatchainSeqno cc_seqno,
                                                            const td::BufferSlice &data) {
  if (custom_overlays_sent_shard_block_desc_.contains(block_id)) {
    return;
  }
  custom_overlays_sent_shard_block_desc_.put(block_id, {});
  for (auto &[_, private_overlay] : custom_overlays_) {
    if (private_overlay.params_.send_shard(block_id.shard_full())) {
      for (auto &[local_id, actor] : private_overlay.actors_) {
        if (private_overlay.params_.block_senders_.contains(local_id)) {
          td::actor::send_closure(actor, &FullNodeCustomOverlay::send_shard_block_info, block_id, cc_seqno,
                                  data.clone());
        }
      }
    }
  }
}

FullNodeImpl::FullNodeImpl(PublicKeyHash local_id, adnl::AdnlNodeIdShort adnl_id, FileHash zero_state_file_hash,
                           FullNodeOptions opts, td::actor::ActorId<keyring::Keyring> keyring,
                           td::actor::ActorId<adnl::Adnl> adnl, td::actor::ActorId<rldp2::Rldp> rldp2,
                           td::actor::ActorId<quic::QuicSender> quic, td::actor::ActorId<dht::Dht> dht,
                           td::actor::ActorId<overlay::Overlays> overlays,
                           td::actor::ActorId<ValidatorManagerInterface> validator_manager,
                           td::actor::ActorId<adnl::AdnlExtClient> client, std::string db_root,
                           td::Promise<td::Unit> started_promise)
    : local_id_(local_id)
    , adnl_id_(adnl_id)
    , zero_state_file_hash_(zero_state_file_hash)
    , keyring_(keyring)
    , adnl_(adnl)
    , rldp2_(rldp2)
    , quic_(quic)
    , dht_(dht)
    , overlays_(overlays)
    , validator_manager_(validator_manager)
    , client_(client)
    , db_root_(db_root)
    , started_promise_(std::move(started_promise))
    , opts_(opts)
    , limiter_(make_limiter(opts)) {
}

td::actor::ActorOwn<FullNode> FullNode::create(
    ton::PublicKeyHash local_id, adnl::AdnlNodeIdShort adnl_id, FileHash zero_state_file_hash, FullNodeOptions opts,
    td::actor::ActorId<keyring::Keyring> keyring, td::actor::ActorId<adnl::Adnl> adnl,
    td::actor::ActorId<rldp2::Rldp> rldp2, td::actor::ActorId<quic::QuicSender> quic, td::actor::ActorId<dht::Dht> dht,
    td::actor::ActorId<overlay::Overlays> overlays, td::actor::ActorId<ValidatorManagerInterface> validator_manager,
    td::actor::ActorId<adnl::AdnlExtClient> client, std::string db_root, td::Promise<td::Unit> started_promise) {
  return td::actor::create_actor<FullNodeImpl>("fullnode", local_id, adnl_id, zero_state_file_hash, opts, keyring, adnl,
                                               rldp2, quic, dht, overlays, validator_manager, client, db_root,
                                               std::move(started_promise));
}

FullNodeConfig::FullNodeConfig(const tl_object_ptr<ton_api::engine_validator_fullNodeConfig> &obj)
    : ext_messages_broadcast_disabled_(obj->ext_messages_broadcast_disabled_) {
}

tl_object_ptr<ton_api::engine_validator_fullNodeConfig> FullNodeConfig::tl() const {
  return create_tl_object<ton_api::engine_validator_fullNodeConfig>(ext_messages_broadcast_disabled_);
}

bool CustomOverlayParams::send_shard(const ShardIdFull &shard) const {
  return sender_shards_.empty() ||
         std::any_of(sender_shards_.begin(), sender_shards_.end(),
                     [&](const ShardIdFull &our_shard) { return shard_intersects(shard, our_shard); });
}

CustomOverlayParams CustomOverlayParams::fetch(const ton_api::engine_validator_customOverlay &f) {
  CustomOverlayParams c;
  c.name_ = f.name_;
  for (const auto &node : f.nodes_) {
    c.nodes_.emplace_back(node->adnl_id_);
    if (node->msg_sender_) {
      c.msg_senders_[adnl::AdnlNodeIdShort{node->adnl_id_}] = node->msg_sender_priority_;
    }
    if (node->block_sender_) {
      c.block_senders_.emplace(node->adnl_id_);
    }
  }
  for (const auto &shard : f.sender_shards_) {
    c.sender_shards_.push_back(create_shard_id(shard));
  }
  c.skip_public_msg_send_ = f.skip_public_msg_send_;
  c.use_quic_ = f.use_quic_;
  return c;
}

decltype(FullNodeImpl::limiter_) FullNodeImpl::make_limiter(const FullNodeOptions &opts) {
  double w_size = opts.ratelimit_window_size_;
  size_t h_limit = opts.ratelimit_heavy_;
  size_t m_limit = opts.ratelimit_medium_;
  size_t g_limit = opts.ratelimit_global_;
  return std::make_shared<RateLimiter<>>(
      RateLimit{w_size, g_limit},
      std::map<int32_t, RateLimit>{{ton_api::tonNode_getArchiveSlice::ID, {w_size, h_limit}},
                                   {ton_api::tonNode_downloadPersistentStateSliceV2::ID, {w_size, h_limit}},
                                   {ton_api::tonNode_downloadZeroState::ID, {w_size, h_limit}},

                                   {ton_api::tonNode_downloadBlock::ID, {w_size, m_limit}},
                                   {ton_api::tonNode_downloadBlockFull::ID, {w_size, m_limit}},
                                   {ton_api::tonNode_downloadNextBlockFull::ID, {w_size, m_limit}},
                                   {ton_api::tonNode_downloadBlockProof::ID, {w_size, m_limit}},
                                   {ton_api::tonNode_downloadBlockProofLink::ID, {w_size, m_limit}},
                                   {ton_api::tonNode_downloadKeyBlockProof::ID, {w_size, m_limit}},
                                   {ton_api::tonNode_downloadKeyBlockProofLink::ID, {w_size, m_limit}},
                                   {ton_api::tonNode_getOutMsgQueueProof::ID, {w_size, m_limit}}});
}

}  // namespace fullnode

}  // namespace validator

}  // namespace ton
