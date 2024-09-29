#include "ClusterSyncer.hpp"
#include "tl/generate/auto/tl/lite_api.h"
#include "ton/lite-tl.hpp"
#include "tl-utils/lite-utils.hpp"


std::string hex_str_to_bytes(std::string str) {
  std::string t;

  if (str.size() & 1) {
    throw std::invalid_argument("not a hex string");
  }
  t.reserve(str.size() >> 1);

  std::size_t i;
  unsigned f = 0;
  for (i = 0; i < str.size(); i++) {
    int c = str[i];
    if (c >= '0' && c <= '9') {
      c -= '0';
    } else {
      c |= 0x20;
      if (c >= 'a' && c <= 'f') {
        c -= 'a' - 10;
      } else {
        break;
      }
    }
    f = (f << 4) + c;
    if (i & 1) {
      t += (char) (f & 0xff);
    }
  }

  return t;
}

std::string public_slice_buffer_to_hex(td::Slice buffer) {
  const char *hex = "0123456789ABCDEF";
  std::string res(2 * buffer.size(), '\0');
  for (std::size_t i = 0; i < buffer.size(); i++) {
    auto c = buffer.ubegin()[i];
    res[2 * i] = hex[(c >> 4) & 0xF];
    res[2 * i + 1] = hex[c & 0xF];
  }
  return res;
}


namespace ton {
    ClusterPublishSync::ClusterPublishSync() {

      if (std::getenv("CLUSTER_SYNC_KEY") != nullptr) {
        auto key = std::string(std::getenv("CLUSTER_SYNC_KEY"));
        auto pub_key = td::Ed25519::PublicKey{td::SecureString{hex_str_to_bytes(std::move(key))}};
        auto key_text = public_slice_buffer_to_hex(pub_key.as_octet_string());

        adnl_id = ton::adnl::AdnlNodeIdFull{ton::PublicKey(std::move(pub_key))};
        int port = std::atoi(std::getenv("CLUSTER_SYNC_PORT"));
        std::string host = std::getenv("CLUSTER_SYNC_HOST");
        remote_addr.init_host_port(host, port).ensure();
        LOG(INFO) << "Cluster sync inited with: " << remote_addr << " key: "
                  << key_text << " adnl: "
                  << adnl_id.pubkey().compute_short_id().bits256_value().to_hex();
        connection_allowed = true;
      }
    }

    void ClusterPublishSync::sync_block_state(std::tuple<td::Bits256, td::string, td::string> result,
                                              td::Promise<std::tuple<td::string, td::string>> P) {
      auto root_hash = std::get<0>(result);
      auto answer = std::make_tuple(std::get<1>(result), std::get<2>(result));

      if (!connected) {
        LOG(INFO) << "Process root_hash: " << root_hash.to_hex() << " sync server NOT connected";
        P.set_result(std::move(answer));
      } else {
        auto query = ton::create_tl_object<ton::lite_api::liteServer_checkItemPublished>(root_hash, 0);
        auto q = ton::create_tl_object<ton::lite_api::liteServer_adminQuery>(ton::serialize_tl_object(query, true));

        td::actor::send_closure(
                client, &ton::adnl::AdnlExtClient::send_query, "query", serialize_tl_object(std::move(q), true),
                td::Timestamp::in(timeout), [SelfID = actor_id(this), P = std::move(P),
                        answer = std::move(answer), root_hash](td::Result<td::BufferSlice> res) mutable -> void {
                    if (res.is_error()) {
                      LOG(ERROR) << "Sync server incorrect answer for: " << root_hash.to_hex();
                      td::actor::send_closure(SelfID, &ClusterPublishSync::pass_or_publish, answer, std::move(P),
                                              false);
                    } else {
                      auto F =
                              ton::fetch_tl_object<ton::lite_api::liteServer_itemPublished>(res.move_as_ok(), true);

                      if (F.is_error()) {
                        LOG(ERROR) << "Sync server can't decode answer for: " << root_hash.to_hex();
                        td::actor::send_closure(SelfID, &ClusterPublishSync::pass_or_publish, answer, std::move(P),
                                                false);
                      } else {
                        auto data = F.move_as_ok();
                        LOG(INFO) << "Sync server answer for: " << root_hash.to_hex() << " -> " << bool(data->value_);
                        td::actor::send_closure(SelfID, &ClusterPublishSync::pass_or_publish, answer, std::move(P),
                                                bool(data->value_));
                      }
                    }
                });
      }
    }

