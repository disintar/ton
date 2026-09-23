// SPDX-License-Identifier: LGPL-2.0-or-later
#pragma once
#include <atomic>
#include <sstream>
namespace ton::validator::toncenter {
struct Metrics {
  std::atomic<unsigned long long> enqueued{0}, attempts{0}, accepted{0}, network_errors{0},
      http_errors{0}, rate_limited{0}, invalid_responses{0}, duplicates{0}, queue_full{0}, expired{0}, queue_size{0};
  std::atomic<unsigned long long> private_attempts{0}, private_accepted{0};
  std::atomic<bool> enabled{false};
  std::string prometheus() const {
    std::ostringstream out;
#define TONCENTER_METRIC(field, name, type) \
    out << "# TYPE ton_node_toncenter_" name " " type "\n" \
        << "ton_node_toncenter_" name " " << field.load() << "\n";
    TONCENTER_METRIC(enabled, "enabled", "gauge")
    TONCENTER_METRIC(enqueued, "enqueued_total", "counter")
    TONCENTER_METRIC(attempts, "requests_total", "counter")
    TONCENTER_METRIC(accepted, "accepted_total", "counter")
    TONCENTER_METRIC(private_attempts, "private_overlay_requests_total", "counter")
    TONCENTER_METRIC(private_accepted, "private_overlay_accepted_total", "counter")
    TONCENTER_METRIC(network_errors, "network_errors_total", "counter")
    TONCENTER_METRIC(http_errors, "http_errors_total", "counter")
    TONCENTER_METRIC(rate_limited, "rate_limited_total", "counter")
    TONCENTER_METRIC(invalid_responses, "invalid_responses_total", "counter")
    TONCENTER_METRIC(duplicates, "duplicates_total", "counter")
    TONCENTER_METRIC(queue_full, "queue_full_total", "counter")
    TONCENTER_METRIC(expired, "expired_total", "counter")
    TONCENTER_METRIC(queue_size, "queue_size", "gauge")
#undef TONCENTER_METRIC
    return out.str();
  }
};
inline Metrics &metrics() { static Metrics value; return value; }
}
