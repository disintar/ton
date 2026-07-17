#include "validator-engine/prometheus/PrometheusExporterActor.h"
#include "validator/custom-overlay-metrics.h"

namespace ton {
    namespace {
      bool is_valid_metric_start(char c) {
        return std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == ':';
      }

      bool is_valid_metric_char(char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == ':';
      }

      std::string sanitize_metric_name(td::Slice raw) {
        std::string name;
        name.reserve(raw.size());
        for (char c : raw.str()) {
          name.push_back(is_valid_metric_char(c) ? c : '_');
        }
        while (!name.empty() && name.back() == '_') {
          name.pop_back();
        }
        if (name.empty() || !is_valid_metric_start(name.front())) {
          name.insert(name.begin(), '_');
        }
        return name;
      }

      std::string validator_status_metric_name(td::Slice key) {
        std::string sanitized = sanitize_metric_name(key);
        if (sanitized == "shardclientmasterchainseqno") {
          return "ton_node_status_shard_client_masterchain_seqno";
        }
        if (sanitized == "masterchainblock") {
          return "ton_node_status_last_masterchain_block_seqno";
        }
        if (sanitized == "masterchainblocktime") {
          return "ton_node_status_shard_client_at";
        }
        return "ton_node_status_" + sanitized;
      }

      bool parse_number(td::Slice raw, double &value, std::string *unit = nullptr) {
        std::string s = raw.str();
        auto first = s.find_first_not_of(" \t");
        if (first == std::string::npos) {
          return false;
        }
        auto last = s.find_last_not_of(" \t");
        s = s.substr(first, last - first + 1);

        char *end = nullptr;
        value = std::strtod(s.c_str(), &end);
        if (end == s.c_str()) {
          return false;
        }
        while (*end == ' ' || *end == '\t') {
          ++end;
        }
        if (*end == '\0') {
          if (unit) {
            unit->clear();
          }
          return true;
        }
        const char *unit_start = end;
        while (std::isalpha(static_cast<unsigned char>(*end))) {
          ++end;
        }
        while (*end == ' ' || *end == '\t') {
          ++end;
        }
        if (*end != '\0') {
          return false;
        }
        if (unit) {
          *unit = sanitize_metric_name(td::Slice(unit_start, static_cast<std::size_t>(end - unit_start)));
          std::transform(unit->begin(), unit->end(), unit->begin(), [](unsigned char c) { return std::tolower(c); });
        }
        return true;
      }

      bool parse_block_seqno(td::Slice raw, double &value) {
        std::string s = raw.str();
        auto close = s.find(')');
        if (close == std::string::npos) {
          return false;
        }
        auto comma = s.rfind(',', close);
        if (comma == std::string::npos || comma + 1 >= close) {
          return false;
        }
        return parse_number(td::Slice(s).substr(comma + 1, close - comma - 1), value);
      }

      bool emit_colon_pairs(std::stringstream &out, const std::string &metric, td::Slice raw) {
        std::stringstream in(raw.str());
        std::string token;
        bool emitted = false;
        while (in >> token) {
          auto colon = token.find(':');
          if (colon == std::string::npos || colon == 0 || colon + 1 >= token.size()) {
            continue;
          }
          double value = 0.0;
          if (!parse_number(td::Slice(token).substr(colon + 1), value)) {
            continue;
          }
          auto label = sanitize_metric_name(td::Slice(token).substr(0, colon));
          out << metric << "{item=\"" << label << "\"} " << value << "\n";
          emitted = true;
        }
        return emitted;
      }

      bool emit_named_pairs(std::stringstream &out, const std::string &metric, td::Slice raw) {
        std::stringstream in(raw.str());
        std::string name;
        std::string colon;
        std::string value_s;
        bool emitted = false;
        while (in >> name >> colon >> value_s) {
          if (colon != ":") {
            return false;
          }
          double value = 0.0;
          std::string unit;
          if (!parse_number(td::Slice(value_s), value, &unit)) {
            return false;
          }
          auto suffix = sanitize_metric_name(name);
          std::transform(suffix.begin(), suffix.end(), suffix.begin(), [](unsigned char c) { return std::tolower(c); });
          out << metric << "_" << suffix;
          if (!unit.empty()) {
            out << "_" << unit;
          }
          out << " " << value << "\n";
          emitted = true;
        }
        return emitted;
      }

