/*
    This file is part of TON Blockchain Library.

    TON Blockchain Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
*/
#pragma once

#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

#include "td/utils/Time.h"
#include "td/utils/logging.h"
#include "validator/types.h"

namespace ton {

namespace validator {

namespace overlay_gap {

struct Record {
  BlockIdExt block_id;
  std::string overlay;
  std::string src;
  std::string stage;
  std::string reason;
  double recv_at = 0.0;
  double deserialize_at = 0.0;
  double process_at = 0.0;
  double abort_at = 0.0;
  double applied_at = 0.0;
  bool final = false;
};

inline std::mutex mutex;
inline std::unordered_map<std::string, Record> records;
inline std::deque<std::string> order;
inline constexpr std::size_t max_records = 4096;

inline double now() {
  return td::Clocks::system();
}

inline std::string sanitize(std::string value) {
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

inline std::string key(const BlockIdExt &block_id) {
  return block_id.to_str();
}

inline long long ms_since(double started_at, double finished_at = now()) {
  if (started_at <= 0.0) {
    return -1;
  }
  return static_cast<long long>((finished_at - started_at) * 1000.0);
}

inline void remember(const BlockIdExt &block_id, const char *stage, std::string overlay = {}, std::string src = {},
                     std::string reason = {}, bool final = false) {
  const auto t = now();
  const auto k = key(block_id);
  Record snapshot;
  {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = records.find(k);
    if (it == records.end()) {
      order.push_back(k);
      it = records.emplace(k, Record{}).first;
      it->second.block_id = block_id;
      if (order.size() > max_records) {
        records.erase(order.front());
        order.pop_front();
      }
    }
    auto &record = it->second;
    if (!overlay.empty()) {
      record.overlay = std::move(overlay);
    }
    if (!src.empty()) {
      record.src = std::move(src);
    }
    record.stage = stage;
    record.reason = std::move(reason);
    record.final = record.final || final;
    if (std::string(stage) == "custom.recv" && record.recv_at <= 0.0) {
      record.recv_at = t;
    } else if (std::string(stage) == "custom.deserialize") {
      record.deserialize_at = t;
    } else if (std::string(stage) == "fullnode.process") {
      record.process_at = t;
    } else if (std::string(stage) == "validate.abort" || std::string(stage) == "apply.abort") {
      record.abort_at = t;
    } else if (std::string(stage) == "apply.applied_set") {
      record.applied_at = t;
    }
    snapshot = record;
  }

  bool important = snapshot.applied_at > 0.0 || snapshot.abort_at > 0.0 || std::string(stage) == "custom.deserialize";
  if (important) {
    LOG(WARNING) << "[overlay-gap]"
                 << " event=stage"
                 << " stage=" << stage
                 << " block=" << block_id.to_str()
                 << " wc=" << block_id.id.workchain
                 << " shard=" << block_id.id.shard
                 << " seqno=" << block_id.id.seqno
                 << " overlay=" << sanitize(snapshot.overlay)
                 << " src=" << sanitize(snapshot.src)
                 << " final=" << (snapshot.final ? 1 : 0)
                 << " ms_from_recv=" << ms_since(snapshot.recv_at, t)
                 << " ms_deserialize=" << ms_since(snapshot.deserialize_at, t)
                 << " ms_process=" << ms_since(snapshot.process_at, t)
                 << " reason=" << sanitize(snapshot.reason);
  }
}

inline bool find_next_hint(const BlockIdExt &prev_id, Record &out) {
  std::lock_guard<std::mutex> lock(mutex);
  for (auto it = order.rbegin(); it != order.rend(); ++it) {
    auto record_it = records.find(*it);
    if (record_it == records.end()) {
      continue;
    }
    const auto &record = record_it->second;
    const auto &id = record.block_id.id;
    if (id.workchain == prev_id.id.workchain && id.shard == prev_id.id.shard && id.seqno == prev_id.id.seqno + 1) {
      out = record;
      return true;
    }
  }
  return false;
}

inline void log_download_next_decision(const BlockIdExt &prev_id, const char *source, const char *overlay,
                                       std::string reason = {}) {
  Record hint;
  const bool has_hint = find_next_hint(prev_id, hint);
  const auto t = now();
  LOG(WARNING) << "[overlay-gap]"
               << " event=download_next"
               << " source=" << source
               << " prev=" << prev_id.to_str()
               << " wc=" << prev_id.id.workchain
               << " shard=" << prev_id.id.shard
               << " prev_seqno=" << prev_id.id.seqno
               << " expected_seqno=" << (prev_id.id.seqno + 1)
               << " overlay=" << sanitize(overlay == nullptr ? std::string{} : std::string{overlay})
               << " hint=" << (has_hint ? 1 : 0)
               << " hint_block=" << (has_hint ? hint.block_id.to_str() : "-")
               << " hint_stage=" << (has_hint ? sanitize(hint.stage) : "-")
               << " hint_src=" << (has_hint ? sanitize(hint.src) : "-")
               << " hint_ms_from_recv=" << (has_hint ? ms_since(hint.recv_at, t) : -1)
               << " hint_ms_from_deserialize=" << (has_hint ? ms_since(hint.deserialize_at, t) : -1)
               << " hint_applied=" << (has_hint && hint.applied_at > 0.0 ? 1 : 0)
               << " hint_aborted=" << (has_hint && hint.abort_at > 0.0 ? 1 : 0)
               << " reason=" << sanitize(std::move(reason));
}

}  // namespace overlay_gap

}  // namespace validator

}  // namespace ton
