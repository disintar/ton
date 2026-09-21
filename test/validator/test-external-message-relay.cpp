// SPDX-License-Identifier: LGPL-2.0-or-later

#include "td/utils/tests.h"
#include "validator/external-message-relay.h"

using ton::validator::fullnode::ExternalMessageRelayCache;

TEST(ExternalMessageRelay, SuppressesDuplicatesWithoutExtendingExpiry) {
  ExternalMessageRelayCache cache;
  const std::string hash(64, '0');
  ASSERT_TRUE(cache.admit(hash, 100.0));
  ASSERT_TRUE(!cache.admit(hash, 101.0));
  ASSERT_TRUE(!cache.admit(hash, 159.9));
  ASSERT_TRUE(cache.admit(hash, 160.0));
  ASSERT_TRUE(!cache.admit(hash, 161.0));
}

TEST(ExternalMessageRelay, BoundedCacheAllowsEvictedMessage) {
  ExternalMessageRelayCache cache(2);
  const std::string a(64, '0'), b(64, '1'), c(64, '2');
  ASSERT_TRUE(cache.admit(a, 100.0));
  ASSERT_TRUE(cache.admit(b, 100.0));
  ASSERT_TRUE(cache.admit(c, 100.0));
  ASSERT_TRUE(!cache.admit(b, 101.0));
  ASSERT_TRUE(!cache.admit(c, 101.0));
  ASSERT_TRUE(cache.admit(a, 101.0));
}
