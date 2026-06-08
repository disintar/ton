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
#include <algorithm>
#include <ton/ton-tl.hpp>

#include "common/delay.h"
#include "td/utils/overloaded.h"
#include "td/utils/port/path.h"
#include "ton/ton-io.hpp"

#include "validator/block-propagation-trace.h"
#include "download-archive-slice.hpp"

namespace ton {

namespace validator {

namespace fullnode {

namespace {

constexpr double kArchivePeerResolveTimeout = 2.0;
constexpr double kArchiveInfoTimeout = 3.0;
constexpr double kPublicArchiveInfoTimeout = 1.25;
constexpr double kArchiveSliceChunkTimeout = 15.0;
constexpr td::uint32 kPublicArchivePeerCount = 5;

CustomOverlaySyncResult archive_sync_result_from_status(const td::Status &status) {
  return status.code() == ErrorCode::timeout ? CustomOverlaySyncResult::Timeout : CustomOverlaySyncResult::Error;
}

const char *archive_sender_label(CustomOverlaySyncSender sender) {
  return custom_overlay_sync_sender_label(metric_index(sender));
}

std::string archive_status_reason(td::Status status) {
  auto reason = status.to_string();
  std::replace(reason.begin(), reason.end(), ' ', '_');
  return reason;
}

long long archive_elapsed_ms(double started_at) {
  return block_propagation_trace_ms(started_at, block_propagation_trace_now());
}

}  // namespace

DownloadArchiveSlice::DownloadArchiveSlice(
    BlockSeqno masterchain_seqno, ShardIdFull shard_prefix, std::string tmp_dir, adnl::AdnlNodeIdShort local_id,
    overlay::OverlayIdShort overlay_id, adnl::AdnlNodeIdShort download_from, td::Timestamp timeout,
    td::actor::ActorId<ValidatorManagerInterface> validator_manager, td::actor::ActorId<adnl::AdnlSenderInterface> rldp,
    td::actor::ActorId<overlay::Overlays> overlays, td::actor::ActorId<adnl::Adnl> adnl,
    td::actor::ActorId<adnl::AdnlExtClient> client, td::Promise<std::string> promise,
    std::vector<adnl::AdnlNodeIdShort> download_from_list, bool use_sender_for_prepare_query,
    bool use_sender_for_slice_query, bool resolve_peers_before_download, bool record_archive_sync_metrics,
    CustomOverlaySyncSender archive_sync_sender)
    : masterchain_seqno_(masterchain_seqno)
    , shard_prefix_(shard_prefix)
    , tmp_dir_(std::move(tmp_dir))
    , local_id_(local_id)
    , overlay_id_(overlay_id)
    , download_from_(download_from)
    , timeout_(timeout)
    , validator_manager_(validator_manager)
    , rldp_(rldp)
    , overlays_(overlays)
    , adnl_(adnl)
    , client_(client)
    , promise_(std::move(promise))
    , use_sender_for_prepare_query_(use_sender_for_prepare_query)
    , use_sender_for_slice_query_(use_sender_for_slice_query)
    , resolve_peers_before_download_(resolve_peers_before_download)
    , record_archive_sync_metrics_(record_archive_sync_metrics)
    , archive_sync_sender_(archive_sync_sender) {
  if (!download_from.is_zero() || !download_from_list.empty()) {
    original_zero_download_ = false;
  }
  download_from_list_ = std::move(download_from_list);
}

const char *DownloadArchiveSlice::archive_source() const {
  if (!client_.empty()) {
    return "client";
  }
  return record_archive_sync_metrics_ ? "custom" : "public";
}

const char *DownloadArchiveSlice::archive_prepare_transport() const {
  if (!client_.empty()) {
    return "client";
  }
  return use_sender_for_prepare_query_ ? archive_sender_label(archive_sync_sender_) : "overlay";
}

const char *DownloadArchiveSlice::archive_slice_transport() const {
  if (!client_.empty()) {
    return "client";
  }
  return use_sender_for_slice_query_ ? archive_sender_label(archive_sync_sender_) : "overlay";
}

double DownloadArchiveSlice::archive_info_timeout_seconds() const {
  if (client_.empty() && !record_archive_sync_metrics_) {
    return kPublicArchiveInfoTimeout;
  }
  return kArchiveInfoTimeout;
}

void DownloadArchiveSlice::abort_query(td::Status reason) {
  if (promise_) {
    auto reason_text = archive_status_reason(reason.clone());
    if (record_archive_sync_metrics_ && !archive_sync_metric_finished_) {
      record_custom_overlay_sync_download(CustomOverlaySyncKind::Archive, archive_sync_sender_,
                                          archive_sync_result_from_status(reason), archive_sync_started_at_,
                                          block_propagation_trace_now());
      archive_sync_metric_finished_ = true;
    }
    LOG(WARNING) << "[archive-sync] stage=archive.done source=" << archive_source()
                 << " transport=" << archive_slice_transport() << " seqno=" << masterchain_seqno_
                 << " shard=" << shard_prefix_.to_str() << " peer=" << download_from_
                 << " offset=" << offset_ << " ms=" << archive_elapsed_ms(archive_sync_started_at_)
                 << " result=error reason=" << reason_text;
    promise_.set_error(std::move(reason));
    if (!fd_.empty()) {
      td::unlink(tmp_name_).ensure();
      fd_.close();
    }
  }
  stop();
}

void DownloadArchiveSlice::alarm() {
  abort_query(td::Status::Error(ErrorCode::timeout, "timeout"));
}

void DownloadArchiveSlice::finish_query() {
  if (promise_) {
    if (record_archive_sync_metrics_ && !archive_sync_metric_finished_) {
      record_custom_overlay_sync_download(CustomOverlaySyncKind::Archive, archive_sync_sender_,
                                          CustomOverlaySyncResult::Ok, archive_sync_started_at_,
                                          block_propagation_trace_now());
      archive_sync_metric_finished_ = true;
    }
    LOG(WARNING) << "[archive-sync] stage=archive.done source=" << archive_source()
                 << " transport=" << archive_slice_transport() << " seqno=" << masterchain_seqno_
                 << " shard=" << shard_prefix_.to_str() << " peer=" << download_from_
                 << " offset=" << offset_ << " ms=" << archive_elapsed_ms(archive_sync_started_at_)
                 << " result=ok reason=downloaded";
    promise_.set_value(std::move(tmp_name_));
    fd_.close();
  }
  stop();
}

void DownloadArchiveSlice::start_up() {
  alarm_timestamp() = timeout_;
  archive_sync_started_at_ = block_propagation_trace_now();
  if (record_archive_sync_metrics_) {
    record_custom_overlay_sync_download(CustomOverlaySyncKind::Archive, archive_sync_sender_,
                                        CustomOverlaySyncResult::Attempt);
  }
  LOG(WARNING) << "[archive-sync] stage=archive.start source=" << archive_source()
               << " transport=" << archive_slice_transport() << " seqno=" << masterchain_seqno_
               << " shard=" << shard_prefix_.to_str() << " peer=" << download_from_
               << " listed_peers=" << download_from_list_.size() << " result=start";

  auto R = td::mkstemp(tmp_dir_);
  if (R.is_error()) {
    abort_query(R.move_as_error_prefix("failed to open temp file: "));
    return;
  }
  auto r = R.move_as_ok();
  fd_ = std::move(r.first);
  tmp_name_ = std::move(r.second);

  if (!download_from_list_.empty()) {
    got_node_to_download(std::move(download_from_list_));
  } else if (download_from_.is_zero() && client_.empty()) {
    request_random_public_peers("startup_no_peer");
  } else {
    std::vector<adnl::AdnlNodeIdShort> tmp;
    tmp.emplace_back(download_from_);
    got_node_to_download(std::move(tmp));
  }
}

void DownloadArchiveSlice::request_random_public_peers(const char *reason) {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<std::vector<adnl::AdnlNodeIdShort>> R) {
    if (R.is_error()) {
      auto error = R.move_as_error();
      LOG(WARNING) << "[archive-sync] stage=random_peers.done source=public transport=overlay"
                   << " result=error reason=" << archive_status_reason(error.clone());
      td::actor::send_closure(SelfId, &DownloadArchiveSlice::abort_query, std::move(error));
    } else {
      auto vec = R.move_as_ok();
      LOG(WARNING) << "[archive-sync] stage=random_peers.done source=public transport=overlay"
                   << " peers=" << vec.size() << " result=" << (vec.empty() ? "empty" : "ok");
      if (vec.size() == 0) {
        td::actor::send_closure(SelfId, &DownloadArchiveSlice::abort_query,
                                td::Status::Error(ErrorCode::notready, "no nodes"));
      } else {
        td::actor::send_closure(SelfId, &DownloadArchiveSlice::got_node_to_download, vec);
      }
    }
  });

