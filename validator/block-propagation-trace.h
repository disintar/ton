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
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>

#include "td/utils/Time.h"
#include "td/utils/logging.h"
#include "validator/types.h"

namespace ton {

namespace validator {

inline double block_propagation_trace_now() {
  return td::Clocks::system();
}

enum class BlockPropagationTraceMode { Off, Sync, All };

inline BlockPropagationTraceMode block_propagation_trace_mode() {
  static const auto mode = [] {
    const char *env = std::getenv("DTON_TRACE_BLOCK_PROPAGATION");
    if (env == nullptr || env[0] == '\0') {
      return BlockPropagationTraceMode::Off;
    }
    std::string value(env);
    if (value == "0" || value == "false" || value == "FALSE" || value == "off" || value == "OFF") {
      return BlockPropagationTraceMode::Off;
    }
    return value == "sync" ? BlockPropagationTraceMode::Sync : BlockPropagationTraceMode::All;
  }();
  return mode;
}

inline bool block_propagation_trace_enabled() {
  return block_propagation_trace_mode() == BlockPropagationTraceMode::All;
}

inline bool private_sync_trace_enabled() {
  return block_propagation_trace_mode() != BlockPropagationTraceMode::Off;
}

inline double block_propagation_trace_slow_ms() {
  static const double slow_ms = [] {
    const char *env = std::getenv("DTON_TRACE_BLOCK_PROPAGATION_SLOW_MS");
    if (env == nullptr || env[0] == '\0') {
      return 0.0;
    }
    return std::atof(env);
  }();
  return slow_ms;
}

inline long long block_propagation_trace_ms(double started_at, double now) {
  if (started_at <= 0.0) {
    return -1;
  }
  return static_cast<long long>((now - started_at) * 1000.0);
}

inline bool private_sync_trace_should_log(bool failed, long long elapsed_ms) {
  if (!private_sync_trace_enabled()) {
    return false;
  }
  if (block_propagation_trace_mode() == BlockPropagationTraceMode::All) {
    return true;
  }
  if (!failed && elapsed_ms >= 0 && elapsed_ms < block_propagation_trace_slow_ms()) {
    return false;
  }
  static std::atomic<std::uint64_t> bucket{0};
  static std::atomic<std::uint64_t> suppressed{0};
  auto second = static_cast<std::uint64_t>(td::Time::now());
  auto current = bucket.load(std::memory_order_relaxed);
  while (true) {
    auto count = (current >> 8) == second ? current & 255 : 0;
    if (count >= 20) {
      suppressed.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    auto next = (second << 8) | (count + 1);
    if (bucket.compare_exchange_weak(current, next, std::memory_order_relaxed)) {
      if (count == 0) {
        auto skipped = suppressed.exchange(0, std::memory_order_relaxed);
        if (skipped != 0) {
          LOG(INFO) << "[private-sync] suppressed=" << skipped;
        }
      }
      return true;
    }
  }
}

inline std::string block_propagation_trace_sanitize(std::string value) {
  if (value.empty()) {
    return "-";
  }
  for (auto &ch : value) {
    if (ch <= ' ' || ch == '"' || ch == '\'') {
      ch = '_';
    }
  }
  return value;
}

inline bool block_propagation_trace_should_log(const BlockPropagationTrace &trace, double stage_ms,
                                               bool force_when_enabled) {
  if (!block_propagation_trace_enabled()) {
    return false;
  }
  if (trace.enabled || force_when_enabled) {
    return true;
  }
  auto slow_ms = block_propagation_trace_slow_ms();
  return slow_ms > 0.0 && stage_ms >= slow_ms;
}

inline void log_block_propagation_stage(const BlockIdExt &block_id, const BlockPropagationTrace &trace,
                                        const char *stage, const char *source, bool final_known, bool final,
                                        const char *result = "ok", std::string reason = {},
                                        double stage_started_at = 0.0, bool force_when_enabled = false) {
  auto now = block_propagation_trace_now();
  auto stage_ms = block_propagation_trace_ms(stage_started_at, now);
  if (!block_propagation_trace_should_log(trace, static_cast<double>(stage_ms), force_when_enabled)) {
    return;
  }
  auto ms_from_custom = block_propagation_trace_ms(trace.custom_received_at, now);
  LOG(WARNING) << "[block-propagation]"
               << " stage=" << stage << " block=" << block_id.to_str() << " wc=" << block_id.id.workchain
               << " shard=" << block_id.id.shard << " seqno=" << block_id.id.seqno << " source=" << source
               << " overlay=" << block_propagation_trace_sanitize(trace.overlay_name)
               << " src=" << block_propagation_trace_sanitize(trace.src_adnl)
               << " final=" << (final_known && final ? 1 : 0) << " ms_from_custom=" << ms_from_custom
               << " ms_stage=" << stage_ms << " result=" << result
               << " reason=" << block_propagation_trace_sanitize(std::move(reason));
}

inline void log_block_propagation_stage(const BlockBroadcast &broadcast, const char *stage, const char *source,
                                        const char *result = "ok", std::string reason = {},
                                        double stage_started_at = 0.0, bool force_when_enabled = false) {
  bool final_known = !broadcast.sig_set.is_null();
  bool final = final_known && broadcast.sig_set->is_final();
  log_block_propagation_stage(broadcast.block_id, broadcast.trace, stage, source, final_known, final, result,
                              std::move(reason), stage_started_at, force_when_enabled);
}

inline BlockPropagationTrace make_block_propagation_trace(std::string overlay_name, std::string src_adnl,
                                                          double custom_received_at) {
  BlockPropagationTrace trace;
  trace.enabled = block_propagation_trace_enabled();
  if (!trace.enabled) {
    return trace;
  }
  trace.custom_received_at = custom_received_at;
  trace.overlay_name = std::move(overlay_name);
  trace.src_adnl = std::move(src_adnl);
  return trace;
}

}  // namespace validator

}  // namespace ton
