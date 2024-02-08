#include <cstdlib>
#include "lite-server-rate-limiter.h"

namespace ton::liteserver {
void LiteServerLimiter::start_up() {
  LOG(INFO) << "Rate limiter start";
}

void LiteServerLimiter::recv_connection(
    adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data, td::Promise<td::BufferSlice> promise,
    td::Promise<std::tuple<td::BufferSlice, td::Promise<td::BufferSlice>, td::uint8>> P) {
  LOG(INFO) << "Got connection from: " << dst;

  // All ok
  P.set_value(std::make_tuple(std::move(data), std::move(promise), 0));
};
};  // namespace ton::liteserver