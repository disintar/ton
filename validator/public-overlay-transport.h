#pragma once

#include <set>

#include "adnl/adnl.h"
#include "block-propagation-trace.h"

namespace ton::validator::fullnode {

// Keep transport selection per destination when a download retries another peer.
class PublicOverlayTransport final : public adnl::AdnlSenderInterface {
 public:
  PublicOverlayTransport(td::actor::ActorId<adnl::AdnlSenderInterface> rldp,
                         td::actor::ActorId<adnl::AdnlSenderInterface> quic)
      : rldp_(rldp), quic_(quic) {
  }

  void set_peer_quic(adnl::AdnlNodeIdShort peer, bool enabled) {
    bool changed = enabled ? quic_peers_.insert(peer).second : quic_peers_.erase(peer) != 0;
    if (changed && private_sync_trace_should_log(false, 0)) {
      LOG(WARNING) << "[public-transport] stage=peer.capability peer=" << peer
                   << " transport=" << (enabled ? "quic" : "rldp2");
    }
  }

  void send_message(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data) override {
    td::actor::send_closure(sender(dst), &adnl::AdnlSenderInterface::send_message, src, dst, std::move(data));
  }

  void send_query(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, std::string name,
                  td::Promise<td::BufferSlice> promise, td::Timestamp timeout, td::BufferSlice data) override {
    td::actor::send_closure(sender(dst), &adnl::AdnlSenderInterface::send_query, src, dst, std::move(name),
                            std::move(promise), timeout, std::move(data));
  }

  void send_query_ex(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, std::string name,
                     td::Promise<td::BufferSlice> promise, td::Timestamp timeout, td::BufferSlice data,
                     td::uint64 max_answer_size) override {
    bool use_quic = quic_peers_.contains(dst);
    auto started_at = td::Time::now();
    auto traced = td::PromiseCreator::lambda(
        [dst, use_quic, name, started_at, promise = std::move(promise)](td::Result<td::BufferSlice> result) mutable {
          auto elapsed_ms = static_cast<long long>((td::Time::now() - started_at) * 1000.0);
          if (private_sync_trace_should_log(result.is_error(), elapsed_ms)) {
            LOG(WARNING) << "[public-transport] stage=query.done peer=" << dst
                         << " transport=" << (use_quic ? "quic" : "rldp2") << " query=" << name
                         << " ms=" << elapsed_ms << " result=" << (result.is_ok() ? "ok" : "error")
                         << " reason=" << (result.is_error() ? result.error().to_string() : "-");
          }
          promise.set_result(std::move(result));
        });
    td::actor::send_closure(use_quic ? quic_ : rldp_, &adnl::AdnlSenderInterface::send_query_ex, src, dst,
                            std::move(name), std::move(traced), timeout, std::move(data), max_answer_size);
  }

  void get_conn_ip_str(adnl::AdnlNodeIdShort local, adnl::AdnlNodeIdShort peer,
                       td::Promise<td::string> promise) override {
    td::actor::send_closure(sender(peer), &adnl::AdnlSenderInterface::get_conn_ip_str, local, peer,
                            std::move(promise));
  }

 private:
  td::actor::ActorId<adnl::AdnlSenderInterface> sender(adnl::AdnlNodeIdShort peer) const {
    return quic_peers_.contains(peer) ? quic_ : rldp_;
  }

  td::actor::ActorId<adnl::AdnlSenderInterface> rldp_;
  td::actor::ActorId<adnl::AdnlSenderInterface> quic_;
  std::set<adnl::AdnlNodeIdShort> quic_peers_;
};

}  // namespace ton::validator::fullnode