  LOG(WARNING) << "[archive-sync] stage=random_peers.start source=public transport=overlay"
               << " seqno=" << masterchain_seqno_ << " shard=" << shard_prefix_.to_str()
               << " requested=" << kPublicArchivePeerCount << " result=start reason=" << reason;
  td::actor::send_closure(overlays_, &overlay::Overlays::get_overlay_random_peers, local_id_, overlay_id_,
                          kPublicArchivePeerCount, std::move(P));
}

bool DownloadArchiveSlice::try_random_public_peers_fallback(td::Status reason) {
  if (!client_.empty() || record_archive_sync_metrics_ || random_public_peers_requested_ || timeout_.is_in_past()) {
    return false;
  }
  random_public_peers_requested_ = true;
  download_from_ = adnl::AdnlNodeIdShort::zero();
  download_from_list_.clear();
  resolved_download_from_list_.clear();
  current_peer_index_ = 0;
  current_peer_count_ = 0;
  archive_info_parallel_ = false;
  archive_info_pending_ = 0;
  LOG(WARNING) << "[archive-sync] stage=random_peers.fallback source=public transport=overlay"
               << " seqno=" << masterchain_seqno_ << " shard=" << shard_prefix_.to_str()
               << " offset=" << offset_ << " result=start reason=" << archive_status_reason(std::move(reason));
  request_random_public_peers("explicit_peers_failed");
  return true;
}

void DownloadArchiveSlice::got_node_to_download(std::vector<adnl::AdnlNodeIdShort> download_from) {
  if (download_from.empty()) {
    abort_query(td::Status::Error(ErrorCode::notready, "no nodes"));
    return;
  }
  download_from_list_ = std::move(download_from);
  LOG(WARNING) << "[archive-sync] stage=peers source=" << archive_source()
               << " transport=" << archive_prepare_transport() << " seqno=" << masterchain_seqno_
               << " shard=" << shard_prefix_.to_str() << " peers=" << download_from_list_.size()
               << " first_peer=" << download_from_list_.front() << " result=ok";
  if (resolve_peers_before_download_ && client_.empty()) {
    resolve_download_peers();
    return;
  }
  try_download(0);
}

void DownloadArchiveSlice::resolve_download_peers() {
  resolved_download_from_list_.clear();
  resolving_peers_ = download_from_list_.size();
  if (resolving_peers_ == 0) {
    abort_query(td::Status::Error(ErrorCode::notready, "no nodes"));
    return;
  }

  auto query_id = ++resolve_query_id_;
  if (record_archive_sync_metrics_) {
    LOG(INFO) << "[archive-sync] stage=resolve.start source=" << archive_source()
              << " transport=" << archive_prepare_transport() << " seqno=" << masterchain_seqno_
              << " shard=" << shard_prefix_.to_str()
              << " peers=" << resolving_peers_ << " result=start";
  }
  delay_action(
      [SelfId = actor_id(this), query_id]() {
        td::actor::send_closure(SelfId, &DownloadArchiveSlice::resolve_download_peers_timeout, query_id);
      },
      td::Timestamp::in(kArchivePeerResolveTimeout));
  for (const auto &peer : download_from_list_) {
    td::actor::send_closure(
        adnl_, &adnl::Adnl::get_peer_node, local_id_, peer,
        [SelfId = actor_id(this), query_id, peer](td::Result<adnl::AdnlNode> R) mutable {
          td::actor::send_closure(SelfId, &DownloadArchiveSlice::got_resolved_download_peer, query_id, peer,
                                  std::move(R));
        });
  }
}
void DownloadArchiveSlice::got_resolved_download_peer(td::uint64 query_id, adnl::AdnlNodeIdShort peer,
                                                      td::Result<adnl::AdnlNode> result) {
  if (query_id != resolve_query_id_ || resolving_peers_ == 0) {
    return;
  }
  if (result.is_ok()) {
    auto node = result.move_as_ok();
    if (record_archive_sync_metrics_) {
      LOG(INFO) << "[archive-sync] stage=resolve.peer source=" << archive_source()
                << " transport=" << archive_prepare_transport() << " seqno=" << masterchain_seqno_
                << " shard=" << shard_prefix_.to_str()
                << " peer=" << peer << " addr_count=" << node.addr_list().size() << " result=ok";
    }
    resolved_download_from_list_.push_back(peer);
  } else {
    auto error = result.move_as_error();
    if (record_archive_sync_metrics_) {
      LOG(INFO) << "[archive-sync] stage=resolve.peer source=" << archive_source()
                << " transport=" << archive_prepare_transport() << " seqno=" << masterchain_seqno_
                << " shard=" << shard_prefix_.to_str()
                << " peer=" << peer << " result=error reason=" << archive_status_reason(std::move(error));
    }
  }

  CHECK(resolving_peers_ > 0);
  --resolving_peers_;
  if (resolving_peers_ != 0) {
    return;
  }

  if (resolved_download_from_list_.empty()) {
    abort_query(td::Status::Error(ErrorCode::notready, "no resolved custom archive peers"));
    return;
  }

  download_from_list_ = std::move(resolved_download_from_list_);
  if (record_archive_sync_metrics_) {
    LOG(INFO) << "[archive-sync] stage=resolve.done source=" << archive_source()
              << " transport=" << archive_prepare_transport() << " seqno=" << masterchain_seqno_
              << " shard=" << shard_prefix_.to_str()
              << " peers=" << download_from_list_.size() << " result=ok";
  }
  try_download(0);
}

void DownloadArchiveSlice::resolve_download_peers_timeout(td::uint64 query_id) {
  if (query_id != resolve_query_id_ || resolving_peers_ == 0) {
    return;
  }
  if (record_archive_sync_metrics_) {
    LOG(INFO) << "[archive-sync] stage=resolve.done source=" << archive_source()
              << " transport=" << archive_prepare_transport() << " seqno=" << masterchain_seqno_
              << " shard=" << shard_prefix_.to_str()
              << " peers=" << resolved_download_from_list_.size() << " pending=" << resolving_peers_
              << " result=timeout reason=dht_resolve_timeout";
  }
  ++resolve_query_id_;
  resolving_peers_ = 0;
  if (resolved_download_from_list_.empty()) {
    abort_query(td::Status::Error(ErrorCode::timeout, "custom archive peer resolve timeout"));
    return;
  }
  download_from_list_ = std::move(resolved_download_from_list_);
  try_download(0);
}

void DownloadArchiveSlice::try_download(int index){
  if (record_archive_sync_metrics_ && use_sender_for_prepare_query_ && index == 0 && download_from_list_.size() > 1) {
    try_download_parallel();
    return;
  }

  download_from_ = download_from_list_[index];
  current_peer_index_ = index;
  current_peer_count_ = static_cast<int>(download_from_list_.size());

  if (record_archive_sync_metrics_) {
    record_custom_overlay_sync_peer_download(CustomOverlaySyncKind::Archive, archive_sync_sender_,
                                             CustomOverlaySyncResult::Attempt);
  }
  LOG(WARNING) << "[archive-sync] stage=archive_info.start source=" << archive_source()
               << " transport=" << archive_prepare_transport() << " seqno=" << masterchain_seqno_
               << " shard=" << shard_prefix_.to_str()
               << " peer=" << download_from_ << " peer_index=" << index << " peers=" << download_from_list_.size()
               << " result=start";

  auto query_id = ++archive_info_query_id_;
  archive_info_started_at_ = block_propagation_trace_now();
  delay_action(
      [SelfId = actor_id(this), query_id, index, total_nodes = static_cast<int>(download_from_list_.size())]() {
        td::actor::send_closure(SelfId, &DownloadArchiveSlice::archive_info_timeout, query_id, index, total_nodes);
      },
      td::Timestamp::in(archive_info_timeout_seconds()));
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this),
                                       query_id,
                                       index,
                                       total_nodes = static_cast<int>(download_from_list_.size())](td::Result<td::BufferSlice> R) {
      td::actor::send_closure(SelfId, &DownloadArchiveSlice::got_archive_info_result, query_id, index, total_nodes,
                              std::move(R));
  });

  td::BufferSlice q;
  if (shard_prefix_.is_masterchain()) {
    q = create_serialize_tl_object<ton_api::tonNode_getArchiveInfo>(masterchain_seqno_);
  } else {
    q = create_serialize_tl_object<ton_api::tonNode_getShardArchiveInfo>(masterchain_seqno_,
                                                                         create_tl_shard_id(shard_prefix_));
  }
  if (client_.empty()) {
    if (use_sender_for_prepare_query_) {
      td::actor::send_closure(overlays_, &overlay::Overlays::send_query_via, download_from_, local_id_, overlay_id_,
                              "get_archive_info", std::move(P), td::Timestamp::in(5.0), std::move(q),
                              adnl::Adnl::huge_packet_max_size(), rldp_);
    } else {
      td::actor::send_closure(overlays_, &overlay::Overlays::send_query, download_from_, local_id_, overlay_id_,
                              "get_archive_info", std::move(P), td::Timestamp::in(5.0), std::move(q));
    }
  } else {
    td::actor::send_closure(client_, &adnl::AdnlExtClient::send_query, "get_archive_info",
                            create_serialize_tl_object_suffix<ton_api::tonNode_query>(std::move(q)),
                            td::Timestamp::in(3.0), std::move(P));
  }
}

