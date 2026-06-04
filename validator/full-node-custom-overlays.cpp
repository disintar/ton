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
*/
#include <algorithm>
#include <cstring>
#include <mutex>

#include "auto/tl/ton_api_json.h"
#include "common/checksum.h"
#include "common/delay.h"
#include "net/download-archive-slice.hpp"
#include "net/download-block-new.hpp"
#include "net/download-proof.hpp"
#include "td/utils/JsonBuilder.h"
#include "tl/tl_json.h"
#include "ton/ton-shard.h"
#include "ton/ton-tl.hpp"

#include "block-propagation-trace.h"
#include "custom-overlay-metrics.h"
#include "full-node-custom-overlays.hpp"
#include "overlay-gap-diagnostics.h"
#include "full-node-shard-queries.hpp"
#include "full-node-serializer.hpp"

namespace ton::validator::fullnode {

namespace {

constexpr const char *k_called_from_custom = "custom";
constexpr td::uint32 k_heavy_request_cost_unit = 1 << 21;
constexpr long long CUSTOM_OVERLAY_SYNC_SLOW_LOG_MS = 800;

size_t heavy_request_cost(td::uint64 requested_max_size) {
  size_t cost = static_cast<size_t>((requested_max_size + k_heavy_request_cost_unit - 1) / k_heavy_request_cost_unit);
  return cost == 0 ? 1 : cost;
}

size_t request_cost_for_custom_overlay_query(ton_api::Function &function) {
  size_t cost = 1;
  ton_api::downcast_call(function, td::overloaded(
                                       [&](const ton_api::tonNode_getArchiveSlice &query) {
                                         cost = heavy_request_cost(
                                             query.max_size_ > 0 ? static_cast<td::uint64>(query.max_size_) : 0);
                                       },
                                       [&](const ton_api::tonNode_downloadBlockFull &) {
                                         cost = heavy_request_cost(FullNode::max_proof_size() +
                                                                   FullNode::max_block_size());
                                       },
                                       [&](const ton_api::tonNode_downloadNextBlockFull &) {
                                         cost = heavy_request_cost(FullNode::max_proof_size() +
                                                                   FullNode::max_block_size());
                                       },
                                       [&](const auto &) {}));
  return cost;
}

bool custom_overlay_serves_shard(const std::vector<ShardIdFull> &sender_shards, const ShardIdFull &shard) {
  return sender_shards.empty() ||
         std::any_of(sender_shards.begin(), sender_shards.end(),
                     [&](const ShardIdFull &our_shard) { return shard_intersects(shard, our_shard); });
}

struct CustomOverlaySyncLogTarget {
  std::string block;
  td::int32 wc = 0;
  td::uint64 shard = 0;
  td::uint32 seqno = 0;
};

CustomOverlaySyncLogTarget custom_overlay_sync_log_target(const BlockIdExt &id) {
  return {id.to_str(), id.id.workchain, id.id.shard, id.id.seqno};
}

std::string custom_overlay_sync_adnl_to_string(adnl::AdnlNodeIdShort id) {
  return PSTRING() << id;
}

CustomOverlaySyncSender custom_overlay_sync_sender(bool use_quic) {
  return use_quic ? CustomOverlaySyncSender::Quic : CustomOverlaySyncSender::Rldp2;
}

CustomOverlaySyncResult custom_overlay_sync_result_from_status(const td::Status &error) {
  return error.code() == ErrorCode::timeout ? CustomOverlaySyncResult::Timeout : CustomOverlaySyncResult::Error;
}

bool custom_sync_log_value_is(const char *value, const char *expected) {
  return std::strcmp(value, expected) == 0;
}

bool should_log_custom_overlay_sync_stage(CustomOverlaySyncKind kind, const char *stage, const char *result,
                                          long long ms) {
  if (block_propagation_trace_enabled()) {
    return true;
  }
  if (!custom_sync_log_value_is(stage, "custom.done")) {
    return false;
  }
  if (!custom_sync_log_value_is(result, "ok")) {
    return true;
  }
  return kind == CustomOverlaySyncKind::Archive || ms >= CUSTOM_OVERLAY_SYNC_SLOW_LOG_MS;
}

void log_custom_overlay_sync_stage(CustomOverlaySyncKind kind, CustomOverlaySyncSender sender,
                                   const std::string &overlay, const std::string &local,
                                   const std::string &peer, const CustomOverlaySyncLogTarget &target,
                                   const char *stage, const char *result, std::string reason, std::size_t peers,
                                   double started_at) {
  auto now = block_propagation_trace_now();
  auto ms = block_propagation_trace_ms(started_at, now);
  if (!should_log_custom_overlay_sync_stage(kind, stage, result, ms)) {
    return;
  }
  LOG(WARNING) << "[custom-overlay-sync]"
               << " stage=" << stage
               << " kind=" << custom_overlay_sync_kind_label(metric_index(kind))
               << " source=custom"
               << " sender=" << custom_overlay_sync_sender_label(metric_index(sender))
               << " overlay=" << block_propagation_trace_sanitize(overlay)
               << " local=" << block_propagation_trace_sanitize(local)
               << " peer=" << block_propagation_trace_sanitize(peer)
               << " block=" << target.block
               << " wc=" << target.wc
               << " shard=" << target.shard
               << " seqno=" << target.seqno
               << " peers=" << peers
               << " ms=" << ms
               << " result=" << result
               << " reason=" << block_propagation_trace_sanitize(std::move(reason));
}

struct CustomOverlaySyncDownloadState {
  CustomOverlaySyncDownloadState(CustomOverlaySyncKind kind, CustomOverlaySyncSender sender, double started_at,
                                 std::size_t pending, std::string overlay_name, std::string local_id,
                                 CustomOverlaySyncLogTarget target, td::Promise<ReceivedBlock> promise)
      : kind(kind)
      , sender(sender)
      , started_at(started_at)
      , pending(pending)
      , peers_total(pending)
      , overlay_name(std::move(overlay_name))
      , local_id(std::move(local_id))
      , target(std::move(target))
      , promise(std::move(promise)) {
  }

  CustomOverlaySyncKind kind;
  CustomOverlaySyncSender sender;
  double started_at = 0.0;
  std::size_t pending = 0;
  std::size_t peers_total = 0;
  std::string overlay_name;
  std::string local_id;
  CustomOverlaySyncLogTarget target;
  td::Promise<ReceivedBlock> promise;
  std::mutex mutex;
  bool done = false;
  std::string last_error;
};

void finish_custom_overlay_sync_download(std::shared_ptr<CustomOverlaySyncDownloadState> state,
                                         td::Result<ReceivedBlock> R, double peer_started_at) {
  auto now = block_propagation_trace_now();
  if (R.is_ok()) {
    record_custom_overlay_sync_peer_download(state->kind, state->sender, CustomOverlaySyncResult::Ok, peer_started_at,
                                             now);
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
      record_custom_overlay_sync_download(state->kind, state->sender, CustomOverlaySyncResult::Ok, state->started_at,
                                          now);
      log_custom_overlay_sync_stage(state->kind, state->sender, state->overlay_name, state->local_id, "-",
                                    state->target, "custom.done", "ok", {}, state->peers_total, state->started_at);
      promise.set_value(std::move(value));
    }
    return;
  }