      void append_validator_manager_metric(std::stringstream &out, td::Slice key, td::Slice raw_value) {
        auto metric = validator_status_metric_name(key);

        double value = 0.0;
        if (key == "masterchainblock" && parse_block_seqno(raw_value, value)) {
          out << metric << " " << value << "\n";
          return;
        }

        std::string unit;
        if (parse_number(raw_value, value, &unit)) {
          if (!unit.empty()) {
            metric += "_" + unit;
          }
          out << metric << " " << value << "\n";
          return;
        }

        if (emit_named_pairs(out, metric, raw_value)) {
          return;
        }
        if (emit_colon_pairs(out, metric, raw_value)) {
          return;
        }

        out << "# skipped non-numeric validator manager stat " << sanitize_metric_name(key) << "\n";
      }

      std::string sanitize_metrics_blob(td::Slice raw) {
        std::stringstream in(raw.str());
        std::stringstream out;
        std::string line;

        while (std::getline(in, line)) {
          if (!line.empty() && line.back() == '\r') {
            line.pop_back();
          }

          auto first_non_space = line.find_first_not_of(" \t");
          if (first_non_space != std::string::npos && line[first_non_space] == '=') {
            out << '#';
          }

          out << line << '\n';
        }

        return out.str();
      }
    }  // namespace

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

      ss << "# Validator manager stats\n";
      for (const auto &x : validator_manager_stats) {
        append_validator_manager_metric(ss, x.first, x.second);
      }

