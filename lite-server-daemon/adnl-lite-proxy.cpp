#include <cstdlib>
#include "td/utils/logging.h"
#include "td/utils/port/path.h"
#include "td/actor/actor.h"
#include "td/utils/Time.h"
#include "td/utils/filesystem.h"
#include "td/utils/JsonBuilder.h"
#include "auto/tl/ton_api_json.h"
#include "crypto/vm/cp0.h"
#include "validator/validator.h"
#include "validator/manager-disk.h"
#include "ton/ton-types.h"
#include "ton/ton-tl.hpp"
#include "tl/tlblib.hpp"
#include "block/block.h"
#include "block/block-parse.h"
#include "common/delay.h"
#include "block/block-auto.h"
#include "vm/dict.h"
#include "vm/cells/MerkleProof.h"
#include "vm/vm.h"
#include "td/utils/Slice.h"
#include "td/utils/common.h"
#include "td/utils/Random.h"
#include "td/utils/OptionParser.h"
#include "td/utils/port/user.h"
#include "auto/tl/lite_api.h"
#include "auto/tl/ton_api.h"
#include "auto/tl/lite_api.hpp"
#include "tl-utils/lite-utils.hpp"
#include "td/utils/date.h"
#include <utility>
#include <fstream>
#include "auto/tl/lite_api.h"
#include "adnl/utils.hpp"
#include "tuple"
#include "crypto/block/mc-config.h"
#include "lite-server-config.hpp"
#include <algorithm>
#include <queue>
#include <chrono>
#include <thread>

using ValidUntil = td::int64;
using RateLimit = td::int32;

std::string time_to_human(unsigned ts) {  // todo: move from explorer
  td::StringBuilder sb;
  sb << date::format("%F %T",
                     std::chrono::time_point<std::chrono::system_clock, std::chrono::seconds>{std::chrono::seconds(ts)})
     << ", ";
  auto now = (unsigned)td::Clocks::system();
  bool past = now >= ts;
  unsigned x = past ? now - ts : ts - now;
  if (!past) {
    sb << "in ";
  }
  if (x < 60) {
    sb << x << "s";
  } else if (x < 3600) {
    sb << x / 60 << "m " << x % 60 << "s";
  } else if (x < 3600 * 24) {
    x /= 60;
    sb << x / 60 << "h " << x % 60 << "m";
  } else {
    x /= 3600;
    sb << x / 24 << "d " << x % 24 << "h";
  }
  if (past) {
    sb << " ago";
  }
  return sb.as_cslice().str();
}

namespace ton::liteserver {
class LiteClientFire : public td::actor::Actor {
 public:
  class Callback {
   public:
    virtual void refire(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data,
                        td::Promise<td::BufferSlice> promise) = 0;
    virtual ~Callback() = default;
  };

  LiteClientFire(int to_wait, adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice req,
                 td::Promise<td::BufferSlice> promise, std::unique_ptr<LiteClientFire::Callback> callback) {
    to_wait_ = to_wait;
    src_ = std::move(src);
    dst_ = std::move(dst);
    request_ = std::move(req);
    promise_ = std::move(promise);
    callback_ = std::move(callback);
  }

  void start_up() override {
  }