  auto error = R.move_as_error();
  auto peer_result = custom_overlay_sync_result_from_status(error);
  record_custom_overlay_sync_peer_download(state->kind, state->sender, peer_result, peer_started_at, now);

  td::Promise<ReceivedBlock> promise;
  bool should_finish = false;
  std::string last_error = error.to_string();
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->done) {
      return;
    }
    state->last_error = last_error;
    CHECK(state->pending > 0);
    state->pending--;
    if (state->pending == 0) {
      state->done = true;
      promise = std::move(state->promise);
      should_finish = true;
      last_error = state->last_error;
    }
  }
  if (should_finish) {
    record_custom_overlay_sync_download(state->kind, state->sender, CustomOverlaySyncResult::Exhausted,
                                        state->started_at, now);
    log_custom_overlay_sync_stage(state->kind, state->sender, state->overlay_name, state->local_id, "-", state->target,
                                  "custom.done", "exhausted", last_error, state->peers_total, state->started_at);
    promise.set_error(td::Status::Error(ErrorCode::notready,
                                        PSTRING() << "no custom overlay peer returned block: " << last_error));
  }
}

}  // namespace

bool FullNodeCustomOverlay::mark_block_broadcast_received(BlockIdExt block_id, bool final) {
  constexpr td::uint32 received_non_final_broadcast = 1;
  constexpr td::uint32 received_final_broadcast = 2;
  auto received_flag = final ? received_final_broadcast : received_non_final_broadcast;
  auto *received_flags = received_block_broadcasts_.get_if_exists(block_id);
  if (received_flags && (*received_flags & received_flag)) {
    record_custom_overlay_duplicate_block_broadcast_dropped();
    return false;
  }
  received_block_broadcasts_.put(block_id, (received_flags ? *received_flags : 0) | received_flag);
  return true;
}

bool FullNodeCustomOverlay::mark_block_candidate_received(BlockIdExt block_id) {
  if (received_block_candidates_.contains(block_id)) {
    record_custom_overlay_duplicate_block_candidate_dropped();
    return false;
  }
  received_block_candidates_.put(block_id, {});
  return true;
}

void FullNodeCustomOverlay::process_broadcast(PublicKeyHash src, ton_api::tonNode_blockBroadcast &query) {
  process_block_broadcast(src, query);
}

void FullNodeCustomOverlay::process_broadcast(PublicKeyHash src, ton_api::tonNode_blockBroadcastCompressed &query) {
  process_block_broadcast(src, query);
}

void FullNodeCustomOverlay::process_broadcast(PublicKeyHash src, ton_api::tonNode_blockBroadcastCompressedV2 &query) {
  auto received_at = block_propagation_trace_now();
  auto trace = make_block_propagation_trace(name_, src.bits256_value().to_hex(), received_at);
  if (!block_senders_.count(adnl::AdnlNodeIdShort(src))) {
    VLOG(FULL_NODE_DEBUG) << "Dropping block broadcast in private overlay \"" << name_ << "\" from unauthorized sender "
                          << src;
    if (trace.enabled) {
      log_block_propagation_stage(create_block_id(query.id_), trace, "custom.recv", "custom", false, false, "drop",
                                  "unauthorized_sender", received_at);
    }
    return;
  }
  auto block_id = create_block_id(query.id_);

  auto R_requires_state = need_state_for_decompression(query);
  if (R_requires_state.is_error()) {
    LOG(DEBUG) << "Failed to check if state is required for broadcast: " << R_requires_state.move_as_error();
    if (trace.enabled) {
      log_block_propagation_stage(block_id, trace, "custom.recv", "custom", false, false, "error",
                                  "need_state_check_failed", received_at);
    }
    return;
  }

  if (R_requires_state.move_as_ok()) {
    if (block_broadcast_signature_set_visible(query) &&
        !mark_block_broadcast_received(block_id, block_broadcast_has_final_signature_set(query))) {
      log_block_propagation_stage(block_id, trace, "custom.recv", "custom", false, false, "drop", "duplicate_broadcast",
                                  received_at);
      return;
    }
    auto block_wo_data = get_block_broadcast_without_data(query);
    block_wo_data.trace = trace;
    log_block_propagation_stage(block_wo_data, "custom.recv", "custom", "ok", {}, received_at);
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), src,
                                         query = std::move(query), trace = std::move(trace)](td::Result<td::Unit> R) mutable {
      if (R.is_error()) {
        LOG(DEBUG) << "Dropped V2 broadcast because of signatures validation error: " << R.move_as_error();
        log_block_propagation_stage(create_block_id(query.id_), trace, "custom.deserialize", "custom", false, false,
                                    "drop", "signatures_validation_error", trace.custom_received_at);
        return;
      }

      td::actor::send_closure(SelfId, &FullNodeCustomOverlay::obtain_state_for_decompression, src, std::move(query),
                              std::move(trace));
    });
    td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::validate_block_broadcast_signatures,
                            std::move(block_wo_data), std::move(P));
    return;
  }

  process_block_broadcast(src, query, received_at);
}

void FullNodeCustomOverlay::process_block_broadcast(PublicKeyHash src, ton_api::tonNode_Broadcast &query,
                                                    double received_at) {
  if (received_at <= 0.0) {
    received_at = block_propagation_trace_now();
  }
  auto trace = make_block_propagation_trace(name_, src.bits256_value().to_hex(), received_at);
  auto R_id = get_block_broadcast_id(query);
  if (!block_senders_.count(adnl::AdnlNodeIdShort(src))) {
    VLOG(FULL_NODE_DEBUG) << "Dropping block broadcast in private overlay \"" << name_ << "\" from unauthorized sender "
                          << src;
    if (R_id.is_ok()) {
      log_block_propagation_stage(R_id.ok(), trace, "custom.recv", "custom", false, false, "drop",
                                  "unauthorized_sender", received_at);
    }
    return;
  }
  if (R_id.is_ok()) {
    if (block_broadcast_signature_set_visible(query) &&
        !mark_block_broadcast_received(R_id.ok(), block_broadcast_has_final_signature_set(query))) {
      overlay_gap::remember(R_id.ok(), "custom.recv", name_, src.bits256_value().to_hex(), "duplicate_broadcast",
                            block_broadcast_has_final_signature_set(query));
      log_block_propagation_stage(R_id.ok(), trace, "custom.recv", "custom", false, false, "drop",
                                  "duplicate_broadcast", received_at);
      return;
    }
    overlay_gap::remember(R_id.ok(), "custom.recv", name_, src.bits256_value().to_hex(), {},
                          block_broadcast_has_final_signature_set(query));
    log_block_propagation_stage(R_id.ok(), trace, "custom.recv", "custom", false, false, "ok", {}, received_at);
  }
  auto deserialize_started_at = block_propagation_trace_now();
  auto B = deserialize_block_broadcast(query, overlay::Overlays::max_fec_broadcast_size(), k_called_from_custom);
  if (B.is_error()) {
    auto error = B.move_as_error();
    if (R_id.is_ok()) {
      log_block_propagation_stage(R_id.ok(), trace, "custom.deserialize", "custom", false, false, "error",
                                  error.to_string(), deserialize_started_at);
    }
    LOG(DEBUG) << "dropped broadcast: " << error;
    return;
  }
  auto broadcast = B.move_as_ok();
  trace.custom_deserialized_at = block_propagation_trace_now();
  broadcast.trace = trace;
  overlay_gap::remember(broadcast.block_id, "custom.deserialize", name_, src.bits256_value().to_hex(), {},
                        !broadcast.sig_set.is_null() && broadcast.sig_set->is_final());
  log_block_propagation_stage(broadcast, "custom.deserialize", "custom", "ok", {}, deserialize_started_at);
  VLOG(FULL_NODE_DEBUG) << "Received block broadcast " << (broadcast.sig_set->is_final() ? "" : "(approve signatures) ")
                        << "in custom overlay \"" << name_ << "\" from " << src << ": " << broadcast.block_id.to_str();
  td::actor::send_closure(full_node_, &FullNode::process_block_broadcast, std::move(broadcast), false, true);
}

