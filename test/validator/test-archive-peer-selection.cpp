// SPDX-License-Identifier: LGPL-2.0-or-later
#include <set>
#include "td/utils/tests.h"
#include "validator/net/archive-peer-selection.h"

using namespace ton::validator::fullnode;

TEST(ArchivePeers, LastResponderFailureRetainsEarlierCandidates) {
  std::vector<int> peers{0, 1, 2, 3, 4};
  archive_promote_peer(peers, 4);
  ASSERT_EQ(peers[0], 4);
  ASSERT_EQ(peers[1], 0);
  ASSERT_EQ(peers[4], 3);
  ASSERT_EQ(std::set<int>(peers.begin(), peers.end()).size(), 5u);
}

TEST(ArchivePeers, RetriesReachPeersOutsideFastestFive) {
  std::vector<int> peers;
  for (int i = 0; i < 16; ++i) peers.push_back(i);
  std::size_t cursor = 0;
  std::set<int> visited;
  for (int retry = 0; retry < 4; ++retry) {
    auto selected = archive_peer_window(peers, cursor, 5);
    ASSERT_EQ(selected.size(), 5u);
    ASSERT_EQ(std::set<int>(selected.begin(), selected.end()).size(), 5u);
    visited.insert(selected.begin(), selected.end());
  }
  ASSERT_EQ(visited.size(), 16u);
}

TEST(ArchivePeers, HandlesChangingAndEmptyNeighbors) {
  std::size_t cursor = 15;
  auto selected = archive_peer_window(std::vector<int>{7, 8}, cursor, 5);
  ASSERT_EQ(selected.size(), 2u);
  ASSERT_EQ(selected[0], 8);
  ASSERT_EQ(selected[1], 7);
  ASSERT_TRUE(archive_peer_window(std::vector<int>{}, cursor, 5).empty());
  ASSERT_EQ(cursor, 0u);
}
