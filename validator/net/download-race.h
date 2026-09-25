// SPDX-License-Identifier: LGPL-2.0-or-later
#pragma once
#include <memory>
#include <mutex>
#include <vector>
#include "td/actor/PromiseFuture.h"
#include "td/utils/Time.h"

namespace ton::validator::fullnode {

// Every callback can arrive on a different actor thread. Complete once on the
// first success, or only after every source failed. Never run a callback locked.
template <class T>
std::vector<td::Promise<T>> download_race_promises(std::size_t count, td::Promise<T> promise) {
  struct State {
    std::mutex mutex;
    std::size_t pending;
    td::Promise<T> promise;
    State(std::size_t n, td::Promise<T> p) : pending(n), promise(std::move(p)) {}
  };
  CHECK(count > 0);
  auto state = std::make_shared<State>(count, std::move(promise));
  std::vector<td::Promise<T>> callbacks;
  for (std::size_t i = 0; i < count; ++i) {
    callbacks.push_back(td::PromiseCreator::lambda([state](td::Result<T> result) mutable {
      td::Promise<T> done;
      {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->promise) return;
        --state->pending;
        if (result.is_ok() || state->pending == 0) done = std::move(state->promise);
      }
      if (done) done.set_result(std::move(result));
    }));
  }
  return callbacks;
}

// Dispatch both paths while the original deadline is still usable. A stalled
// custom path must not consume the public path's entire deadline.
template <class T, class Custom, class Public>
void start_download_race(td::Timestamp deadline, td::Promise<T> promise, Custom custom, Public public_download) {
  auto callbacks = download_race_promises<T>(2, std::move(promise));
  custom(deadline, std::move(callbacks[0]));
  public_download(deadline, std::move(callbacks[1]));
}

}  // namespace ton::validator::fullnode