void FullNodeCustomOverlay::obtain_state_for_decompression(PublicKeyHash src,
                                                           ton_api::tonNode_blockBroadcastCompressedV2 query,
                                                           BlockPropagationTrace trace) {
  auto id = create_block_id(query.id_);
  auto R_prev = extract_prev_blocks_from_proof(query.proof_.as_slice(), id);
  if (R_prev.is_error()) {
    auto error = R_prev.move_as_error();
    log_block_propagation_stage(id, trace, "custom.deserialize", "custom", false, false, "error", error.to_string(),
                                trace.custom_received_at);
    LOG(DEBUG) << "Failed to extract prev blocks for V2 broadcast: " << error;
    return;
  }
  auto prev_blocks = R_prev.move_as_ok();
  auto P_state = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), src, query = std::move(query), trace = std::move(trace)](
          td::Result<td::Ref<ShardState>> R_state) mutable {
        if (R_state.is_error()) {
          auto error = R_state.move_as_error();
          log_block_propagation_stage(create_block_id(query.id_), trace, "custom.deserialize", "custom", false, false,
                                      "error", error.to_string(), trace.custom_received_at);
          LOG(DEBUG) << "Failed to get state for V2 broadcast: " << error;
          return;
        }
        td::actor::send_closure(SelfId, &FullNodeCustomOverlay::process_block_broadcast_with_state, src,
                                std::move(query), R_state.move_as_ok(), std::move(trace));
      });
  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::wait_state_by_prev_blocks, id,
                          std::move(prev_blocks), std::move(P_state));
}

void FullNodeCustomOverlay::process_block_broadcast_with_state(PublicKeyHash src,
                                                               ton_api::tonNode_blockBroadcastCompressedV2 query,
                                                               td::Ref<ShardState> state,
                                                               BlockPropagationTrace trace) {
  td::Ref<vm::Cell> state_root = state->root_cell();
  auto deserialize_started_at = block_propagation_trace_now();
  auto B =
      deserialize_block_broadcast(query, overlay::Overlays::max_fec_broadcast_size(), k_called_from_custom, state_root);
  if (B.is_error()) {
    auto error = B.move_as_error();
    log_block_propagation_stage(create_block_id(query.id_), trace, "custom.deserialize", "custom", false, false,
                                "error", error.to_string(), deserialize_started_at);
    LOG(DEBUG) << "Failed to deserialize block broadcast: " << error;
    return;
  }
  auto broadcast = B.move_as_ok();
  trace.custom_deserialized_at = block_propagation_trace_now();
  broadcast.trace = trace;
  overlay_gap::remember(broadcast.block_id, "custom.deserialize", name_, src.bits256_value().to_hex(), {},
                        !broadcast.sig_set.is_null() && broadcast.sig_set->is_final());
  log_block_propagation_stage(broadcast, "custom.deserialize", "custom", "ok", {}, deserialize_started_at);
  VLOG(FULL_NODE_DEBUG) << "Received block broadcast in custom overlay \"" << name_ << "\" from " << src << ": "
                        << broadcast.block_id.to_str();
  td::actor::send_closure(full_node_, &FullNode::process_block_broadcast, std::move(broadcast), true, true);
}

void FullNodeCustomOverlay::process_broadcast(PublicKeyHash src, ton_api::tonNode_externalMessageBroadcast &query) {
  auto it = msg_senders_.find(adnl::AdnlNodeIdShort{src});
  if (it == msg_senders_.end()) {
    VLOG(FULL_NODE_DEBUG) << "Dropping external message broadcast in custom overlay \"" << name_
                          << "\" from unauthorized sender " << src;
    return;
  }
  VLOG(FULL_NODE_DEBUG) << "Got external message in custom overlay \"" << name_ << "\" from " << src
                        << " (priority=" << it->second << ")";
  td::actor::ask(validator_manager_, &ValidatorManagerInterface::new_external_message_broadcast,
                 std::move(query.message_->data_), it->second)
      .detach();
}

void FullNodeCustomOverlay::process_broadcast(PublicKeyHash src, ton_api::tonNode_newBlockCandidateBroadcast &query) {
  process_block_candidate_broadcast(src, query);
}

void FullNodeCustomOverlay::process_broadcast(PublicKeyHash src,
                                              ton_api::tonNode_newBlockCandidateBroadcastCompressed &query) {
  process_block_candidate_broadcast(src, query);
}

void FullNodeCustomOverlay::process_broadcast(PublicKeyHash src,
                                              ton_api::tonNode_newBlockCandidateBroadcastCompressedV2 &query) {
  process_block_candidate_broadcast(src, query);
}

void FullNodeCustomOverlay::process_block_candidate_broadcast(PublicKeyHash src, ton_api::tonNode_Broadcast &query) {
  if (!block_senders_.count(adnl::AdnlNodeIdShort(src))) {
    VLOG(FULL_NODE_DEBUG) << "Dropping block candidate broadcast in private overlay \"" << name_
                          << "\" from unauthorized sender " << src;
    return;
  }
  auto R_id = get_block_candidate_broadcast_id(query);
  if (R_id.is_ok() && !mark_block_candidate_received(R_id.ok())) {
    return;
  }
  BlockIdExt block_id;
  CatchainSeqno cc_seqno;
  td::uint32 validator_set_hash;
  td::BufferSlice data;
  auto S = deserialize_block_candidate_broadcast(query, block_id, cc_seqno, validator_set_hash, data,
                                                 overlay::Overlays::max_fec_broadcast_size(), k_called_from_custom);
  if (S.is_error()) {
    LOG(DEBUG) << "dropped broadcast: " << S;
    return;
  }
  if (data.size() > FullNode::max_block_size()) {
    VLOG(FULL_NODE_WARNING) << "received block candidate with too big size from " << src;
    return;
  }
  if (td::sha256_bits256(data.as_slice()) != block_id.file_hash) {
    VLOG(FULL_NODE_WARNING) << "received block candidate with incorrect file hash from " << src;
    return;
  }
  VLOG(FULL_NODE_DEBUG) << "Received newBlockCandidate in custom overlay \"" << name_ << "\" from " << src << ": "
                        << block_id.to_str();
  td::actor::send_closure(full_node_, &FullNode::process_block_candidate_broadcast, block_id, cc_seqno,
                          validator_set_hash, std::move(data));
}

