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

TEST(ArchivePeers, BulkSuccessRemainsFirstWhileOtherPeersRotate) {
  ArchivePeerHistory<int> history;
  std::vector<int> peers{0, 1, 2, 3, 4, 5, 6};
  history.record(6, ArchivePeerResult::Success, 100.0);
  std::set<int> alternatives;
  for (int retry = 0; retry < 3; ++retry) {
    auto selected = history.select(peers, 5, 101.0);
    ASSERT_EQ(selected[0], 6);
    ASSERT_EQ(selected.size(), 5u);
    alternatives.insert(selected.begin() + 1, selected.end());
  }
  ASSERT_EQ(alternatives.size(), 6u);
}

TEST(ArchivePeers, FailedBulkPeerQuarantinedForThirtySeconds) {
  ArchivePeerHistory<int> history;
  history.record(4, ArchivePeerResult::Success, 100.0);
  history.record(4, ArchivePeerResult::BulkFailure, 101.0);
  ASSERT_TRUE(!history.is_preferred(4));
  ASSERT_TRUE(history.select({4}, 5, 130.999).empty());
  ASSERT_EQ(history.select({4}, 5, 131.0).size(), 1u);
}

TEST(ArchivePeers, UnavailableTipDoesNotQuarantineAndRemovedPeersStayRemoved) {
  ArchivePeerHistory<int> history;
  history.record(4, ArchivePeerResult::Success, 100.0);
  ASSERT_EQ(history.select({1, 2}, 5, 101.0).size(), 2u);
  history.record(4, ArchivePeerResult::Unavailable, 102.0);
  ASSERT_TRUE(!history.is_preferred(4));
  ASSERT_EQ(history.select({4}, 5, 102.0).size(), 1u);
  ASSERT_EQ(history.quarantined_count(), 0u);
}

TEST(ArchivePeers, FeedbackMemoryAndRequestBudgetAreBounded) {
  ArchivePeerHistory<int> history;
  for (int peer = 0; peer < 100; ++peer) {
    history.record(peer, ArchivePeerResult::BulkFailure, 100.0);
  }
  ASSERT_EQ(history.quarantined_count(), 64u);
  ASSERT_TRUE(history.select({100}, 0, 101.0).empty());
  history.record(100, ArchivePeerResult::Success, 101.0);
  ASSERT_EQ(history.select({100, 100, 101, 101}, 5, 101.0).size(), 2u);
  history.select({}, 5, 131.0);
  ASSERT_EQ(history.quarantined_count(), 0u);
}
