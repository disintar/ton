#ifndef TON_LSLIMITER
#define TON_LSLIMITER

#include <cstdlib>
#include "td/utils/logging.h"
#include "td/actor/actor.h"
#include "td/utils/Time.h"
#include "adnl/utils.hpp"
#include "tuple"

namespace ton::liteserver {

class LiteServerLimiter : public td::actor::Actor {
 private:
  std::string db_root_;

 public:
  LiteServerLimiter(std::string db_root) {
    db_root_ = std::move(db_root);
  }

  void start_up() override;
  void recv_connection(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data,
                       td::Promise<td::BufferSlice> promise,
                       td::Promise<std::tuple<td::BufferSlice, td::Promise<td::BufferSlice>, td::uint8>> P);
};
};  // namespace ton::liteserver

#endif  //TON_LSLIMITER