void FullNodeCustomOverlay::process_broadcast(PublicKeyHash src, ton_api::tonNode_newShardBlockBroadcast &query) {
  if (!block_senders_.count(adnl::AdnlNodeIdShort(src))) {
    VLOG(FULL_NODE_DEBUG) << "Dropping shard block description broadcast in private overlay \"" << name_
                          << "\" from unauthorized sender " << src;
    return;
  }
  BlockIdExt block_id = create_block_id(query.block_->block_);
  VLOG(FULL_NODE_DEBUG) << "Received newShardBlockBroadcast in custom overlay \"" << name_ << "\" from " << src << ": "
                        << block_id.to_str();
  td::actor::send_closure(full_node_, &FullNode::process_shard_block_info_broadcast, block_id, query.block_->cc_seqno_,
                          std::move(query.block_->data_));
}

void FullNodeCustomOverlay::receive_broadcast(PublicKeyHash src, td::BufferSlice broadcast) {
  if (adnl::AdnlNodeIdShort{src} == local_id_) {
    return;
  }
  auto B = fetch_tl_object<ton_api::tonNode_Broadcast>(std::move(broadcast), true);
  if (B.is_error()) {
    return;
  }
  ton_api::downcast_call(*B.move_as_ok(), [src, Self = this](auto &obj) { Self->process_broadcast(src, obj); });
}

void FullNodeCustomOverlay::process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_getArchiveInfo &query,
                                          td::Promise<td::BufferSlice> promise) {
  auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<td::uint64> R) mutable {
    if (R.is_error()) {
      promise.set_value(create_serialize_tl_object<ton_api::tonNode_archiveNotFound>());
    } else {
      promise.set_value(create_serialize_tl_object<ton_api::tonNode_archiveInfo>(R.move_as_ok()));
    }
  });
  VLOG(FULL_NODE_DEBUG) << "Got custom overlay getArchiveInfo " << query.masterchain_seqno_ << " from " << src;
  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::get_archive_id, query.masterchain_seqno_,
                          ShardIdFull{masterchainId}, std::move(P));
}

void FullNodeCustomOverlay::process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_getShardArchiveInfo &query,
                                          td::Promise<td::BufferSlice> promise) {
  ShardIdFull shard_prefix = create_shard_id(query.shard_prefix_);
  VLOG(FULL_NODE_DEBUG) << "Got custom overlay getShardArchiveInfo " << query.masterchain_seqno_ << " "
                        << shard_prefix.to_str() << " from " << src;
  if (!custom_overlay_serves_shard(sender_shards_, shard_prefix)) {
    promise.set_value(create_serialize_tl_object<ton_api::tonNode_archiveNotFound>());
    return;
  }
  auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<td::uint64> R) mutable {
    if (R.is_error()) {
      promise.set_value(create_serialize_tl_object<ton_api::tonNode_archiveNotFound>());
    } else {
      promise.set_value(create_serialize_tl_object<ton_api::tonNode_archiveInfo>(R.move_as_ok()));
    }
  });
  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::get_archive_id, query.masterchain_seqno_,
                          shard_prefix, std::move(P));
}

void FullNodeCustomOverlay::process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_getArchiveSlice &query,
                                          td::Promise<td::BufferSlice> promise) {
  VLOG(FULL_NODE_DEBUG) << "Got custom overlay getArchiveSlice " << query.archive_id_ << " " << query.offset_ << " "
                        << query.max_size_ << " from " << src;
  if (query.max_size_ < 0 || query.max_size_ > (1 << 24)) {
    promise.set_error(td::Status::Error(ErrorCode::protoviolation, "invalid max_size"));
    return;
  }
  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::get_archive_slice, query.archive_id_,
                          query.offset_, query.max_size_, std::move(promise));
}

void FullNodeCustomOverlay::process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_downloadBlockFull &query,
                                          td::Promise<td::BufferSlice> promise) {
  BlockIdExt block_id = create_block_id(query.block_);
  VLOG(FULL_NODE_DEBUG) << "Got custom overlay downloadBlockFull " << block_id.to_str() << " from " << src;
  if (!custom_overlay_serves_shard(sender_shards_, block_id.shard_full())) {
    promise.set_value(create_serialize_tl_object<ton_api::tonNode_dataFullEmpty>());
    return;
  }
  td::actor::create_actor<BlockFullSender>("customsender", block_id, false, validator_manager_, std::move(promise))
      .release();
}

void FullNodeCustomOverlay::process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_downloadNextBlockFull &query,
                                          td::Promise<td::BufferSlice> promise) {
  BlockIdExt block_id = create_block_id(query.prev_block_);
  VLOG(FULL_NODE_DEBUG) << "Got custom overlay downloadNextBlockFull " << block_id.to_str() << " from " << src;
  if (!custom_overlay_serves_shard(sender_shards_, block_id.shard_full())) {
    promise.set_value(create_serialize_tl_object<ton_api::tonNode_dataFullEmpty>());
    return;
  }
  td::actor::create_actor<BlockFullSender>("customsender", block_id, true, validator_manager_, std::move(promise))
      .release();
}

void FullNodeCustomOverlay::process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_prepareBlockProof &query,
                                          td::Promise<td::BufferSlice> promise) {
  if (query.block_->seqno_ == 0) {
    promise.set_error(td::Status::Error(ErrorCode::protoviolation, "cannot download proof for zero state"));
    return;
  }
  BlockIdExt block_id = create_block_id(query.block_);
  VLOG(FULL_NODE_DEBUG) << "Got custom overlay prepareBlockProof " << block_id.to_str() << " from " << src;
  if (!custom_overlay_serves_shard(sender_shards_, block_id.shard_full())) {
    promise.set_value(create_serialize_tl_object<ton_api::tonNode_preparedProofEmpty>());
    return;
  }
  auto P = td::PromiseCreator::lambda([allow_partial = query.allow_partial_,
                                       promise = std::move(promise)](td::Result<BlockHandle> R) mutable {
    if (R.is_error()) {
      promise.set_value(create_serialize_tl_object<ton_api::tonNode_preparedProofEmpty>());
      return;
    }
    auto handle = R.move_as_ok();
    if (!handle || (!handle->inited_proof() && (!allow_partial || !handle->inited_proof_link()))) {
      promise.set_value(create_serialize_tl_object<ton_api::tonNode_preparedProofEmpty>());
      return;
    }
    if (handle->inited_proof() && handle->id().is_masterchain()) {
      promise.set_value(create_serialize_tl_object<ton_api::tonNode_preparedProof>());
    } else {
      promise.set_value(create_serialize_tl_object<ton_api::tonNode_preparedProofLink>());
    }
  });
  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::get_block_handle, block_id, false,
                          std::move(P));
}

