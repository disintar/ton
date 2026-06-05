/*
    This file is part of TON Blockchain Library.

    TON Blockchain Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    TON Blockchain Library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with TON Blockchain Library.  If not, see <http://www.gnu.org/licenses/>.

    Copyright 2017-2020 Telegram Systems LLP
*/
#pragma once

#include <map>
#include <set>

<<<<<<< .merge_file_V33HoF
#include <unordered_map>
#include "adnl-peer-table.h"
=======
#include "td/actor/MultiPromise.h"
>>>>>>> /var/folders/3k/91ytkdls3l93dl_snvs85g2r0000gn/T/tmp.ZUtm7DxSJj
#include "td/net/TcpListener.h"
#include "td/utils/BufferedFd.h"
#include "td/utils/crypto.h"

#include "adnl-ext-connection.hpp"
#include "adnl-ext-server.h"
#include "adnl-peer-table.h"

namespace ton {

namespace adnl {

class AdnlExtServerImpl;

class AdnlInboundConnection : public AdnlExtConnection {
 public:
  AdnlInboundConnection(td::SocketFd fd, td::actor::ActorId<AdnlPeerTable> peer_table,
                        td::actor::ActorId<AdnlExtServerImpl> ext_server)
      : AdnlExtConnection(std::move(fd), nullptr, false), peer_table_(peer_table), ext_server_(ext_server) {
  }

  AdnlInboundConnection(td::SocketFd fd, td::actor::ActorId<AdnlPeerTable> peer_table,
                        td::actor::ActorId<AdnlExtServerImpl> ext_server, std::shared_ptr<AdnlInboundConnectionCallback> callback)
      : AdnlExtConnection(std::move(fd), nullptr, false), peer_table_(peer_table), callback_(std::move(callback)), ext_server_(ext_server) {
    callback_available = true;
    local_address_.init_peer_address(buffered_fd_);
  }

  td::Status process_packet(td::BufferSlice data) override;
  td::Status process_init_packet(td::BufferSlice data) override;
  td::Status process_custom_packet(td::BufferSlice &data, bool &processed) override;
  void allow_connection(bool allow);
  void inited_crypto(td::Result<td::BufferSlice> R);

  ~AdnlInboundConnection() override {
    if (callback_available){
      callback_->connection_stopped(local_id_, local_address_.get_ip_host());
    }
  }

 private:
  td::actor::ActorId<AdnlPeerTable> peer_table_;
  std::shared_ptr<AdnlInboundConnectionCallback> callback_;
  bool callback_available = false;
  td::actor::ActorId<AdnlExtServerImpl> ext_server_;
  AdnlNodeIdShort local_id_;
  td::IPAddress local_address_;

  td::SecureString nonce_;
  AdnlNodeIdShort remote_id_ = AdnlNodeIdShort::zero();
};

class AdnlExtServerImpl : public AdnlExtServer {
 public:
  void add_tcp_port(td::uint16 port, td::Promise<td::Unit> promise) override;
  void add_local_id(AdnlNodeIdShort id) override;
  void accepted(td::SocketFd fd);
  void stop(std::string ip_addr);
  void set_connection_callback(std::shared_ptr<AdnlInboundConnectionCallback> callback){
    connection_callback_ = std::move(callback);
  }
  void decrypt_init_packet(AdnlNodeIdShort dst, td::BufferSlice data, td::Promise<td::BufferSlice> promise);

  void start_up() override {
    td::MultiPromise mp;
    auto ig = mp.init_guard();
    ig.add_promise(td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
      td::actor::send_closure(SelfId, &AdnlExtServerImpl::initial_ports_bound);
    }));
    for (auto &port : ports_) {
      add_tcp_port(port, ig.get_promise());
    }
    ports_.clear();
    alarm_timestamp() = td::Timestamp::in(1);
  }

<<<<<<< .merge_file_V33HoF
  void alarm() override;
=======
  void initial_ports_bound() {
    promise_.set_value(td::actor::ActorOwn{actor_id(this)});
  }
>>>>>>> /var/folders/3k/91ytkdls3l93dl_snvs85g2r0000gn/T/tmp.ZUtm7DxSJj

  void reopen_port() {
  }

  AdnlExtServerImpl(td::actor::ActorId<AdnlPeerTable> adnl, std::vector<AdnlNodeIdShort> ids,
<<<<<<< .merge_file_V33HoF
                    std::vector<td::uint16> ports)
      : peer_table_(adnl) {
    alarm_timestamp() = td::Timestamp::in(10);

=======
                    std::vector<td::uint16> ports, td::Promise<td::actor::ActorOwn<AdnlExtServer>> promise)
      : promise_(std::move(promise)), peer_table_(adnl) {
>>>>>>> /var/folders/3k/91ytkdls3l93dl_snvs85g2r0000gn/T/tmp.ZUtm7DxSJj
    for (auto &id : ids) {
      add_local_id(id);
    }
    for (auto &port : ports) {
      ports_.insert(port);
    }
  }

 private:
  td::Promise<td::actor::ActorOwn<AdnlExtServer>> promise_;
  td::actor::ActorId<AdnlPeerTable> peer_table_;
  std::shared_ptr<AdnlInboundConnectionCallback> connection_callback_;
  std::set<AdnlNodeIdShort> local_ids_;
  std::set<td::uint16> ports_;
  std::map<td::uint16, td::actor::ActorOwn<td::TcpInfiniteListener>> listeners_;
  std::unordered_map<std::string, int> ip_connection_count_;
  int max_connections_per_ip_ = 3;
};

}  // namespace adnl

}  // namespace ton