void DownloadArchiveSlice::try_download_parallel() {
  current_peer_count_ = static_cast<int>(download_from_list_.size());
  archive_info_parallel_ = true;
  archive_info_pending_ = current_peer_count_;
  archive_info_finished_by_peer_.assign(download_from_list_.size(), false);
  archive_info_started_at_by_peer_.assign(download_from_list_.size(), 0.0);

  auto query_id = ++archive_info_query_id_;
  auto timeout = td::Timestamp::in(archive_info_timeout_seconds());
  for (std::size_t i = 0; i < download_from_list_.size(); i++) {
    auto index = static_cast<int>(i);
    auto peer = download_from_list_[i];
    archive_info_started_at_by_peer_[i] = block_propagation_trace_now();
    record_custom_overlay_sync_peer_download(CustomOverlaySyncKind::Archive, archive_sync_sender_,
                                             CustomOverlaySyncResult::Attempt);
    LOG(WARNING) << "[archive-sync] stage=archive_info.start source=" << archive_source()
                 << " transport=" << archive_prepare_transport() << " seqno=" << masterchain_seqno_
                 << " shard=" << shard_prefix_.to_str()
                 << " peer=" << peer << " peer_index=" << index << " peers=" << download_from_list_.size()
                 << " mode=parallel result=start";

    delay_action(
        [SelfId = actor_id(this), query_id, index, total_nodes = current_peer_count_]() {
          td::actor::send_closure(SelfId, &DownloadArchiveSlice::archive_info_timeout, query_id, index, total_nodes);
        },
        timeout);
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), query_id, index,
                                         total_nodes = current_peer_count_](td::Result<td::BufferSlice> R) {
      td::actor::send_closure(SelfId, &DownloadArchiveSlice::got_archive_info_result, query_id, index, total_nodes,
                              std::move(R));
    });

    td::BufferSlice q;
    if (shard_prefix_.is_masterchain()) {
      q = create_serialize_tl_object<ton_api::tonNode_getArchiveInfo>(masterchain_seqno_);
    } else {
      q = create_serialize_tl_object<ton_api::tonNode_getShardArchiveInfo>(masterchain_seqno_,
                                                                           create_tl_shard_id(shard_prefix_));
    }
    td::actor::send_closure(overlays_, &overlay::Overlays::send_query_via, peer, local_id_, overlay_id_,
                            "get_archive_info", std::move(P), td::Timestamp::in(5.0), std::move(q),
                            adnl::Adnl::huge_packet_max_size(), rldp_);
  }
}

