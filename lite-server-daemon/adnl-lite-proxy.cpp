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
#include "block/block-auto.h"
#include "vm/dict.h"
#include "vm/cells/MerkleProof.h"
#include "vm/vm.h"
#include "td/utils/Slice.h"
#include "td/utils/common.h"
#include "td/utils/OptionParser.h"
#include "td/utils/port/user.h"
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

namespace ton::liteserver {
class LiteProxy : public td::actor::Actor {
 public:
  LiteProxy(std::string config_path, std::string db_path, std::string address, std::string global_config) {
    config_path_ = std::move(config_path);
    db_root_ = std::move(db_path);
    address_.init_host_port(std::move(address)).ensure();
    global_config_ = std::move(global_config);
  }

  void start_up() override {
    LOG(INFO) << "Start LiteProxy";
    adnl_network_manager_ = adnl::AdnlNetworkManager::create(static_cast<td::uint16>(address_.get_port()));
    keyring_ = ton::keyring::Keyring::create(db_root_ + "/keyring");
    adnl_ = adnl::Adnl::create("", keyring_.get());
    td::actor::send_closure(adnl_, &adnl::Adnl::register_network_manager, adnl_network_manager_.get());
    adnl::AdnlCategoryMask cat_mask;
    cat_mask[0] = true;
    td::actor::send_closure(adnl_network_manager_, &adnl::AdnlNetworkManager::add_self_addr, address_,
                            std::move(cat_mask), 0);

    ratelimitdb = std::make_shared<td::RocksDb>(
        td::RocksDb::open(db_root_ + "/" + config_.overlay_prefix + "rate-limits/", true).move_as_ok());

    load_config();
  }

  td::Status init_dht() {
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
    TRY_RESULT_PREFIX(dht, ton::dht::Dht::create_global_config(std::move(conf.dht_)), "bad [dht] section: ");

    auto dht_config = std::move(dht);

    td::mkdir(db_root_ + "/lite-proxy").ensure();

    // Start DHT
    for (auto &dht : config_.dht_ids) {
      auto D = ton::dht::Dht::create(ton::adnl::AdnlNodeIdShort{dht}, db_root_ + "/lite-proxy", dht_config,
                                     keyring_.get(), adnl_.get());
      D.ensure();
      adnl::AdnlNodeIdFull local_id_full = adnl::AdnlNodeIdFull::create(keys_[dht].tl()).move_as_ok();
      td::actor::send_closure(adnl_, &ton::adnl::Adnl::add_id, std::move(local_id_full), ton::adnl::AdnlAddressList{},
                              static_cast<td::uint8>(255));

      dht_nodes_[dht] = D.move_as_ok();
      if (default_dht_node_.is_zero()) {
        default_dht_node_ = dht;
      }
    }

    if (default_dht_node_.is_zero()) {
      LOG(ERROR) << "Config broken, no DHT";
      std::_Exit(2);
    }

    td::actor::send_closure(adnl_, &ton::adnl::Adnl::add_static_nodes_from_config, std::move(adnl_static_nodes_));
    return td::Status::OK();
  }

  void init_network() {
    init_dht().ensure();
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
                            std::vector<td::uint16>{}, std::move(Q));
  }

  void created_ext_server(td::actor::ActorOwn<adnl::AdnlExtServer> server) {
    LOG(INFO) << "Server started";
    lite_proxy_ = std::move(server);
    init_users();
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

  void check_ext_query(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data,
                       td::Promise<td::BufferSlice> promise) {
    LOG(WARNING) << "New proxy query: " << dst.bits256_value().to_hex() << " size: " << data.size();
    promise.set_error(td::Status::Error("Ok"));
  }

  void alarm() override {
    alarm_timestamp() = td::Timestamp::in(5.0);
    init_users();  // update users if needed
  }

  void add_ext_server_id(adnl::AdnlNodeIdShort id) {
    class Cb : public adnl::Adnl::Callback {
     private:
      td::actor::ActorId<LiteProxy> id_;

      void receive_message(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data) override {
      }
      void receive_query(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data,
                         td::Promise<td::BufferSlice> promise) override {
        td::actor::send_closure(id_, &LiteProxy::check_ext_query, src, dst, std::move(data), std::move(promise));
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
      LOG(WARNING) << "ADNL available on: " << config_.addr_;

      for (auto &t : config_.adnl_ids) {
        LOG(WARNING) << "ADNL pub: " << keys_[t.first].ed25519_value().raw().to_hex();
      }

      td::actor::send_closure(actor_id(this), &LiteProxy::init_network);
    }
  }

  void load_config() {
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
  std::string db_root_;
  std::string config_path_;
  std::string global_config_;
  ton::adnl::AdnlNodesList adnl_static_nodes_;
  ton::liteserver::Config config_;
  int to_load_keys;
  std::map<ton::PublicKeyHash, ton::PublicKey> keys_;
  td::actor::ActorOwn<keyring::Keyring> keyring_;
  std::vector<adnl::AdnlNodeIdShort> existing;
  td::actor::ActorOwn<adnl::Adnl> adnl_;
  td::actor::ActorOwn<adnl::AdnlNetworkManager> adnl_network_manager_;
  td::IPAddress address_;
  std::shared_ptr<td::RocksDb> ratelimitdb;
  td::actor::ActorOwn<adnl::AdnlExtServer> lite_proxy_;
  std::map<ton::PublicKeyHash, td::actor::ActorOwn<ton::dht::Dht>> dht_nodes_;
  ton::PublicKeyHash default_dht_node_ = ton::PublicKeyHash::zero();
};
}  // namespace ton::liteserver

int main(int argc, char **argv) {
  SET_VERBOSITY_LEVEL(verbosity_DEBUG);

  td::OptionParser p;
  std::string config_path;
  std::string db_path;
  std::string global_config;
  std::string address;
  td::uint32 threads = 7;
  int verbosity = 0;

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
  p.add_option('S', "server-config", "liteserver config path", [&](td::Slice fname) { config_path = fname.str(); });
  p.add_option('D', "db", "db path (for keyring)", [&](td::Slice fname) { db_path = fname.str(); });
  p.add_option('C', "config", "global config", [&](td::Slice fname) { global_config = fname.str(); });
  p.add_option('A', "address", "ip address and port to start on", [&](td::Slice fname) { address = fname.str(); });

  auto S = p.run(argc, argv);
  if (S.is_error()) {
    std::cerr << S.move_as_error().message().str() << std::endl;
    std::_Exit(2);
  }

  td::actor::set_debug(true);
  td::actor::Scheduler scheduler({threads});
  scheduler.run_in_context([&] {
    td::actor::create_actor<ton::liteserver::LiteProxy>("LiteProxy", std::move(config_path), std::move(db_path),
                                                        std::move(address), std::move(global_config))
        .release();
    return td::Status::OK();
  });

  scheduler.run();
  return 0;
}