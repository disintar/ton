#pragma once

#include "td/utils/logging.h"
#include "td/actor/actor.h"
#include "tuple"
#include "adnl/adnl-ext-client.h"
#include "crypto/Ed25519.h"
#include "blockchain-indexer/json.hpp"
#include "IBlockParser.hpp"


using json = nlohmann::json;

namespace ton {
    namespace validator {
        class IBLockPublisher;
    }


    class ClusterPublishSync : public td::actor::Actor {
    public:
        ClusterPublishSync();

        void start_up() override {
          if (connection_allowed) {
            client = ton::adnl::AdnlExtClient::create(std::move(adnl_id), remote_addr, make_callback());
          }
        }

        void sync_block_state(std::tuple<td::Bits256, td::string, td::string> result,
                              td::Promise<std::tuple<td::string, td::string>> P);

        void sync_block_trace(std::tuple<td::vector<json>, td::Bits256, unsigned long long, int> result,
                              std::shared_ptr<ton::validator::IBLockPublisher> publisher);

        void pass_or_publish(std::tuple<td::string, td::string> answer,
                             td::Promise<std::tuple<td::string, td::string>> P,
                             bool pass);

        void pass_or_publish_traces(
                std::tuple<td::vector<json>, td::Bits256, unsigned long long, int> result,
                std::shared_ptr<ton::validator::IBLockPublisher> publisher,
                bool pass
        );

        std::unique_ptr<ton::adnl::AdnlExtClient::Callback> make_callback();

        void conn_ready() {
          connected = true;
        }

        void conn_closed() {
          connected = false;
        }

    private:
        bool connected = false;
        bool connection_allowed = false;
        ton::adnl::AdnlNodeIdFull adnl_id;
        td::IPAddress remote_addr;
        td::actor::ActorOwn<ton::adnl::AdnlExtClient> client;
    };
}


