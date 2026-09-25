// SPDX-License-Identifier: LGPL-2.0-or-later
#include "td/utils/tests.h"
#include "validator/net/download-race.h"

using namespace ton::validator::fullnode;

TEST(DownloadRace, PublicStartsBeforeCustomTimeoutWithOriginalDeadline) {
  td::Promise<int> stalled_custom;
  bool custom_started = false, public_started = false;
  int completions = 0;
  auto deadline = td::Timestamp::in(10.0);
  start_download_race<int>(
      deadline, td::PromiseCreator::lambda([&](td::Result<int> result) {
        ASSERT_TRUE(result.is_ok());
        ASSERT_EQ(result.ok(), 42);
        ++completions;
      }),
      [&](td::Timestamp received, td::Promise<int> promise) {
        ASSERT_EQ(received.at(), deadline.at());
        custom_started = true;
        stalled_custom = std::move(promise);
      },
      [&](td::Timestamp received, td::Promise<int> promise) {
        ASSERT_TRUE(custom_started);
        ASSERT_TRUE(!received.is_in_past());
        ASSERT_EQ(received.at(), deadline.at());
        public_started = true;
        promise.set_value(42);
      });
  ASSERT_TRUE(public_started);
  ASSERT_EQ(completions, 1);
  stalled_custom.set_error(td::Status::Error("custom timeout"));
  ASSERT_EQ(completions, 1);
}

TEST(DownloadRace, InvalidResponseDoesNotHideValidAlternative) {
  int completions = 0;
  auto callbacks = download_race_promises<int>(3, td::PromiseCreator::lambda([&](td::Result<int> result) {
    ASSERT_TRUE(result.is_ok());
    ASSERT_EQ(result.ok(), 7);
    ++completions;
  }));
  callbacks[0].set_error(td::Status::Error("invalid proof"));
  ASSERT_EQ(completions, 0);
  callbacks[2].set_value(7);
  callbacks[1].set_value(8);
  ASSERT_EQ(completions, 1);
}

TEST(DownloadRace, FailsOnlyAfterAllSourcesFail) {
  int completions = 0;
  auto callbacks = download_race_promises<int>(2, td::PromiseCreator::lambda([&](td::Result<int> result) {
    ASSERT_TRUE(result.is_error());
    ++completions;
  }));
  callbacks[0].set_error(td::Status::Error("not found"));
  ASSERT_EQ(completions, 0);
  callbacks[1].set_error(td::Status::Error("timeout"));
  ASSERT_EQ(completions, 1);
}
