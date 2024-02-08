#ifndef TON_LSLIMITER
#define TON_LSLIMITER

#include <cstdlib>
#include "td/utils/logging.h"
#include "td/actor/actor.h"
#include "td/utils/Time.h"
#include "adnl/utils.hpp"
#include "tuple"
#include "td/db/RocksDb.h"
#include "validator/validator.h"
#include <map>

namespace ton::liteserver {

enum StatusCode : td::uint8 {
  OK = 0,
  NOTREADY = 1,
  PROCESSED = 2,  // process request of admin on lslimiter side
  RATELIMIT = 228,
};

using ValidUntil = td::int64;
using RateLimit = td::int32;

class LiteServerLimiter : public td::actor::Actor {
 private:
  std::string db_root_;
  std::shared_ptr<td::RocksDb> ratelimitdb;
  bool inited{false};
  td::actor::ActorId<ton::validator::ValidatorManagerInterface> validator_manager_;
  td::actor::ActorId<adnl::Adnl> adnl_;
  td::actor::ActorId<keyring::Keyring> keyring_;
  std::vector<ton::adnl::AdnlNodeIdShort> admins_;
  std::map<ton::adnl::AdnlNodeIdShort, std::tuple<ValidUntil, RateLimit>> limits;
  std::map<ton::adnl::AdnlNodeIdShort, int> usage;

 public:
  LiteServerLimiter(std::string db_root, td::actor::ActorId<adnl::Adnl> adnl,
                    td::actor::ActorId<keyring::Keyring> keyring) {
    db_root_ = std::move(db_root);
    adnl_ = std::move(adnl);
    ratelimitdb = std::make_shared<td::RocksDb>(td::RocksDb::open(db_root_ + "rate-limits").move_as_ok());
    keyring_ = std::move(keyring);
  }

  void start_up() override;
  void alarm() override;
  void process_admin_request(td::BufferSlice query, td::Promise<td::BufferSlice> promise,
                             td::Promise<std::tuple<td::BufferSlice, td::Promise<td::BufferSlice>, td::uint8>> P);
  void process_add_user(td::Bits256 private_key, td::int64 valid_until, td::int32 ratelimit,
                        td::Promise<td::BufferSlice> promise);
  void continue_process_add_user();
  void set_validator_manager(td::actor::ActorId<ton::validator::ValidatorManagerInterface> validator_manager);

  void add_admin(ton::adnl::AdnlNodeIdShort admin) {
    admins_.push_back(admin);
  }

  void recv_connection(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data,
                       td::Promise<td::BufferSlice> promise,
                       td::Promise<std::tuple<td::BufferSlice, td::Promise<td::BufferSlice>, td::uint8>> P);
};
};  // namespace ton::liteserver

#endif  //TON_LSLIMITER