    void ClusterPublishSync::sync_block_trace(std::tuple<td::vector<json>, td::Bits256, unsigned long long, int> result,
                                              std::shared_ptr<ton::validator::IBLockPublisher> publisher) {
      auto root_hash = std::get<1>(result);

      if (!connected) {
        LOG(INFO) << "Process root_hash: " << root_hash << " sync server NOT connected";
        td::actor::send_closure(actor_id(this),
                                &ClusterPublishSync::pass_or_publish_traces,
                                std::move(result),
                                std::move(publisher),
                                false);
      } else {
        auto query = ton::create_tl_object<ton::lite_api::liteServer_checkItemPublished>(root_hash, 1);
        auto q = ton::create_tl_object<ton::lite_api::liteServer_adminQuery>(ton::serialize_tl_object(query, true));

        td::actor::send_closure(
                client, &ton::adnl::AdnlExtClient::send_query, "query", serialize_tl_object(std::move(q), true),
                td::Timestamp::in(timeout), [SelfID = actor_id(this),
                        result = std::move(result),
                        publisher = std::move(publisher),
                        root_hash](td::Result<td::BufferSlice> res) mutable -> void {
                    if (res.is_error()) {
                      LOG(ERROR) << "Sync server incorrect answer for: " << root_hash.to_hex();
                      td::actor::send_closure(SelfID, &ClusterPublishSync::pass_or_publish_traces,
                                              std::move(result), std::move(publisher), false);
                    } else {
                      auto F =
                              ton::fetch_tl_object<ton::lite_api::liteServer_itemPublished>(res.move_as_ok(), true);

                      if (F.is_error()) {
                        LOG(ERROR) << "Sync server can't decode answer for: " << root_hash.to_hex();
                        td::actor::send_closure(SelfID, &ClusterPublishSync::pass_or_publish_traces,
                                                std::move(result), std::move(publisher), false);
                      } else {
                        auto data = F.move_as_ok();
                        LOG(INFO) << "Sync server answer for: " << root_hash.to_hex() << " -> " << bool(data->value_);
                        td::actor::send_closure(SelfID, &ClusterPublishSync::pass_or_publish_traces,
                                                std::move(result), std::move(publisher), bool(data->value_));
                      }
                    }
                });
      }
    }

    void ClusterPublishSync::pass_or_publish_traces(
            std::tuple<td::vector<json>, td::Bits256, unsigned long long, int> data,
            std::shared_ptr<ton::validator::IBLockPublisher> publisher,
            bool pass
    ) {
      if (!pass) {
        auto transactions = std::get<0>(data);
        if (!transactions.empty()) {
          json data_to_send = {
                  {"transactions", std::move(transactions)},
                  {"root_hash",    std::get<1>(data).to_hex()},
          };

          publisher->publishOutMsgs(std::get<3>(data), std::get<2>(data), data_to_send.dump(-1));
        }
      }
    };

    void ClusterPublishSync::pass_or_publish(std::tuple<td::string, td::string> answer,
                                             td::Promise<std::tuple<td::string, td::string>> P,
                                             bool pass) {
      if (pass) {
        P.set_error(td::Status::Error());
      } else {
        P.set_result(std::move(answer));
      }
    }

    std::unique_ptr<ton::adnl::AdnlExtClient::Callback> ClusterPublishSync::make_callback() {
      class Callback : public ton::adnl::AdnlExtClient::Callback {
      public:
          void on_ready() override {
            td::actor::send_closure(id_, &ClusterPublishSync::conn_ready);
          }

          void on_stop_ready() override {
            td::actor::send_closure(id_, &ClusterPublishSync::conn_closed);
          }

          Callback(td::actor::ActorId<ClusterPublishSync> id) : id_(std::move(id)) {
          }

      private:
          td::actor::ActorId<ClusterPublishSync> id_;
      };
      return std::make_unique<Callback>(actor_id(this));
    }
}