void DownloadArchiveSlice::got_archive_info_result(td::uint64 query_id, int index, int total_nodes,
                                                   td::Result<td::BufferSlice> result) {
  if (query_id != archive_info_query_id_) {
    return;
  }
  if (archive_info_parallel_) {
    if (index < 0 || index >= static_cast<int>(download_from_list_.size()) || archive_info_finished_by_peer_[index]) {
      return;
    }
    auto peer = download_from_list_[index];
    auto started_at = archive_info_started_at_by_peer_[index];
    archive_info_finished_by_peer_[index] = true;
    CHECK(archive_info_pending_ > 0);
    archive_info_pending_--;
    if (result.is_error()) {
      auto error = result.move_as_error();
      auto reason = archive_status_reason(error.clone());
      record_custom_overlay_sync_peer_download(CustomOverlaySyncKind::Archive, archive_sync_sender_,
                                               archive_sync_result_from_status(error), started_at,
                                               block_propagation_trace_now());
      LOG(WARNING) << "[archive-sync] stage=archive_info.done source=" << archive_source()
                   << " transport=" << archive_prepare_transport() << " seqno=" << masterchain_seqno_
                   << " shard=" << shard_prefix_.to_str()
                   << " peer=" << peer << " peer_index=" << index << " peers=" << total_nodes
                   << " mode=parallel ms=" << archive_elapsed_ms(started_at)
                   << " result=error reason=" << reason;
      if (archive_info_pending_ == 0) {
        ++archive_info_query_id_;
        archive_info_parallel_ = false;
        if (!try_random_public_peers_fallback(error.clone())) {
          abort_query(std::move(error));
        }
      }
      return;
    }
    record_custom_overlay_sync_peer_download(CustomOverlaySyncKind::Archive, archive_sync_sender_,
                                             CustomOverlaySyncResult::Ok, started_at,
                                             block_propagation_trace_now());
    LOG(WARNING) << "[archive-sync] stage=archive_info.done source=" << archive_source()
                 << " transport=" << archive_prepare_transport() << " seqno=" << masterchain_seqno_
                 << " shard=" << shard_prefix_.to_str()
                 << " peer=" << peer << " peer_index=" << index << " peers=" << total_nodes
                 << " mode=parallel ms=" << archive_elapsed_ms(started_at) << " result=ok";
    download_from_ = peer;
    current_peer_index_ = index;
    current_peer_count_ = total_nodes;
    ++archive_info_query_id_;
    archive_info_parallel_ = false;
    got_archive_info(result.move_as_ok());
    return;
  }
  if (result.is_error()) {
    auto error = result.move_as_error();
    auto reason = archive_status_reason(error.clone());
    if (record_archive_sync_metrics_) {
      record_custom_overlay_sync_peer_download(CustomOverlaySyncKind::Archive, archive_sync_sender_,
                                               archive_sync_result_from_status(error), archive_info_started_at_,
                                               block_propagation_trace_now());
    }
    LOG(WARNING) << "[archive-sync] stage=archive_info.done source=" << archive_source()
                 << " transport=" << archive_prepare_transport() << " seqno=" << masterchain_seqno_
                 << " shard=" << shard_prefix_.to_str()
                 << " peer=" << download_from_ << " peer_index=" << index << " peers=" << total_nodes
                 << " ms=" << archive_elapsed_ms(archive_info_started_at_) << " result=error reason=" << reason;
    if (index + 1 >= total_nodes) {
      if (!try_random_public_peers_fallback(error.clone())) {
        abort_query(std::move(error));
      }
    } else {
      try_download(index + 1);
    }
    return;
  }
  if (record_archive_sync_metrics_) {
    record_custom_overlay_sync_peer_download(CustomOverlaySyncKind::Archive, archive_sync_sender_,
                                             CustomOverlaySyncResult::Ok, archive_info_started_at_,
                                             block_propagation_trace_now());
  }
  LOG(WARNING) << "[archive-sync] stage=archive_info.done source=" << archive_source()
               << " transport=" << archive_prepare_transport() << " seqno=" << masterchain_seqno_
               << " shard=" << shard_prefix_.to_str()
               << " peer=" << download_from_ << " peer_index=" << index << " peers=" << total_nodes
               << " ms=" << archive_elapsed_ms(archive_info_started_at_) << " result=ok";
  ++archive_info_query_id_;
  got_archive_info(result.move_as_ok());
}

