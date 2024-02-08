#include <cstdlib>
#include "lite-server-rate-limiter.h"
#include "td/utils/Slice.h"
#include "td/utils/common.h"
#include "td/utils/crypto.h"
#include "td/utils/overloaded.h"
#include "auto/tl/lite_api.h"
#include "auto/tl/lite_api.hpp"
#include "tl-utils/lite-utils.hpp"

namespace ton::liteserver {
void LiteServerLimiter::start_up() {
  LOG(INFO) << "Rate limiter start";

  for (auto k : ratelimitdb->get_all_keys().move_as_ok()) {
    // add existing ID to ADNL
  }
}

void LiteServerLimiter::process_admin_request(
    td::BufferSlice query, td::Promise<td::BufferSlice> promise,
    td::Promise<std::tuple<td::BufferSlice, td::Promise<td::BufferSlice>, td::uint8>> P) {
  auto F = fetch_tl_object<ton::lite_api::Function>(query, true);
  if (F.is_error()) {
    promise.set_error(td::Status::Error("function is not valid"));
    P.set_value(std::make_tuple(td::BufferSlice{}, std::move(promise), StatusCode::PROCESSED));
    return;
  }
  auto pf = F.move_as_ok();

  lite_api::downcast_call(*pf,
                          td::overloaded(
                              [&](lite_api::liteServer_addUser& q) {
                                this->process_add_user(q.pubkey_, q.valid_until_, q.ratelimit_, std::move(promise));
                              },
                              [&](auto& obj) { promise.set_error(td::Status::Error("admin function not found")); }));

  P.set_value(std::make_tuple(td::BufferSlice{}, std::move(promise), StatusCode::PROCESSED));
}

void LiteServerLimiter::process_add_user(td::Bits256 pubkey, td::int64 valid_until, td::int32 ratelimit,
                                         td::Promise<td::BufferSlice> promise) {
  LOG(WARNING) << "Add user: " << pubkey.to_hex() << " valid until: " << valid_until << " ratelimit: " << ratelimit;
  promise.set_value(create_serialize_tl_object<ton::lite_api::liteServer_success>(0));
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

    auto E = fetch_tl_prefix<lite_api::liteServer_adminQuery>(data, true);
    if (E.is_ok()) {
      process_admin_request(std::move(E.move_as_ok()->data_), std::move(promise), std::move(P));
      return;
    }
    // process ordinary lite query from admin
  } else {
    // Check key in hot cache
    //    auto key = dst.pubkey_hash().bits256_value().to_hex();
    //
    //    auto tmpc = ratelimitdb->count(key);
    //    if (tmpc.is_error()) {
    //      P.set_value(std::make_tuple(std::move(data), std::move(promise), StatusCode::NOTREADY));
    //      return;
    //    } else {
    //      auto res = tmpc.move_as_ok();
    //      LOG(INFO) << "Rate limit: " << res;
    //    }
  }

  // All ok
  P.set_value(std::make_tuple(std::move(data), std::move(promise), StatusCode::OK));
};
};  // namespace ton::liteserver