  void receive_answer(td::Result<td::BufferSlice> res, adnl::AdnlNodeIdShort server) {
    to_wait_ -= 1;
    if ((to_wait_ == 0 || res.is_ok()) && !done) {
      if (res.is_ok()) {
        auto data = res.move_as_ok();

        auto E = fetch_tl_object<lite_api::liteServer_error>(data.clone(), true);
        if (E.is_ok() && to_wait_ > 0) {
          LOG(INFO) << "Receive answer from: " << server.bits256_value().to_hex() << " adnl ok: " << res.is_ok()
                    << " error: true, to_wait: " << to_wait_;

          prev_success_ = std::move(data);
          has_prev = true;
          return;
        } else if (E.is_error()) {
          LOG(INFO) << "Receive answer from: " << server.bits256_value().to_hex() << " adnl ok: " << res.is_ok()
                    << " error: false, to_wait: " << to_wait_;
          promise_.set_value(std::move(data));
          done = true;
        } else {
          if (E.is_ok()) {
            auto x = E.move_as_ok()->message_;
            // readonly have problems on sync cells, this is govnokod, sorry.
            // I have not much time and knowledge to fix it other way.
            std::vector<std::string> whitelist_for_refire{"not found", "get account state"};
            for (auto &s : whitelist_for_refire) {
              if (x.find(s) != std::string::npos) {
                // allow refire
                LOG(INFO) << "Refire with: " << s;
                callback_->refire(std::move(src_), std::move(dst_), std::move(request_), std::move(promise_));
                stop();
                return;
              }
            }
          }
          res = td::Result<td::BufferSlice>{std::move(data)};
        }
      }
    } else {
      LOG(INFO) << "Receive answer from: " << server.bits256_value().to_hex() << " adnl ok: " << res.is_ok();
    }

    if (to_wait_ == 0) {
      LOG(ERROR) << "Got last answer, stop, done: " << done;

      if (!done) {
        if (res.is_error() && has_prev) {
          LOG(INFO) << "Send prev error";
          promise_.set_result(std::move(prev_success_));
        } else {
          LOG(INFO) << "Send cur error";
          promise_.set_result(std::move(res));
        }
      }
      stop();
    }
  }

 private:
  int to_wait_;
  bool done{false};
  bool has_prev{false};
  td::BufferSlice prev_success_;
  adnl::AdnlNodeIdShort src_;
  adnl::AdnlNodeIdShort dst_;
  td::BufferSlice request_;
  td::Promise<td::BufferSlice> promise_;
  std::unique_ptr<LiteClientFire::Callback> callback_;
};

class LiteServerClient : public td::actor::Actor {
 public:
  LiteServerClient(td::IPAddress address, ton::adnl::AdnlNodeIdFull id,
                   std::unique_ptr<ton::adnl::AdnlExtClient::Callback> callback) {
    address_ = std::move(address);
    id_ = std::move(id);
    client_ = ton::adnl::AdnlExtClient::create(id_, address_, std::move(callback));
    td::actor::send_closure(client_, &ton::adnl::AdnlExtClient::set_next_alarm, 1);
  }

  void send_raw(td::BufferSlice q, td::Promise<td::BufferSlice> promise) {
    td::actor::send_closure(client_, &ton::adnl::AdnlExtClient::send_query, "query", std::move(q),
                            td::Timestamp::in(2.0), std::move(promise));
  }

  void fire(ton::adnl::AdnlNodeIdFull key, td::BufferSlice q, td::Promise<td::BufferSlice> promise) {
    send_raw(std::move(q), std::move(promise));
  }

  void get_max_time(td::Promise<int> promise) {
    auto P = td::PromiseCreator::lambda([Pp = std::move(promise)](td::Result<td::BufferSlice> R) mutable {
      if (R.is_ok()) {
        auto answer = ton::fetch_tl_object<ton::lite_api::liteServer_masterchainInfoExt>(R.move_as_ok(), true);

        if (answer.is_error()) {
          Pp.set_value(0);
          return;
        }

        auto x = answer.move_as_ok();
        Pp.set_value((int)x->last_utime_);
      } else {
        Pp.set_value(0);
      }
    });

    qprocess(ton::serialize_tl_object(ton::create_tl_object<ton::lite_api::liteServer_getMasterchainInfoExt>(0), true),
             std::move(P));
  }