void DownloadArchiveSlice::archive_info_timeout(td::uint64 query_id, int index, int total_nodes) {
  if (query_id != archive_info_query_id_) {
    return;
  }
  if (archive_info_parallel_) {
    if (index < 0 || index >= static_cast<int>(download_from_list_.size()) || archive_info_finished_by_peer_[index]) {
      return;
    }
    auto peer = download_from_list_[index];
    auto started_at = archive_info_started_at_by_peer_[index];
    archive_info_finished_by_peer_[index] = true;
    CHECK(archive_info_pending_ > 0);
    archive_info_pending_--;
    record_custom_overlay_sync_peer_download(CustomOverlaySyncKind::Archive, archive_sync_sender_,
                                             CustomOverlaySyncResult::Timeout, started_at,
                                             block_propagation_trace_now());
    LOG(WARNING) << "[archive-sync] stage=archive_info.done source=" << archive_source()
                 << " transport=" << archive_prepare_transport() << " seqno=" << masterchain_seqno_
                 << " shard=" << shard_prefix_.to_str()
                 << " peer=" << peer << " peer_index=" << index << " peers=" << total_nodes
                 << " mode=parallel ms=" << archive_elapsed_ms(started_at)
                 << " result=timeout reason=archive_info_timeout";
    if (archive_info_pending_ == 0) {
      ++archive_info_query_id_;
      archive_info_parallel_ = false;
      auto error = td::Status::Error(ErrorCode::timeout, PSTRING() << archive_source() << " archive info timeout");
      if (!try_random_public_peers_fallback(error.clone())) {
        abort_query(std::move(error));
      }
    }
    return;
  }
  if (record_archive_sync_metrics_) {
    record_custom_overlay_sync_peer_download(CustomOverlaySyncKind::Archive, archive_sync_sender_,
                                             CustomOverlaySyncResult::Timeout, archive_info_started_at_,
                                             block_propagation_trace_now());
  }
  LOG(WARNING) << "[archive-sync] stage=archive_info.done source=" << archive_source()
               << " transport=" << archive_prepare_transport() << " seqno=" << masterchain_seqno_
               << " shard=" << shard_prefix_.to_str()
               << " peer=" << download_from_ << " peer_index=" << index << " peers=" << total_nodes
               << " ms=" << archive_elapsed_ms(archive_info_started_at_)
               << " result=timeout reason=archive_info_timeout";
  if (index + 1 >= total_nodes) {
    ++archive_info_query_id_;
    auto error = td::Status::Error(ErrorCode::timeout, PSTRING() << archive_source() << " archive info timeout");
    if (!try_random_public_peers_fallback(error.clone())) {
      abort_query(std::move(error));
    }
  } else {
    try_download(index + 1);
  }
}


