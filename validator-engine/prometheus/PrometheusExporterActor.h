#pragma once

#include "adnl/adnl-ext-client.h"
#include "adnl/utils.hpp"
#include "auto/tl/ton_api_json.h"
#include "td/utils/OptionParser.h"
#include "td/utils/Time.h"
#include "td/utils/filesystem.h"
#include "td/utils/format.h"
#include "td/utils/Random.h"
#include "td/utils/crypto.h"
#include "td/utils/port/signals.h"
#include "td/utils/port/user.h"
#include "td/utils/port/FileFd.h"
#include "ton/ton-tl.hpp"
#include "block/block-db.h"
#include "block/block.h"
#include "block/block-auto.h"
#include "vm/boc.h"
#include "vm/cellops.h"
#include "vm/cells/MerkleProof.h"
#include "block/mc-config.h"

#include "vm/boc.h"
#include "vm/cellops.h"
#include "vm/cells/MerkleProof.h"
#include "vm/vm.h"

#include "auto/tl/lite_api.h"
#include "ton/lite-tl.hpp"
#include "tl-utils/lite-utils.hpp"
#include "lite-client/ext-client.h"

#include <microhttpd.h>

#if TD_DARWIN || TD_LINUX

#include <unistd.h>
#include <fcntl.h>

#endif

#include <iostream>
#include <sstream>

#define MAX_POST_SIZE (64 << 10)

// Beginning with v0.9.71, libmicrohttpd changed the return type of most
// functions from int to enum MHD_Result
// https://git.gnunet.org/gnunet.git/tree/src/include/gnunet_mhd_compat.h
// proposes to define a constant for the return type so it works well
// with all versions of libmicrohttpd
#if MHD_VERSION >= 0x00097002
#define MHD_RESULT enum MHD_Result
#else
#define MHD_RESULT int
#endif

namespace ton {
    extern std::mutex status_mutex;

    class TonNodeStatus {
    public:
        std::vector<std::pair<std::string, std::string>> validator_manager_stats;
        std::string validator_manager_actor_stats = "";
        std::string liteserver_stats = "";
        std::string to_text() const;
        bool alive() const;
        UnixTime validator_manager_alive_at;
    };

    std::shared_ptr<TonNodeStatus> get_ton_node_status();

    class PrometheusExporterActor : public td::actor::Actor {
    private:
        td::uint32 http_port_;
        MHD_Daemon *daemon_ = nullptr;
        struct sockaddr_in addr;
    public:
        td::actor::ActorId<PrometheusExporterActor> self_id_;

        void set_http_port(td::uint32 port) {
          http_port_ = port;
        }

        void start_up() override {
          self_id_ = actor_id(this);
        }

        void set_validator_manager_stats(std::vector<std::pair<std::string, std::string>> data);
        void set_validator_manager_actor_stats(std::string data);
        void set_liteserver_stats(std::string data);
        void set_validator_manager_alive_at(UnixTime at);

        static MHD_Result process_http_request(void *cls, struct MHD_Connection *connection,
                                               const char *url, const char *method,
                                               const char *version, const char *upload_data,
                                               size_t *upload_data_size, void **ptr);

        void tear_down() override {
          if (daemon_) {
            MHD_stop_daemon(daemon_);
            daemon_ = nullptr;
          }
        }

        PrometheusExporterActor(td::uint32 port);

        void run();
    };
}