 private:
  void qprocess(td::BufferSlice q, td::Promise<td::BufferSlice> promise) {
    auto P = td::PromiseCreator::lambda(
        [Pp = std::move(promise)](td::Result<td::BufferSlice> R) mutable { Pp.set_result(std::move(R)); });

    send_raw(ton::serialize_tl_object(ton::create_tl_object<ton::lite_api::liteServer_query>(std::move(q)), true),
             std::move(P));
  }
  td::IPAddress address_;
  ton::adnl::AdnlNodeIdFull id_;
  td::actor::ActorOwn<ton::adnl::AdnlExtClient> client_;
};

class LiteProxy : public td::actor::Actor {
 public:
  LiteProxy(std::string config_path, std::string db_path, std::string address, td::uint32 lite_port,
            td::uint32 adnl_port, std::string global_config, int mode) {
    config_path_ = std::move(config_path);
    db_root_ = std::move(db_path);
    address_.init_host_port(address, lite_port).ensure();
    adnl_address_.init_host_port(std::move(address), adnl_port).ensure();
    global_config_ = std::move(global_config);
    mode_ = mode;
  }

  std::unique_ptr<ton::adnl::AdnlExtClient::Callback> make_callback(adnl::AdnlNodeIdShort server) {
    class Callback : public ton::adnl::AdnlExtClient::Callback {
     public:
      void on_ready() override {
        td::actor::send_closure(id_, &LiteProxy::conn_ready, server_);
      }
      void on_stop_ready() override {
        td::actor::send_closure(id_, &LiteProxy::conn_closed, server_);
      }
      Callback(td::actor::ActorId<LiteProxy> id, adnl::AdnlNodeIdShort server)
          : id_(std::move(id)), server_(std::move(server)) {
      }

     private:
      td::actor::ActorId<LiteProxy> id_;
      adnl::AdnlNodeIdShort server_;
    };
    return std::make_unique<Callback>(actor_id(this), std::move(server));
  }