void DownloadArchiveSlice::got_archive_info(td::BufferSlice data) {
  auto F = fetch_tl_object<ton_api::tonNode_ArchiveInfo>(std::move(data), true);
  if (F.is_error()) {
    abort_query(F.move_as_error_prefix("failed to parse ArchiveInfo answer"));
    return;
  }
  auto f = F.move_as_ok();

  bool fail = false;

  ton_api::downcast_call(*f.get(), td::overloaded(
                                       [&](const ton_api::tonNode_archiveNotFound &obj) {
                                         auto error_message = "remote db not found in member " + download_from_.serialize();

                                         if (original_zero_download_){
                                           error_message += " (not random, ";
                                         } else {
                                           error_message += " (random, ";
                                         }

                                         if (!client_.empty()){
                                           error_message += " client)";
                                         } else {
                                           error_message += " overlay)";
                                         }

                                         LOG(WARNING) << "[archive-sync] stage=archive_info.done source="
                                                      << archive_source()
                                                      << " transport=" << archive_prepare_transport()
                                                      << " seqno=" << masterchain_seqno_
                                                      << " shard=" << shard_prefix_.to_str()
                                                      << " peer=" << download_from_
                                                      << " peer_index=" << current_peer_index_
                                                      << " peers=" << current_peer_count_
                                                      << " result=error reason=archive_not_found";
                                         if (current_peer_index_ + 1 < current_peer_count_) {
                                           try_download(current_peer_index_ + 1);
                                         } else {
                                           auto error = td::Status::Error(ErrorCode::notready, error_message);
                                           if (!try_random_public_peers_fallback(error.clone())) {
                                             abort_query(std::move(error));
                                           }
                                         }
                                         fail = true;
                                       },
                                       [&](const ton_api::tonNode_archiveInfo &obj) { archive_id_ = obj.id_; }));
  if (fail) {
    return;
  }

  prev_logged_timer_ = td::Timer();
  LOG(WARNING) << "[archive-sync] stage=slice.start source=" << archive_source()
               << " transport=" << archive_slice_transport() << " seqno=" << masterchain_seqno_
               << " shard=" << shard_prefix_.to_str()
               << " peer=" << download_from_ << " archive_id=" << archive_id_ << " result=start";
  get_archive_slice();
}

