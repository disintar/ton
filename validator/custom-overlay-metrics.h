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

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>

namespace ton {

namespace validator {

namespace fullnode {

inline std::atomic<std::uint64_t> custom_overlay_block_broadcasts_received_total{0};
inline std::atomic<std::uint64_t> custom_overlay_block_broadcasts_applied_total{0};
inline std::atomic<std::uint64_t> custom_overlay_duplicate_block_broadcasts_dropped_total{0};
inline std::atomic<std::uint64_t> custom_overlay_duplicate_block_candidates_dropped_total{0};
inline std::atomic<std::uint64_t> public_overlay_duplicate_block_broadcasts_dropped_total{0};
inline std::atomic<std::uint64_t> public_overlay_duplicate_block_candidates_dropped_total{0};

enum class CustomOverlaySyncKind : std::size_t { Block = 0, NextBlock = 1, Archive = 2, Count = 3 };
enum class CustomOverlaySyncSender : std::size_t { Rldp2 = 0, Quic = 1, Count = 2 };
enum class CustomOverlaySyncResult : std::size_t {
  Attempt = 0,
  Ok = 1,
  Error = 2,
  NoPeer = 3,
  Timeout = 4,
  NotReady = 5,
  ShardNotServed = 6,
  Exhausted = 7,
  Count = 8
};
enum class CustomOverlaySyncFallbackReason : std::size_t {
  CustomError = 0,
  NoCustomOverlay = 1,
  ShardNotServed = 2,
  NoLocalActor = 3,
  BadArchiveImport = 4,
  Count = 5
};
enum class PublicOverlaySyncReason : std::size_t { Direct = 0, Fallback = 1, Count = 2 };

template <class Enum>
inline constexpr std::size_t metric_index(Enum value) {
  return static_cast<std::size_t>(value);
}

inline constexpr std::size_t custom_overlay_sync_kind_count() {
  return metric_index(CustomOverlaySyncKind::Count);
}

inline constexpr std::size_t custom_overlay_sync_sender_count() {
  return metric_index(CustomOverlaySyncSender::Count);
}

inline constexpr std::size_t custom_overlay_sync_result_count() {
  return metric_index(CustomOverlaySyncResult::Count);
}

inline constexpr std::size_t custom_overlay_sync_fallback_reason_count() {
  return metric_index(CustomOverlaySyncFallbackReason::Count);
}

inline constexpr std::size_t public_overlay_sync_reason_count() {
  return metric_index(PublicOverlaySyncReason::Count);
}

inline const char *custom_overlay_sync_kind_label(std::size_t value) {
  static constexpr std::array<const char *, custom_overlay_sync_kind_count()> labels = {"block", "next_block",
                                                                                         "archive"};
  return labels[value];
}

inline const char *custom_overlay_sync_sender_label(std::size_t value) {
  static constexpr std::array<const char *, custom_overlay_sync_sender_count()> labels = {"rldp2", "quic"};
  return labels[value];
}

inline const char *custom_overlay_sync_result_label(std::size_t value) {
  static constexpr std::array<const char *, custom_overlay_sync_result_count()> labels = {
      "attempt", "ok", "error", "no_peer", "timeout", "not_ready", "shard_not_served", "exhausted"};
  return labels[value];
}

inline const char *custom_overlay_sync_fallback_reason_label(std::size_t value) {
  static constexpr std::array<const char *, custom_overlay_sync_fallback_reason_count()> labels = {
      "custom_error", "no_custom_overlay", "shard_not_served", "no_local_actor", "bad_archive_import"};
  return labels[value];
}

inline const char *public_overlay_sync_reason_label(std::size_t value) {
  static constexpr std::array<const char *, public_overlay_sync_reason_count()> labels = {"direct", "fallback"};
  return labels[value];
}

using CustomOverlaySyncResultCounters = std::array<std::atomic<std::uint64_t>, custom_overlay_sync_result_count()>;
using CustomOverlaySyncSenderCounters =
    std::array<CustomOverlaySyncResultCounters, custom_overlay_sync_sender_count()>;
using CustomOverlaySyncCounters = std::array<CustomOverlaySyncSenderCounters, custom_overlay_sync_kind_count()>;

inline CustomOverlaySyncCounters custom_overlay_sync_downloads_total{};
inline CustomOverlaySyncCounters custom_overlay_sync_download_latency_ms_sum{};
inline CustomOverlaySyncCounters custom_overlay_sync_download_latency_ms_count{};
inline CustomOverlaySyncCounters custom_overlay_sync_peer_downloads_total{};
inline CustomOverlaySyncCounters custom_overlay_sync_peer_latency_ms_sum{};
inline CustomOverlaySyncCounters custom_overlay_sync_peer_latency_ms_count{};

using CustomOverlaySyncFallbackCounters =
    std::array<std::array<std::atomic<std::uint64_t>, custom_overlay_sync_fallback_reason_count()>,
               custom_overlay_sync_kind_count()>;
inline CustomOverlaySyncFallbackCounters custom_overlay_sync_fallbacks_total{};

using PublicOverlaySyncCounters =
    std::array<std::array<std::atomic<std::uint64_t>, public_overlay_sync_reason_count()>,
               custom_overlay_sync_kind_count()>;
inline PublicOverlaySyncCounters public_overlay_sync_downloads_total{};

inline void record_custom_overlay_block_broadcast_received() {
  custom_overlay_block_broadcasts_received_total.fetch_add(1, std::memory_order_relaxed);
}

inline void record_custom_overlay_block_broadcast_applied() {
  custom_overlay_block_broadcasts_applied_total.fetch_add(1, std::memory_order_relaxed);
}

inline void record_custom_overlay_duplicate_block_broadcast_dropped() {
  custom_overlay_duplicate_block_broadcasts_dropped_total.fetch_add(1, std::memory_order_relaxed);
}

inline void record_custom_overlay_duplicate_block_candidate_dropped() {
  custom_overlay_duplicate_block_candidates_dropped_total.fetch_add(1, std::memory_order_relaxed);
}

inline void record_public_overlay_duplicate_block_broadcast_dropped() {
  public_overlay_duplicate_block_broadcasts_dropped_total.fetch_add(1, std::memory_order_relaxed);
}

inline void record_public_overlay_duplicate_block_candidate_dropped() {
  public_overlay_duplicate_block_candidates_dropped_total.fetch_add(1, std::memory_order_relaxed);
}

inline std::uint64_t get_custom_overlay_block_broadcasts_received_total() {
  return custom_overlay_block_broadcasts_received_total.load(std::memory_order_relaxed);
}

inline std::uint64_t get_custom_overlay_block_broadcasts_applied_total() {
  return custom_overlay_block_broadcasts_applied_total.load(std::memory_order_relaxed);
}

inline std::uint64_t get_custom_overlay_duplicate_block_broadcasts_dropped_total() {
  return custom_overlay_duplicate_block_broadcasts_dropped_total.load(std::memory_order_relaxed);
}

inline std::uint64_t get_custom_overlay_duplicate_block_candidates_dropped_total() {
  return custom_overlay_duplicate_block_candidates_dropped_total.load(std::memory_order_relaxed);
}

inline std::uint64_t get_public_overlay_duplicate_block_broadcasts_dropped_total() {
  return public_overlay_duplicate_block_broadcasts_dropped_total.load(std::memory_order_relaxed);
}

inline std::uint64_t get_public_overlay_duplicate_block_candidates_dropped_total() {
  return public_overlay_duplicate_block_candidates_dropped_total.load(std::memory_order_relaxed);
}

inline std::uint64_t elapsed_ms(double started_at, double finished_at) {
  if (started_at <= 0.0 || finished_at < started_at) {
    return 0;
  }
  return static_cast<std::uint64_t>((finished_at - started_at) * 1000.0);
}

inline void record_custom_overlay_sync_download(CustomOverlaySyncKind kind, CustomOverlaySyncSender sender,
                                                CustomOverlaySyncResult result, double started_at = 0.0,
                                                double finished_at = 0.0) {
  auto kind_i = metric_index(kind);
  auto sender_i = metric_index(sender);
  auto result_i = metric_index(result);
  custom_overlay_sync_downloads_total[kind_i][sender_i][result_i].fetch_add(1, std::memory_order_relaxed);
  if (started_at > 0.0 && finished_at >= started_at) {
    custom_overlay_sync_download_latency_ms_sum[kind_i][sender_i][result_i].fetch_add(
        elapsed_ms(started_at, finished_at), std::memory_order_relaxed);
    custom_overlay_sync_download_latency_ms_count[kind_i][sender_i][result_i].fetch_add(1,
                                                                                       std::memory_order_relaxed);
  }
}

inline void record_custom_overlay_sync_peer_download(CustomOverlaySyncKind kind, CustomOverlaySyncSender sender,
                                                     CustomOverlaySyncResult result, double started_at = 0.0,
                                                     double finished_at = 0.0) {
  auto kind_i = metric_index(kind);
  auto sender_i = metric_index(sender);
  auto result_i = metric_index(result);
  custom_overlay_sync_peer_downloads_total[kind_i][sender_i][result_i].fetch_add(1, std::memory_order_relaxed);
  if (started_at > 0.0 && finished_at >= started_at) {
    custom_overlay_sync_peer_latency_ms_sum[kind_i][sender_i][result_i].fetch_add(elapsed_ms(started_at, finished_at),
                                                                                  std::memory_order_relaxed);
    custom_overlay_sync_peer_latency_ms_count[kind_i][sender_i][result_i].fetch_add(1, std::memory_order_relaxed);
  }
}

inline void record_custom_overlay_sync_fallback(CustomOverlaySyncKind kind, CustomOverlaySyncFallbackReason reason) {
  custom_overlay_sync_fallbacks_total[metric_index(kind)][metric_index(reason)].fetch_add(1,
                                                                                         std::memory_order_relaxed);
}

inline void record_public_overlay_sync_download(CustomOverlaySyncKind kind, PublicOverlaySyncReason reason) {
  public_overlay_sync_downloads_total[metric_index(kind)][metric_index(reason)].fetch_add(1,
                                                                                         std::memory_order_relaxed);
}

inline std::uint64_t get_custom_overlay_sync_downloads_total(std::size_t kind, std::size_t sender,
                                                             std::size_t result) {
  return custom_overlay_sync_downloads_total[kind][sender][result].load(std::memory_order_relaxed);
}

inline std::uint64_t get_custom_overlay_sync_download_latency_ms_sum(std::size_t kind, std::size_t sender,
                                                                     std::size_t result) {
  return custom_overlay_sync_download_latency_ms_sum[kind][sender][result].load(std::memory_order_relaxed);
}

inline std::uint64_t get_custom_overlay_sync_download_latency_ms_count(std::size_t kind, std::size_t sender,
                                                                       std::size_t result) {
  return custom_overlay_sync_download_latency_ms_count[kind][sender][result].load(std::memory_order_relaxed);
}

inline std::uint64_t get_custom_overlay_sync_peer_downloads_total(std::size_t kind, std::size_t sender,
                                                                  std::size_t result) {
  return custom_overlay_sync_peer_downloads_total[kind][sender][result].load(std::memory_order_relaxed);
}

inline std::uint64_t get_custom_overlay_sync_peer_latency_ms_sum(std::size_t kind, std::size_t sender,
                                                                 std::size_t result) {
  return custom_overlay_sync_peer_latency_ms_sum[kind][sender][result].load(std::memory_order_relaxed);
}

inline std::uint64_t get_custom_overlay_sync_peer_latency_ms_count(std::size_t kind, std::size_t sender,
                                                                   std::size_t result) {
  return custom_overlay_sync_peer_latency_ms_count[kind][sender][result].load(std::memory_order_relaxed);
}

inline std::uint64_t get_custom_overlay_sync_fallbacks_total(std::size_t kind, std::size_t reason) {
  return custom_overlay_sync_fallbacks_total[kind][reason].load(std::memory_order_relaxed);
}

inline std::uint64_t get_public_overlay_sync_downloads_total(std::size_t kind, std::size_t reason) {
  return public_overlay_sync_downloads_total[kind][reason].load(std::memory_order_relaxed);
}

}  // namespace fullnode

}  // namespace validator

}  // namespace ton
