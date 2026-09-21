// SPDX-License-Identifier: LGPL-2.0-or-later

#pragma once

#include <cstdlib>
#include <cstring>
#include <string>

#include "td/utils/LRUCache.h"

namespace ton::validator::fullnode {

inline bool trace_external_message_relay() {
  static const bool enabled = [] {
    const char *value = std::getenv("DTON_TRACE_EXT_MESSAGE_RELAY");
    return value && std::strcmp(value, "1") == 0;
  }();
  return enabled;
}

// Actor-local, bounded deduplication. Repeated receipts do not extend the expiry,
// so a later explicit retry can still be propagated after the suppression window.
class ExternalMessageRelayCache {
 public:
  explicit ExternalMessageRelayCache(td::uint64 capacity = 8192, double ttl = 60.0)
      : entries_(capacity), ttl_(ttl) {
  }

  bool admit(const std::string &hash, double now) {
    auto expires_at = entries_.get_if_exists(hash);
    if (expires_at && *expires_at > now) {
      return false;
    }
    entries_.put(hash, now + ttl_);
    return true;
  }

 private:
  td::LRUCache<std::string, double> entries_;
  double ttl_;
};

}  // namespace ton::validator::fullnode