void DownloadArchiveSlice::get_archive_slice() {
  auto query_id = ++archive_slice_query_id_;
  archive_slice_started_at_ = block_propagation_trace_now();
  LOG(WARNING) << "[archive-sync] stage=slice.chunk.start source=" << archive_source()
               << " transport=" << archive_slice_transport() << " seqno=" << masterchain_seqno_
               << " shard=" << shard_prefix_.to_str()
               << " peer=" << download_from_ << " archive_id=" << archive_id_ << " offset=" << offset_
               << " bytes=" << slice_size() << " result=start";
  delay_action(
      [SelfId = actor_id(this), query_id]() {
        td::actor::send_closure(SelfId, &DownloadArchiveSlice::archive_slice_timeout, query_id);
      },
      td::Timestamp::in(kArchiveSliceChunkTimeout));
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), query_id](td::Result<td::BufferSlice> R) {
    td::actor::send_closure(SelfId, &DownloadArchiveSlice::got_archive_slice_result, query_id, std::move(R));
  });

  auto q = create_serialize_tl_object<ton_api::tonNode_getArchiveSlice>(archive_id_, offset_, slice_size());
  if (client_.empty()) {
    if (use_sender_for_slice_query_) {
      td::actor::send_closure(overlays_, &overlay::Overlays::send_query_via, download_from_, local_id_, overlay_id_,
                              "get_archive_slice", std::move(P), td::Timestamp::in(15.0), std::move(q),
                              slice_size() + 1024, rldp_);
    } else {
      td::actor::send_closure(overlays_, &overlay::Overlays::send_query, download_from_, local_id_, overlay_id_,
                              "get_archive_slice", std::move(P), td::Timestamp::in(15.0), std::move(q));
    }
  } else {
    td::actor::send_closure(client_, &adnl::AdnlExtClient::send_query, "get_archive_slice",
                            create_serialize_tl_object_suffix<ton_api::tonNode_query>(std::move(q)),
                            td::Timestamp::in(15.0), std::move(P));
  }
}

