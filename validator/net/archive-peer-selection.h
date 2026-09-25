// SPDX-License-Identifier: LGPL-2.0-or-later
#pragma once

#include <algorithm>
#include <cstddef>
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

}  // namespace ton::validator::fullnode
