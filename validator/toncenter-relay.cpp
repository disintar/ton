// SPDX-License-Identifier: LGPL-2.0-or-later
#include "toncenter-relay.h"
#include "toncenter-relay-metrics.h"
#include "external-message-relay.h"
#include "td/utils/base64.h"
#include "td/utils/crypto.h"
#include "td/utils/misc.h"
#include "td/utils/logging.h"
#include "blockchain-indexer/json.hpp"
#include <curl/curl.h>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>

namespace ton::validator::toncenter {
namespace {
constexpr const char *endpoint = "https://toncenter.com/api/v3/message";
using Clock = std::chrono::steady_clock;
size_t receive(char *data, size_t size, size_t count, void *opaque) {
  auto &body = *static_cast<std::string *>(opaque);
  const auto bytes = size * count;
  if (bytes > 65536 || body.size() > 65536 - bytes) return 0;
  body.append(data, bytes);
  return bytes;
}
std::string env(const char *name) {
  auto value = std::getenv(name);
  return value ? value : "";
}
class Relay {
 public:
  Relay() : enabled_(env("DTON_TONCENTER_RELAY") == "1"), api_key_(env("DTON_TONCENTER_API_KEY")) {
    metrics().enabled = enabled_;
    if (!enabled_) return;
    auto rps = env("DTON_TONCENTER_RPS");
    char *end = nullptr;
    long rate = std::strtol(rps.c_str(), &end, 10);
    if (!rps.empty() && *end == 0 && rate >= 1 && rate <= 100) interval_ = std::chrono::milliseconds(1000 / rate);
    worker_ = std::thread([this] { run(); });
    LOG(WARNING) << "[toncenter-relay] enabled interval_ms=" << interval_.count();
  }
  ~Relay() {
    { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; }
    ready_.notify_all();
    if (worker_.joinable()) worker_.join();
  }
  void enqueue(td::Slice boc, Source source) {
    if (!enabled_ || boc.empty() || boc.size() > 65536) return;
    auto hash = td::hex_encode(td::sha256(boc));
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.size() >= 64) { ++metrics().queue_full; return; }
    const auto now = Clock::now();
    if (!seen_.admit(hash, std::chrono::duration<double>(now.time_since_epoch()).count())) {
      ++metrics().duplicates; return;
    }
    queue_.push_back({boc.str(), std::move(hash), now, source});
    ++metrics().enqueued;
    metrics().queue_size = queue_.size();
    ready_.notify_one();
  }
 private:
  struct Item { std::string boc, hash; Clock::time_point enqueued; Source source; };
  void run() {
    auto next = Clock::now();
    while (true) {
      Item item;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        ready_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
        if (stopping_) return;
        ready_.wait_until(lock, next, [this] { return stopping_; });
        if (stopping_) return;
        item = std::move(queue_.front()); queue_.pop_front();
        metrics().queue_size = queue_.size();
      }
      if (Clock::now() - item.enqueued > std::chrono::seconds(5)) { ++metrics().expired; continue; }
      ++metrics().attempts;
      if (item.source == Source::PrivateOverlay) ++metrics().private_attempts;
      auto response = post_message(endpoint, api_key_, item.boc);
      next = Clock::now() + (response.http_status == 429 ? std::chrono::milliseconds(5000) : interval_);
      const char *result;
      if (response.transport_error) { ++metrics().network_errors; result = "network_error"; }
      else if (response.accepted) {
        ++metrics().accepted;
        if (item.source == Source::PrivateOverlay) ++metrics().private_accepted;
        result = "accepted";
      }
      else if (response.http_status == 429) { ++metrics().rate_limited; result = "rate_limited"; }
      else if (response.http_status != 200) { ++metrics().http_errors; result = "http_error"; }
      else { ++metrics().invalid_responses; result = "invalid_response"; }
      LOG(WARNING) << "[toncenter-relay] boc_hash=" << item.hash << " source=" << (item.source == Source::PrivateOverlay ? "private_overlay" : "liteserver")
                   << " result=" << result
                   << " http_status=" << response.http_status << " transport_error=" << response.transport_error
                   << " message_hash=" << response.message_hash;
    }
  }
  bool enabled_, stopping_ = false;
  std::string api_key_;
  std::chrono::milliseconds interval_{1000};
  std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<Item> queue_;
  fullnode::ExternalMessageRelayCache seen_{8192, 60.0};
  std::thread worker_;
};
}
Response post_message(const std::string &url, const std::string &api_key, td::Slice boc) {
  static const auto initialized = curl_global_init(CURL_GLOBAL_DEFAULT);
  Response result;
  if (initialized != CURLE_OK) { result.transport_error = initialized; return result; }
  auto curl = curl_easy_init();
  if (!curl) { result.transport_error = CURLE_FAILED_INIT; return result; }
  auto payload = std::string("{\"boc\":\"") + td::base64_encode(boc) + "\"}";
  std::string body;
  curl_slist *headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  headers = curl_slist_append(headers, "Accept: application/json");
  if (!api_key.empty() && api_key.find_first_of("\r\n") == std::string::npos)
    headers = curl_slist_append(headers, ("X-API-Key: " + api_key).c_str());
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 1000L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 2500L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
  result.transport_error = curl_easy_perform(curl);
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.http_status);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  if (!result.transport_error && result.http_status == 200) {
    auto json = nlohmann::json::parse(body, nullptr, false);
    if (json.is_object() && json.contains("message_hash") && json["message_hash"].is_string()) {
      auto hash = json["message_hash"].get<std::string>();
      auto decoded = td::base64_decode(hash);
      if (decoded.is_ok() && decoded.ok().size() == 32) {
        result.accepted = true; result.message_hash = std::move(hash);
      }
    }
  }
  return result;
}
void submit(td::Slice boc, Source source) { static Relay relay; relay.enqueue(boc, source); }
}