void FullNodeCustomOverlay::process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_downloadBlockProof &query,
                                          td::Promise<td::BufferSlice> promise) {
  BlockIdExt block_id = create_block_id(query.block_);
  VLOG(FULL_NODE_DEBUG) << "Got custom overlay downloadBlockProof " << block_id.to_str() << " from " << src;
  if (!custom_overlay_serves_shard(sender_shards_, block_id.shard_full())) {
    promise.set_error(td::Status::Error(ErrorCode::protoviolation, "unknown block proof"));
    return;
  }
  auto P = td::PromiseCreator::lambda(
      [promise = std::move(promise), validator_manager = validator_manager_](td::Result<BlockHandle> R) mutable {
        if (R.is_error()) {
          promise.set_error(td::Status::Error(ErrorCode::protoviolation, "unknown block proof"));
          return;
        }
        auto handle = R.move_as_ok();
        if (!handle || !handle->inited_proof()) {
          promise.set_error(td::Status::Error(ErrorCode::protoviolation, "unknown block proof"));
          return;
        }
        td::actor::send_closure(validator_manager, &ValidatorManagerInterface::get_block_proof, handle,
                                std::move(promise));
      });
  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::get_block_handle, block_id, false,
                          std::move(P));
}

void FullNodeCustomOverlay::process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_downloadBlockProofLink &query,
                                          td::Promise<td::BufferSlice> promise) {
  BlockIdExt block_id = create_block_id(query.block_);
  VLOG(FULL_NODE_DEBUG) << "Got custom overlay downloadBlockProofLink " << block_id.to_str() << " from " << src;
  if (!custom_overlay_serves_shard(sender_shards_, block_id.shard_full())) {
    promise.set_error(td::Status::Error(ErrorCode::protoviolation, "unknown block proof"));
    return;
  }
  auto P = td::PromiseCreator::lambda(
      [promise = std::move(promise), validator_manager = validator_manager_](td::Result<BlockHandle> R) mutable {
        if (R.is_error()) {
          promise.set_error(td::Status::Error(ErrorCode::protoviolation, "unknown block proof"));
          return;
        }
        auto handle = R.move_as_ok();
        if (!handle || !handle->inited_proof_link()) {
          promise.set_error(td::Status::Error(ErrorCode::protoviolation, "unknown block proof"));
          return;
        }
        td::actor::send_closure(validator_manager, &ValidatorManagerInterface::get_block_proof_link, handle,
                                std::move(promise));
      });
  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::get_block_handle, block_id, false,
                          std::move(P));
}

void FullNodeCustomOverlay::receive_query(adnl::AdnlNodeIdShort src, td::BufferSlice query,
                                          td::Promise<td::BufferSlice> promise) {
  if (std::find(nodes_.begin(), nodes_.end(), src) == nodes_.end()) {
    promise.set_error(td::Status::Error(ErrorCode::protoviolation, "custom overlay peer is not authorized"));
    return;
  }
  auto B = fetch_tl_object<ton_api::Function>(std::move(query), true);
  if (B.is_error()) {
    promise.set_error(td::Status::Error(ErrorCode::protoviolation, "cannot parse custom overlay tonnode query"));
    return;
  }
  auto fun_ptr = B.move_as_ok();
  if (limiter_ && !limiter_->check_in(fun_ptr->get_id(), request_cost_for_custom_overlay_query(*fun_ptr))) {
    promise.set_error(td::Status::Error(ErrorCode::failure, "too many custom overlay archive requests"));
    return;
  }
  ton_api::downcast_call(*fun_ptr.get(), [&](auto &obj) { this->process_query(src, obj, std::move(promise)); });
}

void FullNodeCustomOverlay::send_external_message(td::BufferSlice data) {
  if (!inited_ || opts_.config_.ext_messages_broadcast_disabled_) {
    return;
  }
  VLOG(FULL_NODE_DEBUG) << "Sending external message to custom overlay \"" << name_ << "\"";
  auto B = create_serialize_tl_object<ton_api::tonNode_externalMessageBroadcast>(
      create_tl_object<ton_api::tonNode_externalMessage>(std::move(data)));
  td::actor::send_closure(overlays_, &overlay::Overlays::send_broadcast_fec_ex, local_id_, overlay_id_,
                          local_id_.pubkey_hash(), 0, std::move(B));
}

void FullNodeCustomOverlay::send_broadcast(BlockBroadcast broadcast) {
  if (!inited_) {
    return;
  }
  VLOG(FULL_NODE_DEBUG) << "Sending block broadcast to custom overlay \"" << name_
                        << "\": " << broadcast.block_id.to_str();
  auto B = serialize_block_broadcast(broadcast, k_called_from_custom);
  if (B.is_error()) {
    VLOG(FULL_NODE_WARNING) << "failed to serialize block broadcast: " << B.move_as_error();
    return;
  }
  td::actor::send_closure(overlays_, &overlay::Overlays::send_broadcast_fec_ex, local_id_, overlay_id_,
                          local_id_.pubkey_hash(), overlay::Overlays::BroadcastFlagAnySender(), B.move_as_ok());
}

void FullNodeCustomOverlay::send_block_candidate(BlockIdExt block_id, CatchainSeqno cc_seqno,
                                                 td::uint32 validator_set_hash, td::BufferSlice data) {
  if (!inited_) {
    return;
  }
  auto B = serialize_block_candidate_broadcast(block_id, cc_seqno, validator_set_hash, data, true,
                                               k_called_from_custom);  // compression enabled
  if (B.is_error()) {
    VLOG(FULL_NODE_WARNING) << "failed to serialize block candidate broadcast: " << B.move_as_error();
    return;
  }
  VLOG(FULL_NODE_DEBUG) << "Sending newBlockCandidate in custom overlay \"" << name_ << "\": " << block_id.to_str();
  td::actor::send_closure(overlays_, &overlay::Overlays::send_broadcast_fec_ex, local_id_, overlay_id_,
                          local_id_.pubkey_hash(), overlay::Overlays::BroadcastFlagAnySender(), B.move_as_ok());
}

void FullNodeCustomOverlay::send_shard_block_info(BlockIdExt block_id, CatchainSeqno cc_seqno, td::BufferSlice data) {
  VLOG(FULL_NODE_DEBUG) << "Sending newShardBlockBroadcast in custom overlay \"" << name_
                        << "\": " << block_id.to_str();
  auto B = create_serialize_tl_object<ton_api::tonNode_newShardBlockBroadcast>(
      create_tl_object<ton_api::tonNode_newShardBlock>(create_tl_block_id(block_id), cc_seqno, std::move(data)));
  if (B.size() <= overlay::Overlays::max_simple_broadcast_size()) {
    td::actor::send_closure(overlays_, &overlay::Overlays::send_broadcast_ex, local_id_, overlay_id_,
                            local_id_.pubkey_hash(), 0, std::move(B));
  } else {
    td::actor::send_closure(overlays_, &overlay::Overlays::send_broadcast_fec_ex, local_id_, overlay_id_,
                            local_id_.pubkey_hash(), overlay::Overlays::BroadcastFlagAnySender(), std::move(B));
  }
}

std::vector<adnl::AdnlNodeIdShort> FullNodeCustomOverlay::custom_download_peers() const {
  std::vector<adnl::AdnlNodeIdShort> peers;
  auto add_peer = [&](adnl::AdnlNodeIdShort peer) {
    if (peer.is_zero() || peer == local_id_) {
      return;
    }
    if (std::find(peers.begin(), peers.end(), peer) == peers.end()) {
      peers.push_back(peer);
    }
  };
  for (auto peer : block_senders_) {
    add_peer(peer);
  }
  for (auto peer : nodes_) {
    add_peer(peer);
  }
  return peers;
}

td::Timestamp custom_overlay_peer_download_timeout(td::Timestamp timeout) {
  auto peer_timeout = td::Timestamp::in(2.0);
  return timeout < peer_timeout ? timeout : peer_timeout;
}

