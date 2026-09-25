// SPDX-License-Identifier: LGPL-2.0-or-later
#pragma once

#include <algorithm>
#include <cstddef>
#include <map>
#include <optional>
#include <vector>

namespace ton::validator::fullnode {

// Keep every other candidate available after the fastest info responder fails.
template <class Peer>
void archive_promote_peer(std::vector<Peer> &peers, std::size_t winner) {
  std::rotate(peers.begin(), peers.begin() + winner, peers.begin() + winner + 1);
}

// Control-plane RTT does not establish that a peer can serve archive data.
// Rotate the bounded request budget across all neighbors, including on retries.
template <class Peer>
std::vector<Peer> archive_peer_window(const std::vector<Peer> &peers, std::size_t &cursor,
                                      std::size_t limit) {
  std::vector<Peer> result;
  if (peers.empty()) {
    cursor = 0;
    return result;
  }
  auto count = std::min(limit, peers.size());
  result.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    result.push_back(peers[(cursor + i) % peers.size()]);
  }
  cursor = (cursor + count) % peers.size();
  return result;
}

enum class ArchivePeerResult { Success, Unavailable, BulkFailure };

// Owned exclusively by the overlay/shard actor. Downloader actors return
// feedback through actor messages, never sharing this mutable state.
template <class Peer>
class ArchivePeerHistory {
 public:
  void record(const Peer &peer, ArchivePeerResult result, double now) {
    expire(now);
    if (result == ArchivePeerResult::Success) {
      failed_until_.erase(peer);
      preferred_ = peer;
    } else {
      if (preferred_ && *preferred_ == peer) preferred_.reset();
      if (result == ArchivePeerResult::BulkFailure) {
        failed_until_[peer] = now + 30.0;
        if (failed_until_.size() > 64) {
          auto oldest = std::min_element(failed_until_.begin(), failed_until_.end(),
                                        [](const auto &a, const auto &b) { return a.second < b.second; });
          failed_until_.erase(oldest);
        }
      }
    }
  }

  std::vector<Peer> select(const std::vector<Peer> &candidates, std::size_t limit, double now) {
    expire(now);
    std::vector<Peer> eligible;
    std::vector<Peer> result;
    if (!limit) return result;
    for (const auto &peer : candidates) {
      if (failed_until_.count(peer)) continue;
      if (preferred_ && peer == *preferred_) {
        if (result.empty()) result.push_back(peer);
      } else if (std::find(eligible.begin(), eligible.end(), peer) == eligible.end()) {
        eligible.push_back(peer);
      }
    }
    auto rest = archive_peer_window(eligible, cursor_, limit - result.size());
    result.insert(result.end(), rest.begin(), rest.end());
    return result;
  }

  bool is_preferred(const Peer &peer) const { return preferred_ && *preferred_ == peer; }
  std::size_t quarantined_count() const { return failed_until_.size(); }

 private:
  void expire(double now) {
    for (auto it = failed_until_.begin(); it != failed_until_.end();) {
      if (it->second <= now) it = failed_until_.erase(it);
      else ++it;
    }
  }
  std::optional<Peer> preferred_;
  std::map<Peer, double> failed_until_;
  std::size_t cursor_ = 0;
};

}  // namespace ton::validator::fullnode
