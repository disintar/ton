#include <cstdlib>
#include "lite-server-rate-limiter.h"

namespace ton::liteserver {
void LiteServerLimiter::start_up() {
  LOG(INFO) << "Rate limiter start";
}

void LiteServerLimiter::recv_connection(
    adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data, td::Promise<td::BufferSlice> promise,
    td::Promise<std::tuple<td::BufferSlice, td::Promise<td::BufferSlice>, td::uint8>> P) {
  if (!inited) {
    P.set_value(std::make_tuple(std::move(data), std::move(promise), StatusCode::NOTREADY));
    return;
  }

  if (std::find(admins_.begin(), admins_.end(), dst) != admins_.end()) {
    LOG(INFO) << "Got ADMIN connection from: " << dst;
  } else {
    auto key = dst.pubkey_hash().bits256_value().to_hex();

    auto tmpc = ratelimitdb->count(key);
    if (tmpc.is_error()) {
      P.set_value(std::make_tuple(std::move(data), std::move(promise), StatusCode::NOTREADY));
      return;
    } else {
      auto res = tmpc.move_as_ok();
      LOG(INFO) << "Rate limit: " << res;
    }
  }

  // All ok
  P.set_value(std::make_tuple(std::move(data), std::move(promise), StatusCode::OK));
};
};  // namespace ton::liteserver