void FullNodeCustomOverlay::download_block_from_custom_peers(BlockIdExt id, td::uint32 priority,
                                                             td::Timestamp timeout,
                                                             std::vector<adnl::AdnlNodeIdShort> peers,
                                                             double started_at,
                                                             td::Promise<ReceivedBlock> promise) {
  auto sender = custom_overlay_sync_sender(use_quic_);
  auto target = custom_overlay_sync_log_target(id);
  auto local = custom_overlay_sync_adnl_to_string(local_id_);
  if (peers.empty()) {
    record_custom_overlay_sync_download(CustomOverlaySyncKind::Block, sender, CustomOverlaySyncResult::NoPeer,
                                        started_at, block_propagation_trace_now());
    log_custom_overlay_sync_stage(CustomOverlaySyncKind::Block, sender, name_, local, "-", target, "custom.done",
                                  "no_peer", {}, 0, started_at);
    promise.set_error(td::Status::Error(ErrorCode::notready, "no custom overlay peer has this block"));
    return;
  }
  if (timeout.is_in_past()) {
    record_custom_overlay_sync_download(CustomOverlaySyncKind::Block, sender, CustomOverlaySyncResult::Timeout,
                                        started_at, block_propagation_trace_now());
    log_custom_overlay_sync_stage(CustomOverlaySyncKind::Block, sender, name_, local, "-", target, "custom.done",
                                  "timeout", {}, peers.size(), started_at);
    promise.set_error(td::Status::Error(ErrorCode::timeout, "custom overlay block download timeout"));
    return;
  }
  auto peer_timeout = custom_overlay_peer_download_timeout(timeout);
  log_custom_overlay_sync_stage(CustomOverlaySyncKind::Block, sender, name_, local, "-", target, "custom.race_start",
                                "attempt", {}, peers.size(), started_at);
  auto state = std::make_shared<CustomOverlaySyncDownloadState>(CustomOverlaySyncKind::Block, sender, started_at,
                                                                peers.size(), name_, local, target,
                                                                std::move(promise));
  for (auto peer : peers) {
    auto peer_started_at = block_propagation_trace_now();
    auto peer_string = custom_overlay_sync_adnl_to_string(peer);
    record_custom_overlay_sync_peer_download(CustomOverlaySyncKind::Block, sender, CustomOverlaySyncResult::Attempt);
    log_custom_overlay_sync_stage(CustomOverlaySyncKind::Block, sender, name_, local, peer_string, target,
                                  "peer.attempt", "attempt", {}, peers.size(), peer_started_at);
    auto P = td::PromiseCreator::lambda([state, peer, id, peer_string, peer_started_at](td::Result<ReceivedBlock> R) mutable {
      if (R.is_error()) {
        VLOG(FULL_NODE_DEBUG) << "failed to download block " << id.to_str() << " from custom overlay peer " << peer
                              << ": " << R.error();
        auto reason = R.error().to_string();
        auto result = custom_overlay_sync_result_from_status(R.error());
        log_custom_overlay_sync_stage(state->kind, state->sender, state->overlay_name, state->local_id, peer_string,
                                      state->target, "peer.done",
                                      custom_overlay_sync_result_label(metric_index(result)), reason,
                                      state->peers_total, peer_started_at);
      } else {
        log_custom_overlay_sync_stage(state->kind, state->sender, state->overlay_name, state->local_id, peer_string,
                                      state->target, "peer.done", "ok", {}, state->peers_total, peer_started_at);
      }
      finish_custom_overlay_sync_download(std::move(state), std::move(R), peer_started_at);
    });
    td::actor::create_actor<DownloadBlockNew>(
        "customdownloadreq", id, local_id_, overlay_id_, peer, priority, peer_timeout, validator_manager_,
        td::actor::ActorId<adnl::AdnlSenderInterface>{adnl_sender_}, overlays_, adnl_,
        td::actor::ActorId<adnl::AdnlExtClient>{}, std::move(P))
        .release();
  }
}

void FullNodeCustomOverlay::download_next_block_from_custom_peers(BlockIdExt prev_id, td::uint32 priority,
                                                                  td::Timestamp timeout,
                                                                  std::vector<adnl::AdnlNodeIdShort> peers,
                                                                  double started_at,
                                                                  td::Promise<ReceivedBlock> promise) {
  auto sender = custom_overlay_sync_sender(use_quic_);
  auto target = custom_overlay_sync_log_target(prev_id);
  auto local = custom_overlay_sync_adnl_to_string(local_id_);
  if (peers.empty()) {
    record_custom_overlay_sync_download(CustomOverlaySyncKind::NextBlock, sender, CustomOverlaySyncResult::NoPeer,
                                        started_at, block_propagation_trace_now());
    log_custom_overlay_sync_stage(CustomOverlaySyncKind::NextBlock, sender, name_, local, "-", target, "custom.done",
                                  "no_peer", {}, 0, started_at);
    promise.set_error(td::Status::Error(ErrorCode::notready, "no custom overlay peer has next block"));
    return;
  }
  if (timeout.is_in_past()) {
    record_custom_overlay_sync_download(CustomOverlaySyncKind::NextBlock, sender, CustomOverlaySyncResult::Timeout,
                                        started_at, block_propagation_trace_now());
    log_custom_overlay_sync_stage(CustomOverlaySyncKind::NextBlock, sender, name_, local, "-", target, "custom.done",
                                  "timeout", {}, peers.size(), started_at);
    promise.set_error(td::Status::Error(ErrorCode::timeout, "custom overlay next block download timeout"));
    return;
  }
  auto peer_timeout = custom_overlay_peer_download_timeout(timeout);
  log_custom_overlay_sync_stage(CustomOverlaySyncKind::NextBlock, sender, name_, local, "-", target,
                                "custom.race_start", "attempt", {}, peers.size(), started_at);
  auto state = std::make_shared<CustomOverlaySyncDownloadState>(CustomOverlaySyncKind::NextBlock, sender, started_at,
                                                                peers.size(), name_, local, target,
                                                                std::move(promise));
  for (auto peer : peers) {
    auto peer_started_at = block_propagation_trace_now();
    auto peer_string = custom_overlay_sync_adnl_to_string(peer);
    record_custom_overlay_sync_peer_download(CustomOverlaySyncKind::NextBlock, sender, CustomOverlaySyncResult::Attempt);
    log_custom_overlay_sync_stage(CustomOverlaySyncKind::NextBlock, sender, name_, local, peer_string, target,
                                  "peer.attempt", "attempt", {}, peers.size(), peer_started_at);
    auto P = td::PromiseCreator::lambda([state, peer, prev_id, peer_string, peer_started_at](td::Result<ReceivedBlock> R) mutable {
      if (R.is_error()) {
        VLOG(FULL_NODE_DEBUG) << "failed to download next block after " << prev_id.to_str()
                              << " from custom overlay peer " << peer << ": " << R.error();
        auto reason = R.error().to_string();
        auto result = custom_overlay_sync_result_from_status(R.error());
        log_custom_overlay_sync_stage(state->kind, state->sender, state->overlay_name, state->local_id, peer_string,
                                      state->target, "peer.done",
                                      custom_overlay_sync_result_label(metric_index(result)), reason,
                                      state->peers_total, peer_started_at);
      } else {
        log_custom_overlay_sync_stage(state->kind, state->sender, state->overlay_name, state->local_id, peer_string,
                                      state->target, "peer.done", "ok", {}, state->peers_total, peer_started_at);
      }
      finish_custom_overlay_sync_download(std::move(state), std::move(R), peer_started_at);
    });
    td::actor::create_actor<DownloadBlockNew>(
        "customdownloadnext", local_id_, overlay_id_, prev_id, peer, priority, peer_timeout, validator_manager_,
        td::actor::ActorId<adnl::AdnlSenderInterface>{adnl_sender_}, overlays_, adnl_,
        td::actor::ActorId<adnl::AdnlExtClient>{}, std::move(P))
        .release();
  }
}

