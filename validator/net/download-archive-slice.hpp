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
#pragma once

#include <vector>

#include "adnl/adnl-ext-client.h"
#include "adnl/adnl-node.h"
#include "overlay/overlays.h"
#include "td/utils/port/FileFd.h"
#include "ton/ton-types.h"
#include "validator/custom-overlay-metrics.h"
#include "validator/validator.h"

namespace ton {

namespace validator {

namespace fullnode {

class DownloadArchiveSlice : public td::actor::Actor {
 public:
  DownloadArchiveSlice(BlockSeqno masterchain_seqno, ShardIdFull shard_prefix, std::string tmp_dir,
                       adnl::AdnlNodeIdShort local_id, overlay::OverlayIdShort overlay_id,
                       adnl::AdnlNodeIdShort download_from, td::Timestamp timeout,
                       td::actor::ActorId<ValidatorManagerInterface> validator_manager,
                       td::actor::ActorId<adnl::AdnlSenderInterface> rldp,
                       td::actor::ActorId<overlay::Overlays> overlays, td::actor::ActorId<adnl::Adnl> adnl,
                       td::actor::ActorId<adnl::AdnlExtClient> client, td::Promise<std::string> promise,
                       std::vector<adnl::AdnlNodeIdShort> download_from_list = {},
                       bool use_sender_for_prepare_query = false, bool use_sender_for_slice_query = true,
                       bool resolve_peers_before_download = false, bool record_archive_sync_metrics = false,
                       CustomOverlaySyncSender archive_sync_sender = CustomOverlaySyncSender::Rldp2);

  void abort_query(td::Status reason);
  void alarm() override;
  void finish_query();

  void start_up() override;
  void got_node_to_download(std::vector<adnl::AdnlNodeIdShort> node);
  void got_archive_info(td::BufferSlice data);
  void got_archive_info_result(td::uint64 query_id, int index, int total_nodes, td::Result<td::BufferSlice> result);
  void archive_info_timeout(td::uint64 query_id, int index, int total_nodes);
  void get_archive_slice();
  void got_archive_slice_result(td::uint64 query_id, td::Result<td::BufferSlice> result);
  void archive_slice_timeout(td::uint64 query_id);
  void got_archive_slice(td::BufferSlice data);
  void try_download(int index);
  void try_download_parallel();
  void resolve_download_peers();
  void got_resolved_download_peer(td::uint64 query_id, adnl::AdnlNodeIdShort peer,
                                  td::Result<adnl::AdnlNode> result);
  void resolve_download_peers_timeout(td::uint64 query_id);
  void request_random_public_peers(const char *reason);
  bool try_random_public_peers_fallback(td::Status reason);
  const char *archive_source() const;
  const char *archive_prepare_transport() const;
  const char *archive_slice_transport() const;
  double archive_info_timeout_seconds() const;

  static constexpr td::uint32 slice_size() {
    return 1 << 21;
  }

 private:
  BlockSeqno masterchain_seqno_;
  ShardIdFull shard_prefix_;
  std::string tmp_dir_;
  std::string tmp_name_;
  td::FileFd fd_;
  adnl::AdnlNodeIdShort local_id_;
  overlay::OverlayIdShort overlay_id_;
  td::uint64 offset_ = 0;
  td::uint64 archive_id_;
  bool original_zero_download_ = true;

  adnl::AdnlNodeIdShort download_from_ = adnl::AdnlNodeIdShort::zero();
  std::vector<adnl::AdnlNodeIdShort> download_from_list_;
  int current_peer_index_ = 0;
  int current_peer_count_ = 0;

  td::Timestamp timeout_;
  td::actor::ActorId<ValidatorManagerInterface> validator_manager_;
  td::actor::ActorId<adnl::AdnlSenderInterface> rldp_;
  td::actor::ActorId<overlay::Overlays> overlays_;
  td::actor::ActorId<adnl::Adnl> adnl_;
  td::actor::ActorId<adnl::AdnlExtClient> client_;
  td::Promise<std::string> promise_;
  bool use_sender_for_prepare_query_ = false;
  bool use_sender_for_slice_query_ = true;
  bool resolve_peers_before_download_ = false;
  size_t resolving_peers_ = 0;
  td::uint64 resolve_query_id_ = 0;
  td::uint64 archive_info_query_id_ = 0;
  td::uint64 archive_slice_query_id_ = 0;
  double archive_info_started_at_ = 0.0;
  double archive_slice_started_at_ = 0.0;
  bool archive_info_parallel_ = false;
  int archive_info_pending_ = 0;
  std::vector<bool> archive_info_finished_by_peer_;
  std::vector<double> archive_info_started_at_by_peer_;
  std::vector<adnl::AdnlNodeIdShort> resolved_download_from_list_;
  bool random_public_peers_requested_ = false;
  bool record_archive_sync_metrics_ = false;
  CustomOverlaySyncSender archive_sync_sender_ = CustomOverlaySyncSender::Rldp2;
  double archive_sync_started_at_ = 0.0;
  bool archive_sync_metric_finished_ = false;

  td::uint64 prev_logged_sum_ = 0;
  td::Timer prev_logged_timer_;
};

}  // namespace fullnode

}  // namespace validator

}  // namespace ton