      ss << "\n" << sanitize_metrics_blob(validator_manager_actor_stats);
      ss << "\n# Liteserver stats\n\n";
      ss << "\n" << sanitize_metrics_blob(liteserver_stats);
      ss << "\n# Liteserver credentials\n\n";
      ss << "\n" << sanitize_metrics_blob(liteserver_credentials);
      ss << "\n# HELP ton_custom_overlay_block_broadcasts_received_total Total valid block broadcasts received from custom overlays\n";
      ss << "# TYPE ton_custom_overlay_block_broadcasts_received_total counter\n";
      ss << "ton_custom_overlay_block_broadcasts_received_total "
         << validator::fullnode::get_custom_overlay_block_broadcasts_received_total() << "\n";
      ss << "# HELP ton_custom_overlay_block_broadcasts_applied_total Total custom overlay block broadcasts applied locally\n";
      ss << "# TYPE ton_custom_overlay_block_broadcasts_applied_total counter\n";
      ss << "ton_custom_overlay_block_broadcasts_applied_total "
         << validator::fullnode::get_custom_overlay_block_broadcasts_applied_total() << "\n";
      ss << "# HELP ton_custom_overlay_duplicate_block_broadcasts_dropped_total Duplicate custom overlay block broadcasts dropped before deserialization\n";
      ss << "# TYPE ton_custom_overlay_duplicate_block_broadcasts_dropped_total counter\n";
      ss << "ton_custom_overlay_duplicate_block_broadcasts_dropped_total "
         << validator::fullnode::get_custom_overlay_duplicate_block_broadcasts_dropped_total() << "\n";
      ss << "# HELP ton_custom_overlay_duplicate_block_candidates_dropped_total Duplicate custom overlay block candidates dropped before deserialization\n";
      ss << "# TYPE ton_custom_overlay_duplicate_block_candidates_dropped_total counter\n";
      ss << "ton_custom_overlay_duplicate_block_candidates_dropped_total "
         << validator::fullnode::get_custom_overlay_duplicate_block_candidates_dropped_total() << "\n";
      ss << "# HELP ton_public_overlay_duplicate_block_broadcasts_dropped_total Duplicate public overlay block broadcasts dropped before deserialization\n";
      ss << "# TYPE ton_public_overlay_duplicate_block_broadcasts_dropped_total counter\n";
      ss << "ton_public_overlay_duplicate_block_broadcasts_dropped_total "
         << validator::fullnode::get_public_overlay_duplicate_block_broadcasts_dropped_total() << "\n";
      ss << "# HELP ton_public_overlay_duplicate_block_candidates_dropped_total Duplicate public overlay block candidates dropped before deserialization\n";
      ss << "# TYPE ton_public_overlay_duplicate_block_candidates_dropped_total counter\n";
      ss << "ton_public_overlay_duplicate_block_candidates_dropped_total "
         << validator::fullnode::get_public_overlay_duplicate_block_candidates_dropped_total() << "\n";
      ss << "# HELP ton_custom_overlay_sync_downloads_total Custom overlay sync download attempts by result\n";
      ss << "# TYPE ton_custom_overlay_sync_downloads_total counter\n";
      for (std::size_t kind = 0; kind < validator::fullnode::custom_overlay_sync_kind_count(); kind++) {
        for (std::size_t sender = 0; sender < validator::fullnode::custom_overlay_sync_sender_count(); sender++) {
          for (std::size_t result = 0; result < validator::fullnode::custom_overlay_sync_result_count(); result++) {
            ss << "ton_custom_overlay_sync_downloads_total{kind=\""
               << validator::fullnode::custom_overlay_sync_kind_label(kind) << "\",sender=\""
               << validator::fullnode::custom_overlay_sync_sender_label(sender) << "\",result=\""
               << validator::fullnode::custom_overlay_sync_result_label(result) << "\"} "
               << validator::fullnode::get_custom_overlay_sync_downloads_total(kind, sender, result) << "\n";
          }
        }
      }
      ss << "# HELP ton_custom_overlay_sync_download_latency_ms Custom overlay sync download latency in milliseconds\n";
      ss << "# TYPE ton_custom_overlay_sync_download_latency_ms summary\n";
      for (std::size_t kind = 0; kind < validator::fullnode::custom_overlay_sync_kind_count(); kind++) {
        for (std::size_t sender = 0; sender < validator::fullnode::custom_overlay_sync_sender_count(); sender++) {
          for (std::size_t result = 0; result < validator::fullnode::custom_overlay_sync_result_count(); result++) {
            ss << "ton_custom_overlay_sync_download_latency_ms_sum{kind=\""
               << validator::fullnode::custom_overlay_sync_kind_label(kind) << "\",sender=\""
               << validator::fullnode::custom_overlay_sync_sender_label(sender) << "\",result=\""
               << validator::fullnode::custom_overlay_sync_result_label(result) << "\"} "
               << validator::fullnode::get_custom_overlay_sync_download_latency_ms_sum(kind, sender, result) << "\n";
            ss << "ton_custom_overlay_sync_download_latency_ms_count{kind=\""
               << validator::fullnode::custom_overlay_sync_kind_label(kind) << "\",sender=\""
               << validator::fullnode::custom_overlay_sync_sender_label(sender) << "\",result=\""
               << validator::fullnode::custom_overlay_sync_result_label(result) << "\"} "
               << validator::fullnode::get_custom_overlay_sync_download_latency_ms_count(kind, sender, result) << "\n";
          }
        }
      }
      ss << "# HELP ton_custom_overlay_sync_peer_downloads_total Custom overlay per-peer sync download attempts by result\n";
      ss << "# TYPE ton_custom_overlay_sync_peer_downloads_total counter\n";
      for (std::size_t kind = 0; kind < validator::fullnode::custom_overlay_sync_kind_count(); kind++) {
        for (std::size_t sender = 0; sender < validator::fullnode::custom_overlay_sync_sender_count(); sender++) {
          for (std::size_t result = 0; result < validator::fullnode::custom_overlay_sync_result_count(); result++) {
            ss << "ton_custom_overlay_sync_peer_downloads_total{kind=\""
               << validator::fullnode::custom_overlay_sync_kind_label(kind) << "\",sender=\""
               << validator::fullnode::custom_overlay_sync_sender_label(sender) << "\",result=\""
               << validator::fullnode::custom_overlay_sync_result_label(result) << "\"} "
               << validator::fullnode::get_custom_overlay_sync_peer_downloads_total(kind, sender, result) << "\n";
          }
        }
      }
      ss << "# HELP ton_custom_overlay_sync_peer_latency_ms Custom overlay per-peer sync download latency in milliseconds\n";
      ss << "# TYPE ton_custom_overlay_sync_peer_latency_ms summary\n";
      for (std::size_t kind = 0; kind < validator::fullnode::custom_overlay_sync_kind_count(); kind++) {
        for (std::size_t sender = 0; sender < validator::fullnode::custom_overlay_sync_sender_count(); sender++) {
          for (std::size_t result = 0; result < validator::fullnode::custom_overlay_sync_result_count(); result++) {
            ss << "ton_custom_overlay_sync_peer_latency_ms_sum{kind=\""
               << validator::fullnode::custom_overlay_sync_kind_label(kind) << "\",sender=\""
               << validator::fullnode::custom_overlay_sync_sender_label(sender) << "\",result=\""
               << validator::fullnode::custom_overlay_sync_result_label(result) << "\"} "
               << validator::fullnode::get_custom_overlay_sync_peer_latency_ms_sum(kind, sender, result) << "\n";
            ss << "ton_custom_overlay_sync_peer_latency_ms_count{kind=\""
               << validator::fullnode::custom_overlay_sync_kind_label(kind) << "\",sender=\""
               << validator::fullnode::custom_overlay_sync_sender_label(sender) << "\",result=\""
               << validator::fullnode::custom_overlay_sync_result_label(result) << "\"} "
               << validator::fullnode::get_custom_overlay_sync_peer_latency_ms_count(kind, sender, result) << "\n";
          }
        }
      }
      ss << "# HELP ton_custom_overlay_sync_fallbacks_total Sync downloads that fell back from custom overlay to public overlay\n";
      ss << "# TYPE ton_custom_overlay_sync_fallbacks_total counter\n";
      for (std::size_t kind = 0; kind < validator::fullnode::custom_overlay_sync_kind_count(); kind++) {
        for (std::size_t reason = 0; reason < validator::fullnode::custom_overlay_sync_fallback_reason_count();
             reason++) {
          ss << "ton_custom_overlay_sync_fallbacks_total{kind=\""
             << validator::fullnode::custom_overlay_sync_kind_label(kind) << "\",reason=\""
             << validator::fullnode::custom_overlay_sync_fallback_reason_label(reason) << "\"} "
             << validator::fullnode::get_custom_overlay_sync_fallbacks_total(kind, reason) << "\n";
        }
      }
      ss << "# HELP ton_public_overlay_sync_downloads_total Public overlay sync downloads by reason\n";
      ss << "# TYPE ton_public_overlay_sync_downloads_total counter\n";
      for (std::size_t kind = 0; kind < validator::fullnode::custom_overlay_sync_kind_count(); kind++) {
        for (std::size_t reason = 0; reason < validator::fullnode::public_overlay_sync_reason_count(); reason++) {
          ss << "ton_public_overlay_sync_downloads_total{kind=\""
             << validator::fullnode::custom_overlay_sync_kind_label(kind) << "\",reason=\""
             << validator::fullnode::public_overlay_sync_reason_label(reason) << "\"} "
             << validator::fullnode::get_public_overlay_sync_downloads_total(kind, reason) << "\n";
        }
      }

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

    void PrometheusExporterActor::set_liteserver_credentials(std::string data) {
      std::lock_guard<std::mutex> lock(status_mutex);
      get_ton_node_status()->liteserver_credentials = std::move(data);
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

//        LOG(WARNING) << "[Prometheus Exporter] Received request: " << method << " " << url << " command: " << command;

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

//          LOG(WARNING) << "[Prometheus Exporter] Response queued with status: " << ret;
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
          MHD_add_response_header(response, "Content-Type", "text/plain");

          if (!response) {
            LOG(WARNING) << "[Prometheus Exporter] Failed to create response";
            return MHD_NO;
          }

          MHD_Result ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
          MHD_destroy_response(response);

//          LOG(WARNING) << "[Prometheus Exporter] Response queued with status: " << ret;
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

//          LOG(WARNING) << "[Prometheus Exporter] Response queued with status: " << ret;
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