void FullNodeCustomOverlay::download_block(BlockIdExt id, td::uint32 priority, td::Timestamp timeout,
                                           td::Promise<ReceivedBlock> promise) {
  auto started_at = block_propagation_trace_now();
  auto sender = custom_overlay_sync_sender(use_quic_);
  auto target = custom_overlay_sync_log_target(id);
  auto local = custom_overlay_sync_adnl_to_string(local_id_);
  record_custom_overlay_sync_download(CustomOverlaySyncKind::Block, sender, CustomOverlaySyncResult::Attempt);
  log_custom_overlay_sync_stage(CustomOverlaySyncKind::Block, sender, name_, local, "-", target, "custom.start",
                                "attempt", {}, 0, started_at);
  if (!inited_) {
    record_custom_overlay_sync_download(CustomOverlaySyncKind::Block, sender, CustomOverlaySyncResult::NotReady,
                                        started_at, block_propagation_trace_now());
    log_custom_overlay_sync_stage(CustomOverlaySyncKind::Block, sender, name_, local, "-", target, "custom.done",
                                  "not_ready", {}, 0, started_at);
    promise.set_error(td::Status::Error(ErrorCode::notready, "custom overlay is not ready"));
    return;
  }
  if (!custom_overlay_serves_shard(sender_shards_, id.shard_full())) {
    record_custom_overlay_sync_download(CustomOverlaySyncKind::Block, sender, CustomOverlaySyncResult::ShardNotServed,
                                        started_at, block_propagation_trace_now());
    log_custom_overlay_sync_stage(CustomOverlaySyncKind::Block, sender, name_, local, "-", target, "custom.done",
                                  "shard_not_served", {}, 0, started_at);
    promise.set_error(td::Status::Error(ErrorCode::notready, "custom overlay does not serve shard"));
    return;
  }
  auto peers = custom_download_peers();
  VLOG(FULL_NODE_DEBUG) << "Trying custom overlay \"" << name_ << "\" block " << id.to_str() << " from "
                        << peers.size() << " peers";
  download_block_from_custom_peers(id, priority, timeout, std::move(peers), started_at, std::move(promise));
}

void FullNodeCustomOverlay::download_next_block(BlockIdExt prev_id, td::uint32 priority, td::Timestamp timeout,
                                                td::Promise<ReceivedBlock> promise) {
  auto started_at = block_propagation_trace_now();
  auto sender = custom_overlay_sync_sender(use_quic_);
  auto target = custom_overlay_sync_log_target(prev_id);
  auto local = custom_overlay_sync_adnl_to_string(local_id_);
  record_custom_overlay_sync_download(CustomOverlaySyncKind::NextBlock, sender, CustomOverlaySyncResult::Attempt);
  log_custom_overlay_sync_stage(CustomOverlaySyncKind::NextBlock, sender, name_, local, "-", target, "custom.start",
                                "attempt", {}, 0, started_at);
  if (!inited_) {
    record_custom_overlay_sync_download(CustomOverlaySyncKind::NextBlock, sender, CustomOverlaySyncResult::NotReady,
                                        started_at, block_propagation_trace_now());
    log_custom_overlay_sync_stage(CustomOverlaySyncKind::NextBlock, sender, name_, local, "-", target, "custom.done",
                                  "not_ready", {}, 0, started_at);
    promise.set_error(td::Status::Error(ErrorCode::notready, "custom overlay is not ready"));
    return;
  }
  if (!custom_overlay_serves_shard(sender_shards_, prev_id.shard_full())) {
    record_custom_overlay_sync_download(CustomOverlaySyncKind::NextBlock, sender,
                                        CustomOverlaySyncResult::ShardNotServed, started_at,
                                        block_propagation_trace_now());
    log_custom_overlay_sync_stage(CustomOverlaySyncKind::NextBlock, sender, name_, local, "-", target, "custom.done",
                                  "shard_not_served", {}, 0, started_at);
    promise.set_error(td::Status::Error(ErrorCode::notready, "custom overlay does not serve shard"));
    return;
  }
  auto peers = custom_download_peers();
  VLOG(FULL_NODE_DEBUG) << "Trying custom overlay \"" << name_ << "\" next block after " << prev_id.to_str()
                        << " from " << peers.size() << " peers";
  download_next_block_from_custom_peers(prev_id, priority, timeout, std::move(peers), started_at, std::move(promise));
}

void FullNodeCustomOverlay::download_block_proof(BlockIdExt block_id, td::uint32 priority, td::Timestamp timeout,
                                                 td::Promise<td::BufferSlice> promise) {
  if (!inited_) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "custom overlay is not ready"));
    return;
  }
  if (!custom_overlay_serves_shard(sender_shards_, block_id.shard_full())) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "custom overlay does not serve shard"));
    return;
  }
  VLOG(FULL_NODE_DEBUG) << "Trying custom overlay \"" << name_ << "\" block proof " << block_id.to_str();
  td::actor::create_actor<DownloadProof>(
      "customdownloadproof", block_id, false, false, local_id_, overlay_id_, adnl::AdnlNodeIdShort::zero(), priority,
      timeout, validator_manager_, td::actor::ActorId<adnl::AdnlSenderInterface>{adnl_sender_}, overlays_, adnl_,
      td::actor::ActorId<adnl::AdnlExtClient>{}, std::move(promise))
      .release();
}

void FullNodeCustomOverlay::download_block_proof_link(BlockIdExt block_id, td::uint32 priority, td::Timestamp timeout,
                                                      td::Promise<td::BufferSlice> promise) {
  if (!inited_) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "custom overlay is not ready"));
    return;
  }
  if (!custom_overlay_serves_shard(sender_shards_, block_id.shard_full())) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "custom overlay does not serve shard"));
    return;
  }
  VLOG(FULL_NODE_DEBUG) << "Trying custom overlay \"" << name_ << "\" block proof link " << block_id.to_str();
  td::actor::create_actor<DownloadProof>(
      "customdownloadproof", block_id, true, false, local_id_, overlay_id_, adnl::AdnlNodeIdShort::zero(), priority,
      timeout, validator_manager_, td::actor::ActorId<adnl::AdnlSenderInterface>{adnl_sender_}, overlays_, adnl_,
      td::actor::ActorId<adnl::AdnlExtClient>{}, std::move(promise))
      .release();
}

