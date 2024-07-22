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
#include "td/utils/overloaded.h"
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
#include <cppkafka/cppkafka.h>
#include "blockchain-indexer/json.hpp"
#include <chrono>
#include "tl-utils/lite-utils.hpp"

using ValidUntil = td::int64;
using RateLimit = td::int32;

std::string time_to_human(unsigned ts) {  // todo: move from explorer
    td::StringBuilder sb;
    sb << date::format("%F %T",
                       std::chrono::time_point<std::chrono::system_clock, std::chrono::seconds>{
                               std::chrono::seconds(ts)})
       << ", ";
    auto now = (unsigned) td::Clocks::system();
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
                        LOG(INFO)
                        << "Receive answer from: " << server.bits256_value().to_hex() << " adnl ok: " << res.is_ok()
                        << " error: true, to_wait: " << to_wait_;

                        prev_success_ = std::move(data);
                        has_prev = true;
                        return;
                    } else if (E.is_error()) {
                        LOG(INFO)
                        << "Receive answer from: " << server.bits256_value().to_hex() << " adnl ok: " << res.is_ok()
                        << " error: false, to_wait: " << to_wait_;
                        promise_.set_value(std::move(data));
                        done = true;
                    } else {
                        if (E.is_ok()) {
                            auto x = E.move_as_ok()->message_;
                            // readonly have problems on sync cells, this is govnokod, sorry.
                            // I have not much time and knowledge to fix it other way.
                            std::vector<std::string> whitelist_for_refire{"not found", "get account state"};
                            for (auto &s: whitelist_for_refire) {
                                if (x.find(s) != std::string::npos) {
                                    // allow refire
                                    LOG(INFO) << "Refire with: " << s;
                                    callback_->refire(std::move(src_), std::move(dst_), std::move(request_),
                                                      std::move(promise_));
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

        void send_raw(td::BufferSlice q, td::Promise<td::BufferSlice> promise, double timeout = 1.2) {
            td::actor::send_closure(client_, &ton::adnl::AdnlExtClient::send_query, "query", std::move(q),
                                    td::Timestamp::in(timeout), std::move(promise));
        }

        void fire(ton::adnl::AdnlNodeIdFull key, td::BufferSlice q, td::Promise<td::BufferSlice> promise) {
            send_raw(std::move(q), std::move(promise));
        }

        void get_max_time(td::Promise<std::tuple<ton::UnixTime, ton::BlockSeqno>> promise) {
            auto P = td::PromiseCreator::lambda([Pp = std::move(promise)](td::Result<td::BufferSlice> R) mutable {
                if (R.is_ok()) {
                    auto answer = ton::fetch_tl_object<ton::lite_api::liteServer_masterchainInfoExt>(R.move_as_ok(),
                                                                                                     true);

                    if (answer.is_error()) {
                        Pp.set_value(std::make_tuple(0, 0));
                        return;
                    }

                    auto x = answer.move_as_ok();
                    Pp.set_value(std::make_tuple((ton::UnixTime) x->last_utime_,
                                                 (ton::BlockSeqno) x->last_->seqno_));
                } else {
                    Pp.set_value(std::make_tuple(0, 0));
                }
            });

            qprocess(ton::serialize_tl_object(ton::create_tl_object<ton::lite_api::liteServer_getMasterchainInfoExt>(0),
                                              true),
                     std::move(P), 0.2);
        }

        void get_max_time_lazy(int wait_seqno, td::Promise<std::tuple<ton::UnixTime, ton::BlockSeqno>> promise) {
            if (last_lazy_call_ == 0) {
                last_lazy_call_ = (unsigned) std::time(nullptr);
            } else if ((unsigned) std::time(nullptr) - last_lazy_call_ < 5) {
                LOG(WARNING) << "Error of getting seqno for: " << id_.pubkey().ed25519_value().raw().to_hex()
                             << ", wait 1s and fire with: " << wait_seqno;
                current_seqno_ = wait_seqno;
                wait_promise_ = std::move(promise);
                alarm_timestamp() = td::Timestamp::in(1.0);
                return;
            }

            auto P = td::PromiseCreator::lambda([wait_seqno,
                                                        SelfId = actor_id(this),
                                                        Pp = std::move(promise)](
                    td::Result<td::BufferSlice> R) mutable {
                if (R.is_ok()) {
                    auto answer = ton::fetch_tl_object<ton::lite_api::liteServer_masterchainInfoExt>(R.move_as_ok(),
                                                                                                     true);

                    if (answer.is_ok()) {
                        auto x = answer.move_as_ok();
                        Pp.set_value(std::make_tuple((ton::UnixTime) x->last_utime_,
                                                     (ton::BlockSeqno) x->last_->seqno_));
                        return;
                    }
                }

                td::actor::send_closure(SelfId, &LiteServerClient::get_max_time_lazy, wait_seqno, std::move(Pp));
            });

            auto wait = ton::lite_api::liteServer_waitMasterchainSeqno(wait_seqno, 10000 - 1);
            auto prefix = ton::serialize_tl_object(&wait, true);
            auto q = ton::serialize_tl_object(ton::create_tl_object<ton::lite_api::liteServer_getMasterchainInfoExt>(0),
                                              true);
            auto raw_query = td::BufferSlice(PSLICE() << prefix.as_slice() << q.as_slice());

            qprocess(std::move(raw_query), std::move(P), 15);
        }

        void alarm() override {
            alarm_timestamp() = td::Timestamp::never();

            td::actor::send_closure(actor_id(this),
                                    &LiteServerClient::get_max_time_lazy, current_seqno_,
                                    std::move(wait_promise_));
        }

    private:
        void qprocess(td::BufferSlice q, td::Promise<td::BufferSlice> promise, double timeout = 1.2) {
            auto P = td::PromiseCreator::lambda(
                    [Pp = std::move(promise)](td::Result<td::BufferSlice> R) mutable { Pp.set_result(std::move(R)); });

            send_raw(ton::serialize_tl_object(ton::create_tl_object<ton::lite_api::liteServer_query>(std::move(q)),
                                              true),
                     std::move(P), timeout);
        }

        int current_seqno_;
        td::Promise<std::tuple<ton::UnixTime, ton::BlockSeqno>> wait_promise_;
        td::IPAddress address_;
        ton::adnl::AdnlNodeIdFull id_;
        ton::UnixTime last_lazy_call_ = 0;
        td::actor::ActorOwn<ton::adnl::AdnlExtClient> client_;
    };

    class LiteProxy : public td::actor::Actor {
    public:
        LiteProxy(std::string config_path, std::string db_path, const std::string &address, td::uint32 lite_port,
                  td::uint32 adnl_port, std::string global_config, int mode, std::string publisher_endpoint) : producer(
                cppkafka::Configuration{{"metadata.broker.list", publisher_endpoint},
                                        {"message.max.bytes",    "1000000000"},  // max
                                        {"retry.backoff.ms",     5},
                                        {"retries",              2147483647},
                                        {"acks",                 "1"},
                                        {"debug",                "broker,topic,msg"}}) {
            config_path_ = std::move(config_path);
            db_root_ = std::move(db_path);
            address_.init_host_port(address, lite_port).ensure();
            adnl_address_.init_host_port(address, adnl_port).ensure();
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

        std::unique_ptr<ton::adnl::AdnlExtClient::Callback> make_callback_lazy(adnl::AdnlNodeIdShort server) {
            class Callback : public ton::adnl::AdnlExtClient::Callback {
            public:
                void on_ready() override {

                }

                void on_stop_ready() override {
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
            to_update++;
            auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), server](
                    td::Result<std::tuple<ton::UnixTime, ton::BlockSeqno>> R) mutable {
                td::actor::send_closure(SelfId, &LiteProxy::server_update_time, server, R.move_as_ok(), true);
            });

            td::actor::send_closure(private_servers_[server].get(), &LiteServerClient::get_max_time, std::move(P));
        }

        void server_update_time(adnl::AdnlNodeIdShort server, std::tuple<ton::UnixTime, ton::BlockSeqno> time,
                                bool update = true) {
            if (update) {
                to_update--;
            }

            private_time_updated++;
            private_servers_status_[server] = time;
            if (std::get<0>(time) > std::get<0>(best_time)) {
                best_time = time;
            }


            if (to_update == 0 or update == false) {
                std::vector<adnl::AdnlNodeIdShort> uptodate;
                int outdated{0};

                for (auto &s: private_servers_status_) {
                    if (best_time != s.second) {
                        outdated += 1;
                    } else {
                        uptodate.push_back(s.first);
                    }
                }

                uptodate_private_ls = std::move(uptodate);

                if (private_time_updated >= (int) private_servers_status_.size() * 10) {
                    LOG(INFO)
                    << "Update LS uptodate: "
                    << uptodate_private_ls.size()
                    << " outdated: " << outdated << " best time: " << time_to_human(std::get<0>(best_time))
                    << " updated at: "
                    << started_update_at.elapsed();

                    private_time_updated = 0;
                }

                if (to_update == 0) {
                    inited = true;
                    td::actor::send_closure(actor_id(this), &LiteProxy::go_lazy_update_mode);
                    to_update = 999; // skip further updates, do lazy load
                }
            }

        }

        void go_lazy_update_mode() {
            for (auto &s: private_servers_lazy_clients_) {
                go_lazy_update_server(s.first, 0);
            }
        }

        void go_lazy_update_server(adnl::AdnlNodeIdShort server,
                                   ton::BlockSeqno seqno) {
            ton::BlockSeqno to_find_seqno = std::get<1>(best_time);

            if (std::get<0>(private_servers_status_[server]) >= std::get<0>(best_time)) {
                to_find_seqno = std::get<1>(private_servers_status_[server]) + 1;
            }

            if (seqno >= to_find_seqno) {
                to_find_seqno = seqno + 1;
            }

            auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), server, to_find_seqno](
                    td::Result<std::tuple<ton::UnixTime, ton::BlockSeqno>> R) mutable {
                auto res = R.move_as_ok();

                LOG(INFO)
                << "Receive from server success: " << server.bits256_value().to_hex() << " seqno: " << std::get<1>(res)
                << " waited: " << to_find_seqno;
                td::actor::send_closure(SelfId, &LiteProxy::go_lazy_update_server, server, std::get<1>(res));
                td::actor::send_closure(SelfId, &LiteProxy::server_update_time, server, std::move(res), false);
            });

            LOG(INFO) << "Send wait query for: " << server.bits256_value().to_hex() << " seqno: " << to_find_seqno;
            td::actor::send_closure(private_servers_lazy_clients_[server], &LiteServerClient::get_max_time_lazy,
                                    to_find_seqno, std::move(P));
        }

        void conn_closed(adnl::AdnlNodeIdShort server) {
            //    LOG(INFO) << "Server: " << server.bits256_value().to_hex() << " disconnected";
            private_servers_status_.erase(server);

            auto pos = std::find(uptodate_private_ls.begin(), uptodate_private_ls.end(), server);
            if (pos != uptodate_private_ls.end()) {
                uptodate_private_ls.erase(pos);
            }
        }

        void start_up() override {
            LOG(INFO) << "Start LiteProxy: " << address_;
            LOG(INFO) << "Start ADNL: " << adnl_address_;

            adnl_network_manager_ = adnl::AdnlNetworkManager::create((td::uint16) adnl_address_.get_port());
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
                    TRY_RESULT_PREFIX_ASSIGN(adnl_static_nodes_,
                                             ton::adnl::AdnlNodesList::create(conf.adnl_->static_nodes_),
                                             "bad static adnl nodes: ");
                }
            }
            td::actor::send_closure(adnl_, &ton::adnl::Adnl::add_static_nodes_from_config,
                                    std::move(adnl_static_nodes_));

            TRY_RESULT_PREFIX(dht, ton::dht::Dht::create_global_config(std::move(conf.dht_)), "bad [dht] section: ");
            auto dht_config = std::move(dht);

            td::mkdir(db_root_ + "/lite-proxy").ensure();

            adnl::AdnlAddressList addr_list;
            addr_list.add_udp_address(adnl_address_).ensure();
            addr_list.set_version(static_cast<td::int32>(td::Clocks::system()));
            addr_list.set_reinit_date(adnl::Adnl::adnl_start_time());

            // Start DHT
            for (auto &dht: config_.dht_ids) {
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
                    td::PromiseCreator::lambda(
                            [SelfId = actor_id(this)](td::Result<td::actor::ActorOwn<adnl::AdnlExtServer>> R) {
                                R.ensure();
                                td::actor::send_closure(SelfId, &LiteProxy::created_ext_server, R.move_as_ok());
                            });

            LOG(INFO) << "Start server";
            td::actor::send_closure(adnl_, &adnl::Adnl::create_ext_server, std::vector<adnl::AdnlNodeIdShort>{},
                                    std::vector<td::uint16>{(td::uint16) address_.get_port()}, std::move(Q));
        }

        void created_ext_server(td::actor::ActorOwn<adnl::AdnlExtServer> server) {
            LOG(INFO) << "Server started";
            lite_proxy_ = std::move(server);
            td::actor::send_closure(actor_id(this), &LiteProxy::init_users);
            td::actor::send_closure(actor_id(this), &LiteProxy::init_private_servers);

            for (auto &t: config_.adnl_ids) {
                LOG(WARNING) << "ADNL pub: " << keys_[t.first].ed25519_value().raw().to_hex();
                auto pkey = adnl::AdnlNodeIdFull{keys_[t.first].ed25519_value()};
                add_ext_server_id(pkey.compute_short_id());
                limits[pkey.compute_short_id()] = std::make_tuple(-1, -1);
            }
        }

        void init_private_servers() {
            LOG(INFO) << "Start init private servers";

            // get slaves
            for (auto &s: config_.liteslaves) {
                auto key = adnl::AdnlNodeIdFull{std::move(s.key)};
                td::IPAddress lc_address = s.addr;

                private_servers_[key.compute_short_id()] = td::actor::create_actor<ton::liteserver::LiteServerClient>(
                        "LSC " + key.compute_short_id().bits256_value().to_hex(), lc_address, key,
                        make_callback(key.compute_short_id()));

                private_servers_lazy_clients_[key.compute_short_id()] = td::actor::create_actor<ton::liteserver::LiteServerClient>(
                        "LSC_lazy " + key.compute_short_id().bits256_value().to_hex(), lc_address, key,
                        make_callback_lazy(key.compute_short_id()));
            }
        }

        void init_users() {
            LOG(INFO) << "Init users";
            if (ratelimitdb->count("users").move_as_ok() > 0) {
                std::string utmp;
                ratelimitdb->get("users", utmp);

                auto t = std::time(nullptr);

                auto users = fetch_tl_object<ton::ton_api::storage_liteserver_users>(td::Slice{utmp},
                                                                                     true).move_as_ok();
                LOG(INFO) << "Got: " << users->values_.size() << " users";

                for (td::Bits256 pubkey: users->values_) {
                    LOG(INFO) << "Add new user: " << pubkey;
                    users_.push_back(pubkey);

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
                                          << ", valid until: " << user_data->valid_until_ << ", ratelimit "
                                          << user_data->ratelimit_;

                                keys_[key.compute_short_id().pubkey_hash()] = key.pubkey();
                                limits[key.compute_short_id()] = std::make_tuple(user_data->valid_until_,
                                                                                 user_data->ratelimit_);
                                existing.push_back(key.compute_short_id());
                                td::actor::send_closure(actor_id(this), &LiteProxy::add_ext_server_id,
                                                        key.compute_short_id());
                                td::actor::send_closure(adnl_, &ton::adnl::Adnl::add_id, std::move(key),
                                                        ton::adnl::AdnlAddressList{},
                                                        static_cast<td::uint8>(255));
                            }
                        }
                    }
                }
            } else {
                LOG(ERROR) << "No users found!";
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
                                td::actor::send_closure(ProxyId, &LiteProxy::check_ext_query, std::move(src),
                                                        std::move(dst),
                                                        std::move(data), std::move(promise), refire + 1);
                            },
                            td::Timestamp::in(0.03 * refire_));
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

        void process_add_user(td::Bits256 private_key, td::int64 valid_until, td::int32 ratelimit,
                              td::Promise<td::BufferSlice> promise) {
            auto pk = ton::PrivateKey{ton::privkeys::Ed25519{private_key}};
            auto id = pk.compute_short_id();
            auto adnlkey = adnl::AdnlNodeIdFull{pk.compute_public_key()};
            auto pubk = adnlkey.pubkey().ed25519_value().raw();
            auto short_id = adnlkey.compute_short_id();

            LOG(WARNING)
            << "Add user: " << pubk.to_hex() << " short id: " << short_id << " valid until: " << valid_until
            << " ratelimit: " << ratelimit;

            auto k = adnlkey.pubkey().ed25519_value().raw();
            auto k_short = adnlkey.compute_short_id().bits256_value();

            if (!(limits.find(short_id) != limits.end())) {
                LOG(WARNING) << "User " << pubk.to_hex() << " is new, process add to users";
                td::actor::send_closure(keyring_, &keyring::Keyring::add_key, std::move(pk), false, [](td::Unit) {});
                users_.push_back(pubk);

                std::vector<td::Bits256> users(users_);
                ratelimitdb->set("users",
                                 create_serialize_tl_object<ton::ton_api::storage_liteserver_users>(std::move(users)));

                td::actor::send_closure(actor_id(this), &LiteProxy::add_ext_server_id, short_id);
                td::actor::send_closure(adnl_, &ton::adnl::Adnl::add_id, std::move(adnlkey),
                                        ton::adnl::AdnlAddressList{},
                                        static_cast<td::uint8>(255));
            }

            limits[short_id] = std::make_tuple(valid_until, ratelimit);
            ratelimitdb->set(pubk.as_slice(),
                             create_serialize_tl_object<ton::ton_api::storage_liteserver_user>(valid_until, ratelimit));
            ratelimitdb->flush();

            promise.set_value(create_serialize_tl_object<ton::lite_api::liteServer_newUser>(k, k_short));
        }

        void process_admin_request(td::BufferSlice query, td::Promise<td::BufferSlice> promise) {
            auto F = fetch_tl_object<ton::lite_api::Function>(query, true);
            if (F.is_error()) {
                promise.set_error(td::Status::Error("function is not valid"));
                return;
            }
            auto pf = F.move_as_ok();

            lite_api::downcast_call(
                    *pf, td::overloaded(
                            [&](lite_api::liteServer_addUser &q) {
                                this->process_add_user(q.key_, q.valid_until_, q.ratelimit_, std::move(promise));
                            },
                            [&](lite_api::liteServer_getStatData &q) {
                                this->process_get_stat_data(std::move(promise));
                            },
                            [&](auto &obj) { promise.set_error(td::Status::Error("admin function not found")); }));
        }

        void process_get_stat_data(td::Promise<td::BufferSlice> promise) {
            std::vector<std::unique_ptr<ton::lite_api::liteServer_statItem>> tmp;
            tmp.reserve(stats_data_.size());

            for (auto e: stats_data_) {
                tmp.emplace_back(e.serialize());
            }
            stats_data_.clear();

            promise.set_value(create_serialize_tl_object<ton::lite_api::liteServer_stats>(std::move(tmp)));
        }

        void publish_call(adnl::AdnlNodeIdShort dst, td::BufferSlice data, std::time_t started_at, td::Timer elapsed) {
            auto elapsed_t = elapsed.elapsed();
            auto F = fetch_tl_object<lite_api::liteServer_query>(data.clone(), true);
            if (F.is_ok()) {
                auto real_data = std::move(F.move_as_ok()->data_);

                auto Fn = fetch_tl_object<ton::lite_api::Function>(real_data, true);
                if (Fn.is_ok()) {
                    auto query_obj = Fn.move_as_ok();

                    nlohmann::json answer;
                    answer["dst"] = dst.bits256_value().to_hex();
                    answer["q"] = lite_query_name_by_id(query_obj->get_id());
                    answer["started"] = started_at;
                    answer["elapsed"] = elapsed_t;

                    std::string publish_answer = answer.dump(-1);
                    LOG(WARNING) << publish_answer;
                    producer.produce(cppkafka::MessageBuilder("lite-call-logs").partition(1).payload(publish_answer));
                }
            }
        }

        void check_ext_answer(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data,
                              td::Promise<td::BufferSlice> promise, int refire, td::Result<td::BufferSlice> result,
                              std::time_t started_at, td::Timer elapsed) {
            if (refire > allowed_refire) {
                td::actor::send_closure(actor_id(this), &LiteProxy::publish_call, dst, std::move(data), started_at,
                                        elapsed);
                promise.set_result(std::move(result));
                return;
            }

            if (result.is_ok()) {
                auto res = result.move_as_ok();
                auto lite_error = ton::fetch_tl_object<ton::lite_api::liteServer_error>(res.clone(), true);
                if (lite_error.is_ok()) {
                    auto error = lite_error.move_as_ok();

                    for (const auto &substring: {"cannot compute block with specified transaction",
                                                 "cannot load block", "seqno not in db", "block not found"}) {
                        if (error->message_.find(substring) != std::string::npos) {
                            LOG(ERROR) << "Refire on cannot load block";
                            usage[dst]--;
                            td::actor::send_closure(actor_id(this), &LiteProxy::check_ext_query, src, dst,
                                                    std::move(data), std::move(promise), refire + 1);
                            return;
                        }
                    }

                    LOG(ERROR) << "Got unexpected error for refire: " << error->message_;
                    td::actor::send_closure(actor_id(this), &LiteProxy::publish_call, dst, std::move(data), started_at,
                                            elapsed);
                    promise.set_value(std::move(res));
                    return;

                } else {
                    td::actor::send_closure(actor_id(this), &LiteProxy::publish_call, dst, std::move(data), started_at,
                                            elapsed);
                    promise.set_value(std::move(res));
                    return;
                }
            } else {
                auto e = result.move_as_error();
                usage[dst]--;
                td::actor::send_closure(actor_id(this), &LiteProxy::check_ext_query, src, dst,
                                        std::move(data), std::move(promise), refire + 1);

            }
        }


        void check_ext_query(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data,
                             td::Promise<td::BufferSlice> promise, int refire = 0) {
            if (refire > 50) {
                LOG(ERROR) << "Too deep refire";  // todo: move to public LC
                promise.set_value(create_serialize_tl_object<lite_api::liteServer_error>(228, "Too deep refire"));
                return;
            }

            LOG(INFO) << "Got query to: " << dst;

            if (inited) {
                usage[dst]++;
                auto k = limits[dst];
                if (std::get<1>(k) == -1) {
                    LOG(INFO) << "Got ADMIN connection from: " << dst;

                    auto E = fetch_tl_prefix<lite_api::liteServer_adminQuery>(data, true);
                    if (E.is_ok()) {
                        process_admin_request(std::move(E.move_as_ok()->data_), std::move(promise));
                        return;
                    }
                } else {
                    if (usage[dst] > std::get<1>(k)) {
                        usage[dst]--;
                        LOG(INFO) << "Drop to: " << dst << " because of ratelimit, usage: " << usage[dst]
                                  << " limit: " << std::get<1>(k);
                        promise.set_value(create_serialize_tl_object<lite_api::liteServer_error>(228, "Ratelimit"));
                        return;
                    }

                    if (std::time(nullptr) > std::get<0>(k)) {
                        promise.set_value(create_serialize_tl_object<lite_api::liteServer_error>(228, "Key expired"));
                        return;
                    }
                }

                if (mode_ == 0) {
                    LOG(WARNING) << "New proxy query: " << dst.bits256_value().to_hex() << " size: " << data.size();

                    td::actor::ActorId<LiteServerClient> server;
                    if (!uptodate_private_ls.empty()) {
                        auto adnl =
                                uptodate_private_ls[td::Random::fast(0, td::narrow_cast<td::uint32>(
                                        uptodate_private_ls.size() - 1))];
                        server = private_servers_[adnl].get();
                    } else {
                        auto s = td::Random::fast(0, td::narrow_cast<td::uint32>(private_servers_.size() - 1));
                        int a{0};
                        for (auto &x: private_servers_) {
                            a += 1;
                            if (a == s) {
                                server = x.second.get();
                                break;
                            }
                        }
                    }

                    if (!server.empty()) {
                        auto mP = td::PromiseCreator::lambda(
                                [P = std::move(promise),
                                        SelfId = actor_id(this),
                                        src, dst, data = data.clone(), refire,
                                        started_at = std::time(nullptr), elapsed = td::Timer()](
                                        td::Result<td::BufferSlice> R) mutable {
                                    td::actor::send_closure(SelfId, &LiteProxy::check_ext_answer, src, dst,
                                                            std::move(data), std::move(P), refire, std::move(R),
                                                            started_at, elapsed);
                                });

                        td::actor::send_closure(server, &LiteServerClient::send_raw, std::move(data),
                                                std::move(mP), live_timeout);
                    } else {
                        usage[dst]--;
                        td::actor::send_closure(actor_id(this), &LiteProxy::check_ext_query, src, dst, std::move(data),
                                                std::move(promise), 0);
                    }
                } else {
                    if (!uptodate_private_ls.empty()) {
                        auto waiter = td::actor::create_actor<ton::liteserver::LiteClientFire>(
                                "LSC::Fire", uptodate_private_ls.size(), src, dst, data.clone(), std::move(promise),
                                make_refire_callback(refire))
                                .release();
                        for (auto &s: uptodate_private_ls) {
                            LOG(INFO) << "[uptodate] Send to: " << dst << " to " << s.bits256_value().to_hex();
                            auto P = td::PromiseCreator::lambda(
                                    [WaiterId = waiter, s](td::Result<td::BufferSlice> R) mutable {
                                        td::actor::send_closure(WaiterId, &LiteClientFire::receive_answer, std::move(R),
                                                                s);
                                    });

                            td::actor::send_closure(private_servers_[s].get(), &LiteServerClient::send_raw,
                                                    data.clone(), std::move(P), live_timeout);
                        }
                    } else {
                        auto waiter = td::actor::create_actor<ton::liteserver::LiteClientFire>(
                                "LSC::Fire", private_servers_.size(), src, dst, data.clone(), std::move(promise),
                                make_refire_callback(refire))
                                .release();

                        for (auto &s: private_servers_) {  // Todo: Add some public LC
                            LOG(INFO) << "[all] Send: " << dst << " to " << s.first.bits256_value().to_hex();
                            auto P =
                                    td::PromiseCreator::lambda(
                                            [WaiterId = waiter, s = s.first](td::Result<td::BufferSlice> R) mutable {
                                                td::actor::send_closure(WaiterId, &LiteClientFire::receive_answer,
                                                                        std::move(R), s);
                                            });

                            td::actor::send_closure(s.second, &LiteServerClient::send_raw, data.clone(), std::move(P),
                                                    live_timeout);
                        }
                    }
                }

            } else {
                promise.set_value(create_serialize_tl_object<lite_api::liteServer_error>(228, "Server not ready"));
            }
        }

        void alarm() override {
            double auto_in = 0.2;
            alarm_timestamp() = td::Timestamp::in(auto_in);
            cur_alarm++;
            if (cur_alarm * auto_in == 1) {
                //      LOG(INFO) << "Clear usage";
                usage.clear();
                cur_alarm = 0;
            }

            // update times only on first run, then it will go over lazy mode
            if (to_update == 0) {
                started_update_at = td::Timer();
                to_update = private_servers_.size();
                update_server_stats();
            }

            if (stats_data_.size() > 10000) {  // move to const?
                stats_data_.clear();
                LOG(ERROR) << "Stat cache too large, clear";
            }
        }

        void update_server_stats() {
            for (auto &s: private_servers_) {
                auto P = td::PromiseCreator::lambda(
                        [SelfId = actor_id(this), server = s.first](
                                td::Result<std::tuple<ton::UnixTime, ton::BlockSeqno>> R) mutable {
                            td::actor::send_closure(SelfId, &LiteProxy::server_update_time, server, R.move_as_ok(),
                                                    true);
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

                void
                receive_message(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data) override {
                }

                void receive_query(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data,
                                   td::Promise<td::BufferSlice> promise) override {
                    td::actor::send_closure(id_, &LiteProxy::check_ext_query, src, dst, std::move(data),
                                            std::move(promise), 0);
                }

            public:
                Cb(td::actor::ActorId<LiteProxy> id) : id_(std::move(id)) {
                }
            };

            LOG(INFO) << "Subscribe to: " << id;
            td::actor::send_closure(adnl_, &adnl::Adnl::subscribe, id,
                                    adnl::Adnl::int_to_bytestring(lite_api::liteServer_query::ID),
                                    std::make_unique<Cb>(actor_id(this)));

            td::actor::send_closure(adnl_, &adnl::Adnl::subscribe, id,
                                    adnl::Adnl::int_to_bytestring(lite_api::liteServer_adminQuery::ID),
                                    std::make_unique<Cb>(actor_id(this)));

            td::actor::send_closure(lite_proxy_, &adnl::AdnlExtServer::add_local_id, id);
            LOG(INFO) << "Will fire in 1.0";
            alarm_timestamp() = td::Timestamp::in(1.0);
        }

        void got_key(ton::PublicKey key) {
            to_load_keys--;
            keys_[key.compute_short_id()] = std::move(key);

            if (to_load_keys == 0) {
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
                    td::RocksDb::open(db_root_ + "/" + config_.overlay_prefix + "rate-limits/").move_as_ok());

            for (auto &key: config_.keys_refcnt) {
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
        int cur_alarm = 0;
        std::tuple<ton::UnixTime, ton::BlockSeqno> best_time = std::make_tuple(0, 0);
        int allowed_refire = 20;
        td::Timer started_update_at;
        unsigned long to_update = 0;
        std::string db_root_;
        std::string config_path_;
        std::string global_config_;
        ton::liteserver::Config config_;
        int to_load_keys{0};
        bool inited{false};
        std::vector<td::Bits256> users_;
        std::vector<LiteServerStatItem> stats_data_;
        int private_time_updated;
        int live_timeout = 20;
        std::map<ton::PublicKeyHash, ton::PublicKey> keys_;
        td::actor::ActorOwn<keyring::Keyring> keyring_;
        std::vector<adnl::AdnlNodeIdShort> existing;
        cppkafka::Producer producer;
        td::actor::ActorOwn<adnl::Adnl> adnl_;
        td::actor::ActorOwn<adnl::AdnlNetworkManager> adnl_network_manager_;
        std::map<adnl::AdnlNodeIdShort, std::tuple<ton::UnixTime, ton::BlockSeqno>> private_servers_status_;
        std::map<adnl::AdnlNodeIdShort, td::actor::ActorOwn<LiteServerClient>> private_servers_;
        std::map<adnl::AdnlNodeIdShort, td::actor::ActorOwn<LiteServerClient>> private_servers_lazy_clients_;
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
    std::string publisher_endpoint = "";


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
                    return td::Status::Error(ton::ErrorCode::error,
                                             "bad value for --threads: should be in range [1..256]");
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
    p.add_option('P', "publisher", "publisher endpoint", [&](td::Slice arg) { publisher_endpoint = arg.str(); });

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
                                                            std::move(global_config), mode, publisher_endpoint)
                .release();
        return td::Status::OK();
    });

    scheduler.run();
    return 0;
}