  void conn_ready(adnl::AdnlNodeIdShort server) {
    LOG(INFO) << "Server: " << server.bits256_value().to_hex() << " now available";
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), server](td::Result<int> R) mutable {
      td::actor::send_closure(SelfId, &LiteProxy::server_update_time, server, std::move(R.move_as_ok()));
    });

    td::actor::send_closure(private_servers_[server].get(), &LiteServerClient::get_max_time, std::move(P));
  }

  void server_update_time(adnl::AdnlNodeIdShort server, int time) {
    private_time_updated++;
    private_servers_status_[std::move(server)] = time;

    if (private_time_updated >= (int)private_servers_status_.size() * 10) {  // every 10sec
      auto t = std::time(nullptr);
      std::vector<adnl::AdnlNodeIdShort> uptodate;
      int outdated{0};
      int best_time{0};
      adnl::AdnlNodeIdShort best;

      for (auto &s : private_servers_status_) {
        if (s.second > best_time) {
          best_time = s.second;
          best = s.first;
        }

        auto pos = std::find(uptodate_private_ls.begin(), uptodate_private_ls.end(), s.first);

        if (t - s.second > 30) {
          outdated += 1;

          if (pos != uptodate_private_ls.end()) {
            uptodate_private_ls.erase(pos);
          }
        } else {
          if (pos == uptodate_private_ls.end()) {
            uptodate_private_ls.push_back(s.first);
          }

          uptodate.push_back(s.first);
        }
      }

      LOG(INFO) << "Private LiteServers stats: uptodate: " << uptodate.size() << " outdated: " << outdated
                << " best time: " << time_to_human(best_time) << ", best server: " << best.bits256_value().to_hex();
      uptodate_private_ls = std::move(uptodate);
      private_time_updated = 0;
    }
  }

  void conn_closed(adnl::AdnlNodeIdShort server) {
    LOG(INFO) << "Server: " << server.bits256_value().to_hex() << " disconnected";
    private_servers_status_.erase(server);

    auto pos = std::find(uptodate_private_ls.begin(), uptodate_private_ls.end(), server);
    if (pos != uptodate_private_ls.end()) {
      uptodate_private_ls.erase(pos);
    }
  }

  void start_up() override {
    LOG(INFO) << "Start LiteProxy: " << address_;
    LOG(INFO) << "Start ADNL: " << adnl_address_;

    adnl_network_manager_ = adnl::AdnlNetworkManager::create((td::uint16)adnl_address_.get_port());
    keyring_ = ton::keyring::Keyring::create(db_root_ + "/keyring");
    adnl_ = adnl::Adnl::create("", keyring_.get());
    td::actor::send_closure(adnl_, &adnl::Adnl::register_network_manager, adnl_network_manager_.get());
    adnl::AdnlCategoryMask cat_mask;
    cat_mask[0] = true;
    td::actor::send_closure(adnl_network_manager_, &adnl::AdnlNetworkManager::add_self_addr, adnl_address_,
                            std::move(cat_mask), 0);

    load_config();
  }

  td::Status init_dht() {
    LOG(INFO) << "Init dht";
    TRY_RESULT_PREFIX(conf_data, td::read_file(global_config_), "failed to read: ");
    TRY_RESULT_PREFIX(conf_json, td::json_decode(conf_data.as_slice()), "failed to parse json: ");

    ton::ton_api::config_global conf;
    TRY_STATUS_PREFIX(ton::ton_api::from_json(conf, conf_json.get_object()), "json does not fit TL scheme: ");

    if (conf.adnl_) {
      if (conf.adnl_->static_nodes_) {
        TRY_RESULT_PREFIX_ASSIGN(adnl_static_nodes_, ton::adnl::AdnlNodesList::create(conf.adnl_->static_nodes_),
                                 "bad static adnl nodes: ");
      }
    }
    td::actor::send_closure(adnl_, &ton::adnl::Adnl::add_static_nodes_from_config, std::move(adnl_static_nodes_));

    TRY_RESULT_PREFIX(dht, ton::dht::Dht::create_global_config(std::move(conf.dht_)), "bad [dht] section: ");
    auto dht_config = std::move(dht);

    td::mkdir(db_root_ + "/lite-proxy").ensure();

    adnl::AdnlAddressList addr_list;
    addr_list.add_udp_address(adnl_address_).ensure();
    addr_list.set_version(static_cast<td::int32>(td::Clocks::system()));
    addr_list.set_reinit_date(adnl::Adnl::adnl_start_time());

    // Start DHT
    for (auto &dht : config_.dht_ids) {
      adnl::AdnlNodeIdFull local_id_full = adnl::AdnlNodeIdFull::create(keys_[dht].tl()).move_as_ok();

      td::actor::send_closure(adnl_, &ton::adnl::Adnl::add_id, std::move(local_id_full), addr_list,
                              static_cast<td::uint8>(0));

      LOG(INFO) << "Start dht with: " << local_id_full.compute_short_id().bits256_value().to_hex();
      auto D = ton::dht::Dht::create(local_id_full.compute_short_id(), db_root_ + "/lite-proxy", dht_config,
                                     keyring_.get(), adnl_.get());
      D.ensure();

      dht_nodes_[dht] = D.move_as_ok();
      if (default_dht_node_.is_zero()) {
        default_dht_node_ = dht;
      }
    }

    if (default_dht_node_.is_zero()) {
      LOG(ERROR) << "Config broken, no DHT";
      std::_Exit(2);
    }

    return td::Status::OK();
  }

  void init_network() {
    init_dht().ensure();

    LOG(INFO) << "Add dht node";
    td::actor::send_closure(adnl_, &ton::adnl::Adnl::register_dht_node, dht_nodes_[default_dht_node_].get());

    start_server();
  }

  void start_server() {
    auto Q =
        td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::actor::ActorOwn<adnl::AdnlExtServer>> R) {
          R.ensure();
          td::actor::send_closure(SelfId, &LiteProxy::created_ext_server, R.move_as_ok());
        });

    LOG(INFO) << "Start server";
    td::actor::send_closure(adnl_, &adnl::Adnl::create_ext_server, std::vector<adnl::AdnlNodeIdShort>{},
                            std::vector<td::uint16>{(td::uint16)address_.get_port()}, std::move(Q));
  }

  void created_ext_server(td::actor::ActorOwn<adnl::AdnlExtServer> server) {
    LOG(INFO) << "Server started";
    lite_proxy_ = std::move(server);
    td::actor::send_closure(actor_id(this), &LiteProxy::init_users);
    td::actor::send_closure(actor_id(this), &LiteProxy::init_private_servers);
  }

  void init_private_servers() {
    LOG(INFO) << "Start init private servers";

    // get current lc
    auto my_address = config_.addr_;
    for (auto &s : config_.liteservers) {
      if (keys_.find(s.second) == keys_.end()) {
        LOG(ERROR) << "Can't find key: " << s.second.bits256_value().to_hex();
      } else {
        auto key = ton::adnl::AdnlNodeIdFull{keys_[s.second]};
        td::IPAddress lc_address;
        lc_address.init_host_port(td::IPAddress::ipv4_to_str(my_address.get_ipv4()), s.first).ensure();

        private_servers_[key.compute_short_id()] = td::actor::create_actor<ton::liteserver::LiteServerClient>(
            "LSC " + key.compute_short_id().bits256_value().to_hex(), std::move(lc_address), std::move(key),
            make_callback(key.compute_short_id()));
      }
    }

    // get slaves
    for (auto &s : config_.liteslaves) {
      auto key = adnl::AdnlNodeIdFull{std::move(s.key)};
      td::IPAddress lc_address = s.addr;

      private_servers_[key.compute_short_id()] = td::actor::create_actor<ton::liteserver::LiteServerClient>(
          "LSC " + key.compute_short_id().bits256_value().to_hex(), std::move(lc_address), std::move(key),
          make_callback(key.compute_short_id()));
    }
  }

  void init_users() {
    if (ratelimitdb->count("users").move_as_ok() > 0) {
      std::string utmp;
      ratelimitdb->get("users", utmp);

      auto t = std::time(nullptr);

      auto users = fetch_tl_object<ton::ton_api::storage_liteserver_users>(td::Slice{utmp}, true).move_as_ok();

      for (td::Bits256 pubkey : users->values_) {
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
            if (std::find(existing.begin(), existing.end(), key.compute_short_id()) == existing.end()) {
              LOG(INFO) << "Add key from DB: " << key.pubkey().ed25519_value().raw().to_hex()
                        << ", valid until: " << user_data->valid_until_ << ", ratelimit " << user_data->ratelimit_;

              keys_[key.compute_short_id().pubkey_hash()] = key.pubkey();
              limits[key.compute_short_id()] = std::make_tuple(user_data->valid_until_, user_data->ratelimit_);
              existing.push_back(key.compute_short_id());
              td::actor::send_closure(actor_id(this), &LiteProxy::add_ext_server_id, key.compute_short_id());
              td::actor::send_closure(adnl_, &ton::adnl::Adnl::add_id, std::move(key), ton::adnl::AdnlAddressList{},
                                      static_cast<td::uint8>(255));
            }
          }
        }
      }
    }
  }

  std::unique_ptr<LiteClientFire::Callback> make_refire_callback(int refire) {
    class Callback : public LiteClientFire::Callback {
     public:
      void refire(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data,
                  td::Promise<td::BufferSlice> promise) override {
        delay_action(
            [ProxyId = id_, src = std::move(src), dst = std::move(dst), data = std::move(data),
             promise = std::move(promise), refire = refire_]() mutable {
              td::actor::send_closure(ProxyId, &LiteProxy::check_ext_query, std::move(src), std::move(dst),
                                      std::move(data), std::move(promise), refire + 1);
            },
            td::Timestamp::in(0.1));
      }

      Callback(td::actor::ActorId<LiteProxy> id, int refire) : id_(std::move(id)), refire_(refire) {
      }

     private:
      td::actor::ActorId<LiteProxy> id_;
      adnl::AdnlNodeIdShort server_;
      int refire_;
    };

    return std::make_unique<Callback>(actor_id(this), refire);
  }

  void check_ext_query(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data,
                       td::Promise<td::BufferSlice> promise, int refire = 0) {
    if (refire > 10) {
      LOG(ERROR) << "Too deep refire";
      promise.set_value(create_serialize_tl_object<lite_api::liteServer_error>(228, "Too deep refire"));
      return;
    }

    LOG(INFO) << "Got query to: " << dst;

    if (inited) {
      usage[dst]++;
      auto k = limits[dst];
      if (usage[dst] > std::get<1>(k) || std::time(nullptr) > std::get<0>(k)) {
        LOG(INFO) << "Drop to: " << dst << " because of ratelimit";
        promise.set_value(create_serialize_tl_object<lite_api::liteServer_error>(228, "Ratelimit"));
        return;
      }

      if (mode_ == 0) {
        LOG(WARNING) << "New proxy query: " << dst.bits256_value().to_hex() << " size: " << data.size();

        td::actor::ActorId<LiteServerClient> server;
        if (uptodate_private_ls.size() > 0) {
          auto adnl =
              uptodate_private_ls[td::Random::fast(0, td::narrow_cast<td::uint32>(uptodate_private_ls.size() - 1))];
          server = private_servers_[std::move(adnl)].get();
        } else {
          auto s = td::Random::fast(0, td::narrow_cast<td::uint32>(private_servers_.size() - 1));
          int a{0};
          for (auto &x : private_servers_) {
            a += 1;
            if (a == s) {
              server = x.second.get();
              break;
            }
          }
        }

        td::actor::send_closure(server, &LiteServerClient::send_raw, std::move(data), std::move(promise));
      } else {
        if (uptodate_private_ls.size() > 0) {
          auto waiter = td::actor::create_actor<ton::liteserver::LiteClientFire>(
                            "LSC::Fire", uptodate_private_ls.size(), src, dst, data.clone(), std::move(promise),
                            make_refire_callback(refire))
                            .release();
          for (auto &s : uptodate_private_ls) {
            LOG(INFO) << "[uptodate] Send to: " << dst << " to " << s.bits256_value().to_hex();
            auto P = td::PromiseCreator::lambda([WaiterId = waiter, s](td::Result<td::BufferSlice> R) mutable {
              td::actor::send_closure(WaiterId, &LiteClientFire::receive_answer, std::move(R), s);
            });

            td::actor::send_closure(private_servers_[s].get(), &LiteServerClient::send_raw, data.clone(), std::move(P));
          }
        } else {
          auto waiter = td::actor::create_actor<ton::liteserver::LiteClientFire>(
                            "LSC::Fire", private_servers_.size(), src, dst, data.clone(), std::move(promise),
                            make_refire_callback(refire))
                            .release();

          for (auto &s : private_servers_) {
            LOG(INFO) << "[all] Send: " << dst << " to " << s.first.bits256_value().to_hex();
            auto P =
                td::PromiseCreator::lambda([WaiterId = waiter, s = s.first](td::Result<td::BufferSlice> R) mutable {
                  td::actor::send_closure(WaiterId, &LiteClientFire::receive_answer, std::move(R), s);
                });

            td::actor::send_closure(s.second, &LiteServerClient::send_raw, data.clone(), std::move(P));
          }
        }
      }

    } else {
      promise.set_value(create_serialize_tl_object<lite_api::liteServer_error>(228, "Server not ready"));
    }
  }

  void alarm() override {
    alarm_timestamp() = td::Timestamp::in(1.0);
    usage.clear();
    init_users();  // update users if needed
    update_server_stats();
  }

  void update_server_stats() {
    for (auto &s : private_servers_) {
      auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), server = s.first](td::Result<int> R) mutable {
        td::actor::send_closure(SelfId, &LiteProxy::server_update_time, server, std::move(R.move_as_ok()));
      });

      td::actor::send_closure(s.second, &LiteServerClient::get_max_time, std::move(P));
    }

    if (!inited) {
      inited = true;
    }
  }

  void add_ext_server_id(adnl::AdnlNodeIdShort id) {
    class Cb : public adnl::Adnl::Callback {
     private:
      td::actor::ActorId<LiteProxy> id_;

      void receive_message(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data) override {
      }
      void receive_query(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data,
                         td::Promise<td::BufferSlice> promise) override {
        td::actor::send_closure(id_, &LiteProxy::check_ext_query, src, dst, std::move(data), std::move(promise), 0);
      }

     public:
      Cb(td::actor::ActorId<LiteProxy> id) : id_(std::move(id)) {
      }
    };

    td::actor::send_closure(adnl_, &adnl::Adnl::subscribe, id,
                            adnl::Adnl::int_to_bytestring(lite_api::liteServer_query::ID),
                            std::make_unique<Cb>(actor_id(this)));

    td::actor::send_closure(adnl_, &adnl::Adnl::subscribe, id,
                            adnl::Adnl::int_to_bytestring(lite_api::liteServer_adminQuery::ID),
                            std::make_unique<Cb>(actor_id(this)));

    td::actor::send_closure(lite_proxy_, &adnl::AdnlExtServer::add_local_id, id);
    alarm_timestamp() = td::Timestamp::in(5.0);
  }

  void got_key(ton::PublicKey key) {
    to_load_keys--;
    keys_[key.compute_short_id()] = std::move(key);

    if (to_load_keys == 0) {
      for (auto &t : config_.adnl_ids) {
        LOG(WARNING) << "ADNL pub: " << keys_[t.first].ed25519_value().raw().to_hex();
      }

      td::actor::send_closure(actor_id(this), &LiteProxy::init_network);
    }
  }

  void load_config() {
    LOG(INFO) << "Load config";

    auto conf_data_R = td::read_file(config_path_);
    if (conf_data_R.is_error()) {
      LOG(ERROR) << "Can't read file: " << config_path_;
      std::_Exit(2);
    }
    auto conf_data = conf_data_R.move_as_ok();
    auto conf_json_R = td::json_decode(conf_data.as_slice());
    if (conf_json_R.is_error()) {
      LOG(ERROR) << "Failed to parse json";
      std::_Exit(2);
    }
    auto conf_json = conf_json_R.move_as_ok();

    ton::ton_api::engine_liteserver_config conf;
    auto S = ton::ton_api::from_json(conf, conf_json.get_object());
    if (S.is_error()) {
      LOG(ERROR) << "Json does not fit TL scheme";
      std::_Exit(2);
    }

    config_ = ton::liteserver::Config{conf};

    ratelimitdb = std::make_shared<td::RocksDb>(
        td::RocksDb::open(db_root_ + "/" + config_.overlay_prefix + "rate-limits/", true).move_as_ok());

    for (auto &key : config_.keys_refcnt) {
      to_load_keys++;

      auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<ton::PublicKey> R) mutable {
        if (R.is_error()) {
          LOG(ERROR) << R.move_as_error();
          std::_Exit(2);
        } else {
          td::actor::send_closure(SelfId, &LiteProxy::got_key, R.move_as_ok());
        }
      });

      td::actor::send_closure(keyring_, &ton::keyring::Keyring::add_key_short, key.first, std::move(P));
    }
  }

 private:
  // 0 - random LS
  // 1 - fire to all return 1 success
  int mode_;
  std::string db_root_;
  std::string config_path_;
  std::string global_config_;
  ton::liteserver::Config config_;
  int to_load_keys{0};
  bool inited{false};
  int private_time_updated;
  std::map<ton::PublicKeyHash, ton::PublicKey> keys_;
  td::actor::ActorOwn<keyring::Keyring> keyring_;
  std::vector<adnl::AdnlNodeIdShort> existing;
  td::actor::ActorOwn<adnl::Adnl> adnl_;
  td::actor::ActorOwn<adnl::AdnlNetworkManager> adnl_network_manager_;
  std::map<adnl::AdnlNodeIdShort, int> private_servers_status_;
  std::map<adnl::AdnlNodeIdShort, td::actor::ActorOwn<LiteServerClient>> private_servers_;
  td::IPAddress address_;
  td::IPAddress adnl_address_;
  std::shared_ptr<td::RocksDb> ratelimitdb;
  ton::adnl::AdnlNodesList adnl_static_nodes_;
  td::actor::ActorOwn<adnl::AdnlExtServer> lite_proxy_;
  std::map<ton::PublicKeyHash, td::actor::ActorOwn<ton::dht::Dht>> dht_nodes_;
  std::vector<adnl::AdnlNodeIdShort> uptodate_private_ls{};
  ton::PublicKeyHash default_dht_node_ = ton::PublicKeyHash::zero();
  std::map<ton::adnl::AdnlNodeIdShort, std::tuple<ValidUntil, RateLimit>> limits;
  std::map<ton::adnl::AdnlNodeIdShort, int> usage;
};
}  // namespace ton::liteserver