void FullNodeCustomOverlay::download_archive(BlockSeqno masterchain_seqno, ShardIdFull shard_prefix,
                                             std::string tmp_dir, td::Timestamp timeout,
                                             td::Promise<std::string> promise) {
  if (!inited_) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "custom overlay is not ready"));
    return;
  }
  if (!custom_overlay_serves_shard(sender_shards_, shard_prefix)) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "custom overlay does not serve shard"));
    return;
  }
  auto peers = custom_download_peers();
  if (peers.empty()) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "custom overlay has no remote peers"));
    return;
  }
  LOG(INFO) << "Trying custom overlay \"" << name_ << "\" archive slice #" << masterchain_seqno << " "
            << shard_prefix.to_str() << " from " << peers.size() << " peers";
  td::actor::create_actor<DownloadArchiveSlice>(
      PSTRING() << "customarchive." << masterchain_seqno, masterchain_seqno, shard_prefix, std::move(tmp_dir),
      local_id_, overlay_id_, adnl::AdnlNodeIdShort::zero(), timeout, validator_manager_,
      td::actor::ActorId<adnl::AdnlSenderInterface>{adnl_sender_}, overlays_, adnl_,
      td::actor::ActorId<adnl::AdnlExtClient>{}, std::move(promise), std::move(peers),
      true /* use_sender_for_prepare_query */, true /* use_sender_for_slice_query */,
      false /* resolve_peers_before_download */, true /* record_archive_sync_metrics */,
      custom_overlay_sync_sender(use_quic_))
      .release();
}

void FullNodeCustomOverlay::start_up() {
  std::sort(nodes_.begin(), nodes_.end());
  nodes_.erase(std::unique(nodes_.begin(), nodes_.end()), nodes_.end());
  std::vector<td::Bits256> nodes;
  for (const adnl::AdnlNodeIdShort &id : nodes_) {
    nodes.push_back(id.bits256_value());
  }
  auto X = create_hash_tl_object<ton_api::tonNode_customOverlayId>(zero_state_file_hash_, name_, std::move(nodes));
  td::BufferSlice b{32};
  b.as_slice().copy_from(as_slice(X));
  overlay_id_full_ = overlay::OverlayIdFull{std::move(b)};
  overlay_id_ = overlay_id_full_.compute_short_id();
  try_init();
}

void FullNodeCustomOverlay::try_init() {
  // Sometimes adnl id is added to validator engine later (or not at all)
  td::actor::send_closure(
      adnl_, &adnl::Adnl::check_id_exists, local_id_, [SelfId = actor_id(this)](td::Result<bool> R) {
        if (R.is_ok() && R.ok()) {
          td::actor::send_closure(SelfId, &FullNodeCustomOverlay::init);
        } else {
          delay_action([SelfId]() { td::actor::send_closure(SelfId, &FullNodeCustomOverlay::try_init); },
                       td::Timestamp::in(30.0));
        }
      });
}

void FullNodeCustomOverlay::init() {
  td::actor::send_closure(adnl_sender_, &adnl::AdnlSenderEx::add_id, local_id_);

  LOG(FULL_NODE_WARNING) << "Creating custom overlay \"" << name_ << "\" for adnl id " << local_id_ << " : "
                         << nodes_.size() << " nodes, " << msg_senders_.size() << " msg senders, "
                         << block_senders_.size() << " block senders, overlay_id=" << overlay_id_;
  class Callback : public overlay::Overlays::Callback {
   public:
    void receive_message(adnl::AdnlNodeIdShort src, overlay::OverlayIdShort overlay_id, td::BufferSlice data) override {
    }
    void receive_query(adnl::AdnlNodeIdShort src, overlay::OverlayIdShort overlay_id, td::BufferSlice data,
                       td::Promise<td::BufferSlice> promise) override {
      td::actor::send_closure(node_, &FullNodeCustomOverlay::receive_query, src, std::move(data), std::move(promise));
    }
    void receive_broadcast(PublicKeyHash src, overlay::OverlayIdShort overlay_id, td::BufferSlice data) override {
      td::actor::send_closure(node_, &FullNodeCustomOverlay::receive_broadcast, src, std::move(data));
    }
    void check_broadcast(PublicKeyHash src, overlay::OverlayIdShort overlay_id, td::BufferSlice data,
                         td::Promise<td::Unit> promise) override {
    }
    Callback(td::actor::ActorId<FullNodeCustomOverlay> node) : node_(node) {
    }

   private:
    td::actor::ActorId<FullNodeCustomOverlay> node_;
  };

  std::map<PublicKeyHash, td::uint32> authorized_keys;
  for (const auto &sender : msg_senders_) {
    authorized_keys[sender.first.pubkey_hash()] = overlay::Overlays::max_fec_broadcast_size();
  }
  for (const auto &sender : block_senders_) {
    authorized_keys[sender.pubkey_hash()] = overlay::Overlays::max_fec_broadcast_size();
  }
  overlay::OverlayPrivacyRules rules{overlay::Overlays::max_fec_broadcast_size(), 0, std::move(authorized_keys)};
  overlay::OverlayOptions overlay_options;
  overlay_options.name_ = "custom." + name_;
  overlay_options.broadcast_speed_multiplier_ = opts_.private_broadcast_speed_multiplier_;
  overlay_options.send_twostep_broadcast_ = true;
  overlay_options.twostep_broadcast_sender_ = adnl_sender_;
  td::actor::send_closure(
      overlays_, &overlay::Overlays::create_private_overlay_ex, local_id_, overlay_id_full_.clone(), nodes_,
      std::make_unique<Callback>(actor_id(this)), rules,
      PSTRING() << R"({ "type": "custom-overlay", "name": ")" << td::format::Escaped{name_} << R"(" })",
      overlay_options);

  inited_ = true;
  prewarm_archive_peers();
  alarm_timestamp().relax(td::Timestamp::in(60.0));
}

void FullNodeCustomOverlay::prewarm_archive_peers() {
  auto peers = custom_download_peers();
  if (peers.empty()) {
    return;
  }
  LOG(INFO) << "Prewarming " << peers.size() << " custom overlay \"" << name_ << "\" archive peers via DHT";
  for (const auto &peer : peers) {
    td::actor::send_closure(
        adnl_, &adnl::Adnl::get_peer_node, local_id_, peer,
        [name = name_, local_id = local_id_, peer](td::Result<adnl::AdnlNode> R) mutable {
          if (R.is_ok()) {
            auto node = R.move_as_ok();
            LOG(INFO) << "Prewarmed custom overlay \"" << name << "\" peer " << peer << " for local id "
                      << local_id << " addr_count=" << node.addr_list().size();
          } else {
            LOG(INFO) << "Failed to prewarm custom overlay \"" << name << "\" peer " << peer << " for local id "
                      << local_id << ": " << R.move_as_error();
          }
        });
  }
}

void FullNodeCustomOverlay::alarm() {
  if (inited_) {
    prewarm_archive_peers();
    alarm_timestamp() = td::Timestamp::in(60.0);
  }
}

void FullNodeCustomOverlay::tear_down() {
  LOG(FULL_NODE_WARNING) << "Destroying custom overlay \"" << name_ << "\" for adnl id " << local_id_;
  td::actor::send_closure(overlays_, &ton::overlay::Overlays::delete_overlay, local_id_, overlay_id_);
}

}  // namespace ton::validator::fullnode
