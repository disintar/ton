#include "validator-engine/prometheus/PrometheusExporterActor.h"

namespace ton {
    std::mutex status_mutex;

    std::shared_ptr<TonNodeStatus> get_ton_node_status() {
      static std::shared_ptr<TonNodeStatus> instance;
      static std::once_flag flag;

      std::call_once(flag, []() {
          instance = std::make_shared<TonNodeStatus>();
      });

      return instance;
    }


    std::string TonNodeStatus::to_text() const {
      std::stringstream ss;

      for (auto x: validator_manager_stats) {
        ss << x.first << " " << x.second << "\n";
      }

      ss << "\n" << validator_manager_actor_stats;
      ss << "\n# Liteserver stats\n\n";
      ss << "\n" << liteserver_stats;

      return ss.str();
    }

    bool TonNodeStatus::alive() const {
      if ((td::Clocks::system() - validator_manager_alive_at) > 10) {
        return false;
      } else {
        return true;
      }
    }

    PrometheusExporterActor::PrometheusExporterActor(td::uint32 port) {
      http_port_ = port;
    }

    void PrometheusExporterActor::set_validator_manager_stats(std::vector<std::pair<std::string, std::string>> data) {
      std::lock_guard<std::mutex> lock(status_mutex);
      get_ton_node_status()->validator_manager_stats = std::move(data);
    }

    void PrometheusExporterActor::set_validator_manager_alive_at(UnixTime at) {
      std::lock_guard<std::mutex> lock(status_mutex);
      get_ton_node_status()->validator_manager_alive_at = at;
    }

    void PrometheusExporterActor::set_validator_manager_actor_stats(std::string data) {
      std::lock_guard<std::mutex> lock(status_mutex);
      get_ton_node_status()->validator_manager_actor_stats = std::move(data);
    }

    void PrometheusExporterActor::set_liteserver_stats(std::string data) {
      std::lock_guard<std::mutex> lock(status_mutex);
      get_ton_node_status()->liteserver_stats = std::move(data);
    }

    MHD_Result PrometheusExporterActor::process_http_request(void *cls, struct MHD_Connection *connection,
                                                             const char *url, const char *method,
                                                             const char *version, const char *upload_data,
                                                             size_t *upload_data_size, void **ptr) {
      try {
        std::string url_s = url;
        auto pos = url_s.rfind('/');
        std::string prefix;
        std::string command;
        if (pos == std::string::npos) {
          prefix = "";
          command = url_s;
        } else {
          prefix = url_s.substr(0, pos + 1);
          command = url_s.substr(pos + 1);
        }

        LOG(WARNING) << "[Prometheus Exporter] Received request: " << method << " " << url << " command: " << command;

        if (*upload_data_size != 0) {
          *upload_data_size = 0;
          return MHD_YES;
        }

        if (command == "live") {
          bool alive;

          {
            std::lock_guard<std::mutex> lock(status_mutex);
            alive = get_ton_node_status()->alive();
          }
          std::string status_text;
          if (alive){
            status_text = "ok";
          } else {
            status_text = "degrade";
          }

          struct MHD_Response *response = MHD_create_response_from_buffer(
                  status_text.size(),
                  (void *) status_text.c_str(),
                  MHD_RESPMEM_MUST_COPY
          );

          if (!response) {
            LOG(WARNING) << "Failed to create response";
            return MHD_NO;
          }

          MHD_Result ret = MHD_queue_response(connection, alive ? MHD_HTTP_OK : MHD_HTTP_INTERNAL_SERVER_ERROR, response);
          MHD_destroy_response(response);

          LOG(WARNING) << "[Prometheus Exporter] Response queued with status: " << ret;
          return ret;
        } else if (command == "metrics") {
          std::string status_text;

          {
            std::lock_guard<std::mutex> lock(status_mutex);
            status_text = get_ton_node_status()->to_text();
          }

          struct MHD_Response *response = MHD_create_response_from_buffer(
                  status_text.size(),
                  (void *) status_text.c_str(),
                  MHD_RESPMEM_MUST_COPY
          );

          if (!response) {
            LOG(WARNING) << "Failed to create response";
            return MHD_NO;
          }

          MHD_Result ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
          MHD_destroy_response(response);

          LOG(WARNING) << "[Prometheus Exporter] Response queued with status: " << ret;
          return ret;
        } else {
          std::string status_text = "ready";
          struct MHD_Response *response = MHD_create_response_from_buffer(
                  status_text.size(),
                  (void *) status_text.c_str(),
                  MHD_RESPMEM_MUST_COPY
          );

          MHD_Result ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
          MHD_destroy_response(response);

          LOG(WARNING) << "[Prometheus Exporter] Response queued with status: " << ret;
          return ret;
        }
      } catch (...) {
        LOG(WARNING) << "Server crash";
        return MHD_NO;
      };
    }

    void PrometheusExporterActor::run() {
      memset(&addr, 0, sizeof(addr));
      addr.sin_family = AF_INET;
      addr.sin_port = htons(static_cast<td::uint16>(http_port_));
      addr.sin_addr.s_addr = htonl(INADDR_ANY);

      daemon_ = MHD_start_daemon(
              MHD_USE_INTERNAL_POLLING_THREAD + MHD_USE_POLL + MHD_USE_DEBUG,
              0,
              nullptr,
              nullptr,
              &process_http_request,
              nullptr,
              MHD_OPTION_SOCK_ADDR, &addr,
              MHD_OPTION_END
      );

      CHECK(daemon_ != nullptr);
      LOG(WARNING) << "Start Prometheus status exporter on port: " << http_port_;
    }
}