int main(int argc, char **argv) {
  SET_VERBOSITY_LEVEL(verbosity_DEBUG);

  td::OptionParser p;
  std::string config_path;
  std::string db_path;
  std::string global_config;
  std::string address;
  td::uint32 threads = 20;
  int verbosity = 0;
  int mode = 1;
  td::uint32 lite_port = 0;
  td::uint32 adnl_port = 0;

  p.set_description("lite-proxy");
  p.add_option('h', "help", "prints_help", [&]() {
    char b[10240];
    td::StringBuilder sb(td::MutableSlice{b, 10000});
    sb << p;
    std::cout << sb.as_cslice().c_str();
    std::exit(2);
  });
  p.add_checked_option(
      't', "threads", PSTRING() << "number of threads (default=" << threads << ")", [&](td::Slice arg) {
        td::uint32 v;
        try {
          v = std::stoi(arg.str());
        } catch (...) {
          return td::Status::Error(ton::ErrorCode::error, "bad value for --threads: not a number");
        }
        if (v < 1 || v > 256) {
          return td::Status::Error(ton::ErrorCode::error, "bad value for --threads: should be in range [1..256]");
        }
        threads = v;

        return td::Status::OK();
      });
  p.add_checked_option('v', "verbosity", "set verbosity level", [&](td::Slice arg) {
    verbosity = td::to_integer<int>(arg);
    SET_VERBOSITY_LEVEL(VERBOSITY_NAME(FATAL) + verbosity);
    return (verbosity >= 0 && verbosity <= 9) ? td::Status::OK() : td::Status::Error("verbosity must be 0..9");
  });
  p.add_option('m', "mode", "set mode", [&](td::Slice arg) { mode = td::to_integer<int>(arg); });
  p.add_option('S', "server-config", "liteserver config path", [&](td::Slice fname) { config_path = fname.str(); });
  p.add_option('D', "db", "db path (for keyring)", [&](td::Slice fname) { db_path = fname.str(); });
  p.add_option('C', "config", "global config", [&](td::Slice fname) { global_config = fname.str(); });
  p.add_option('I', "ip", "ip address to start on", [&](td::Slice fname) { address = fname.str(); });
  p.add_option('L', "lite-port", "lite-proxy port", [&](td::Slice arg) { lite_port = td::to_integer<int>(arg); });
  p.add_option('A', "adnl-port", "adnl port", [&](td::Slice arg) { adnl_port = td::to_integer<int>(arg); });

  auto S = p.run(argc, argv);
  if (S.is_error()) {
    std::cerr << S.move_as_error().message().str() << std::endl;
    std::_Exit(2);
  }

  td::actor::set_debug(true);
  td::actor::Scheduler scheduler({threads});
  scheduler.run_in_context([&] {
    td::actor::create_actor<ton::liteserver::LiteProxy>("LiteProxy", std::move(config_path), std::move(db_path),
                                                        std::move(address), lite_port, adnl_port,
                                                        std::move(global_config), mode)
        .release();
    return td::Status::OK();
  });

  scheduler.run();
  return 0;
}