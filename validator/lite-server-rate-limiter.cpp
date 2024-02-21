#include <cstdlib>
#include "lite-server-rate-limiter.h"
#include "td/utils/Slice.h"
#include "td/utils/common.h"
#include "td/utils/crypto.h"
#include "td/utils/overloaded.h"
#include "auto/tl/lite_api.h"
#include "auto/tl/ton_api.h"
#include "auto/tl/lite_api.hpp"
#include "tl-utils/lite-utils.hpp"
#include <ctime>

namespace ton::liteserver {
void LiteServerLimiter::start_up() {
  LOG(INFO) << "Rate limiter start";
  alarm();
}

void LiteServerLimiter::set_validator_manager(
    td::actor::ActorId<ton::validator::ValidatorManagerInterface> validator_manager) {
  validator_manager_ = std::move(validator_manager);
  inited = true;

  auto t = std::time(nullptr);

  if (ratelimitdb->count("users").move_as_ok() > 0) {
    std::string utmp;
    ratelimitdb->get("users", utmp);

    auto users = fetch_tl_object<ton::ton_api::storage_liteserver_users>(td::Slice{utmp}, true).move_as_ok();
    LOG(INFO) << "Known users: " << users->values_.size();

    for (td::Bits256 pubkey : users->values_) {
      users_.push_back(pubkey);
      LOG(INFO) << "Loading user: " << pubkey.to_hex();

      std::string tmp;
      ratelimitdb->get(pubkey.as_slice(), tmp);

      auto f = fetch_tl_object<ton::ton_api::storage_liteserver_user>(td::Slice{tmp}, true);
      if (f.is_error()) {
        LOG(ERROR) << "Broken db on user: " << pubkey;
        continue;
      } else {
        auto user_data = f.move_as_ok();
        auto key = adnl::AdnlNodeIdFull{ton::pubkeys::Ed25519(pubkey)};

        if (t <= user_data->valid_until_) {
          if (limits.find(key.compute_short_id()) == limits.end()) {
            LOG(INFO) << "Add key from DB: " << key.pubkey().ed25519_value().raw().to_hex()
                      << ", valid until: " << user_data->valid_until_ << ", ratelimit " << user_data->ratelimit_;

            limits[key.compute_short_id()] = std::make_tuple(user_data->valid_until_, user_data->ratelimit_);
            td::actor::send_closure(validator_manager_, &ton::validator::ValidatorManagerInterface::add_ext_server_id,
                                    key.compute_short_id());
            td::actor::send_closure(adnl_, &ton::adnl::Adnl::add_id, std::move(key), ton::adnl::AdnlAddressList{},
                                    static_cast<td::uint8>(255));
          } else {
            LOG(INFO) << "Liteserver key: " << key.pubkey().ed25519_value().raw().to_hex() << " duplicated";
          }
        } else {
          LOG(INFO) << "Liteserver key: " << key.pubkey().ed25519_value().raw().to_hex() << " expired";
        }
      }
    }
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

  lite_api::downcast_call(
      *pf, td::overloaded(
               [&](lite_api::liteServer_addUser& q) {
                 this->process_add_user(q.key_, q.valid_until_, q.ratelimit_, std::move(promise));
               },
               [&](lite_api::liteServer_getStatData& q) { this->process_get_stat_data(std::move(promise)); },
               [&](auto& obj) { promise.set_error(td::Status::Error("admin function not found")); }));

  P.set_value(std::make_tuple(td::BufferSlice{}, std::move(promise), StatusCode::PROCESSED));
}

void LiteServerLimiter::process_get_stat_data(td::Promise<td::BufferSlice> promise) {
  std::vector<std::unique_ptr<ton::lite_api::liteServer_statItem>> tmp;
  tmp.reserve(stats_data_.size());

  for (auto e : stats_data_) {
    tmp.emplace_back(e.serialize());
  }
  stats_data_.clear();

  promise.set_value(create_serialize_tl_object<ton::lite_api::liteServer_stats>(std::move(tmp)));
}

void LiteServerLimiter::process_add_user(td::Bits256 private_key, td::int64 valid_until, td::int32 ratelimit,
                                         td::Promise<td::BufferSlice> promise) {
  auto pk = ton::PrivateKey{ton::privkeys::Ed25519{private_key}};
  auto id = pk.compute_short_id();
  auto adnlkey = adnl::AdnlNodeIdFull{pk.compute_public_key()};
  auto pubk = adnlkey.pubkey().ed25519_value().raw();

  LOG(WARNING) << "Add user: " << pubk.to_hex() << " valid until: " << valid_until << " ratelimit: " << ratelimit;
  limits[adnlkey.compute_short_id()] = std::make_tuple(valid_until, ratelimit);

  if (!(limits.find(adnlkey.compute_short_id()) != limits.end())) {
    // if not extending existing user
    td::actor::send_closure(keyring_, &keyring::Keyring::add_key, std::move(pk), false, [](td::Unit) {});
    users_.push_back(pubk);

    std::vector<td::Bits256> users(users_);
    ratelimitdb->set("users", create_serialize_tl_object<ton::ton_api::storage_liteserver_users>(std::move(users)));

    auto k = adnlkey.pubkey().ed25519_value().raw();
    auto k_short = adnlkey.compute_short_id().bits256_value();

    td::actor::send_closure(validator_manager_, &ton::validator::ValidatorManagerInterface::add_ext_server_id,
                            adnlkey.compute_short_id());
    td::actor::send_closure(adnl_, &ton::adnl::Adnl::add_id, std::move(adnlkey), ton::adnl::AdnlAddressList{},
                            static_cast<td::uint8>(255));
  }

  ratelimitdb->set(pubk.as_slice(),
                   create_serialize_tl_object<ton::ton_api::storage_liteserver_user>(valid_until, ratelimit));
  ratelimitdb->flush();

  promise.set_value(create_serialize_tl_object<ton::lite_api::liteServer_newUser>(k, k_short));
}

void LiteServerLimiter::alarm() {
  alarm_timestamp() = td::Timestamp::in(1.0);
  usage.clear();

  if (stats_data_.size() > 1000) {  // move to const?
    stats_data_.clear();
    LOG(ERROR) << "Stat cache too large, clear";
  }
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
    usage[dst]++;
    auto k = limits[dst];
    if (usage[dst] > std::get<1>(k) || std::time(nullptr) > std::get<0>(k)) {
      P.set_value(std::make_tuple(std::move(data), std::move(promise), StatusCode::RATELIMIT));
      return;
    }
  }

  // All ok
  P.set_value(std::make_tuple(std::move(data), std::move(promise), StatusCode::OK));
};
};  // namespace ton::liteserver