void DownloadArchiveSlice::got_archive_slice_result(td::uint64 query_id, td::Result<td::BufferSlice> result) {
  if (query_id != archive_slice_query_id_) {
    return;
  }
  if (result.is_error()) {
    auto error = result.move_as_error();
    auto reason = archive_status_reason(error.clone());
    LOG(WARNING) << "[archive-sync] stage=slice.chunk.done source=" << archive_source()
                 << " transport=" << archive_slice_transport() << " seqno=" << masterchain_seqno_
                 << " shard=" << shard_prefix_.to_str()
                 << " peer=" << download_from_ << " peer_index=" << current_peer_index_
                 << " peers=" << current_peer_count_ << " archive_id=" << archive_id_ << " offset=" << offset_
                 << " ms=" << archive_elapsed_ms(archive_slice_started_at_)
                 << " result=error reason=" << reason;
    if (current_peer_index_ + 1 < current_peer_count_) {
      try_download(current_peer_index_ + 1);
    } else {
      if (!try_random_public_peers_fallback(error.clone())) {
        abort_query(std::move(error));
      }
    }
    return;
  }
  got_archive_slice(result.move_as_ok());
}

void DownloadArchiveSlice::archive_slice_timeout(td::uint64 query_id) {
  if (query_id != archive_slice_query_id_) {
    return;
  }
  LOG(WARNING) << "[archive-sync] stage=slice.chunk.done source=" << archive_source()
               << " transport=" << archive_slice_transport() << " seqno=" << masterchain_seqno_
               << " shard=" << shard_prefix_.to_str()
               << " peer=" << download_from_ << " peer_index=" << current_peer_index_
               << " peers=" << current_peer_count_ << " archive_id=" << archive_id_ << " offset=" << offset_
               << " ms=" << archive_elapsed_ms(archive_slice_started_at_)
               << " result=timeout reason=archive_slice_timeout";
  ++archive_slice_query_id_;
  if (current_peer_index_ + 1 < current_peer_count_) {
    try_download(current_peer_index_ + 1);
  } else {
    auto error = td::Status::Error(ErrorCode::timeout, PSTRING() << archive_source() << " archive slice timeout");
    if (!try_random_public_peers_fallback(error.clone())) {
      abort_query(std::move(error));
    }
  }
}

void DownloadArchiveSlice::got_archive_slice(td::BufferSlice data) {
  auto chunk_offset = offset_;
  auto chunk_size = data.size();
  auto R = fd_.write(data.as_slice());
  if (R.is_error()) {
    abort_query(R.move_as_error_prefix("failed to write temp file: "));
    return;
  }
  if (R.move_as_ok() != data.size()) {
    abort_query(td::Status::Error(ErrorCode::error, "short write to temp file"));
    return;
  }

  offset_ += data.size();
  LOG(WARNING) << "[archive-sync] stage=slice.chunk.done source=" << archive_source()
               << " transport=" << archive_slice_transport() << " seqno=" << masterchain_seqno_
               << " shard=" << shard_prefix_.to_str()
               << " peer=" << download_from_ << " archive_id=" << archive_id_ << " offset=" << chunk_offset
               << " bytes=" << chunk_size << " next_offset=" << offset_
               << " ms=" << archive_elapsed_ms(archive_slice_started_at_) << " result=ok";

  double elapsed = prev_logged_timer_.elapsed();
  if (elapsed > 10.0) {
    prev_logged_timer_ = td::Timer();
    LOG(INFO) << "downloading archive slice #" << masterchain_seqno_ << " " << shard_prefix_ << ": total=" << offset_
              << " (" << td::format::as_size((td::uint64)(double(offset_ - prev_logged_sum_) / elapsed)) << "/s)";
    prev_logged_sum_ = offset_;
  }

  if (data.size() < slice_size()) {
    LOG(INFO) << "finished downloading arcrive slice #" << masterchain_seqno_ << " " << shard_prefix_
              << ": total=" << offset_;
    finish_query();
  } else {
    get_archive_slice();
  }
}

}  // namespace fullnode

}  // namespace validator

}  // namespace ton
