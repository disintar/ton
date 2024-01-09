// Copyright 2023 Disintar LLP / andrey@head-labs.com

#include "PyLiteClient.h"
#include "vm/boc.h"

namespace pylite {

std::string ipv4_int_to_str(int ipv4) {
  return td::IPAddress::ipv4_to_str(ipv4);
}

std::unique_ptr<ton::adnl::AdnlExtClient::Callback> LiteClientActorEngine::make_callback() {
  class Callback : public ton::adnl::AdnlExtClient::Callback {
   public:
    void on_ready() override {
      td::actor::send_closure(id_, &LiteClientActorEngine::conn_ready);
    }
    void on_stop_ready() override {
      td::actor::send_closure(id_, &LiteClientActorEngine::conn_closed);
    }
    Callback(td::actor::ActorId<LiteClientActorEngine> id) : id_(std::move(id)) {
    }

   private:
    td::actor::ActorId<LiteClientActorEngine> id_;
  };
  return std::make_unique<Callback>(actor_id(this));
}

LiteClientActorEngine::LiteClientActorEngine(std::string host, int port, td::Ed25519::PublicKey public_key,
                                             std::shared_ptr<OutputQueue> output_queue_)
    : output_queue(std::move(output_queue_)) {
  adnl_id = ton::adnl::AdnlNodeIdFull{ton::PublicKey(std::move(public_key))};
  remote_addr.init_host_port(host, port).ensure();
}

void LiteClientActorEngine::run() {
  client = ton::adnl::AdnlExtClient::create(std::move(adnl_id), remote_addr, make_callback());
}

ResponseWrapper PyLiteClient::receive_unlocked() {
  if (response_obj_queue_ready_ == 0) {
    response_obj_queue_ready_ = response_obj_->reader_wait_nonblock();
  }
  if (response_obj_queue_ready_ > 0) {
    response_obj_queue_ready_--;
    auto res = response_obj_->reader_get_unsafe();
    // TODO: translate errors;
    return res;
  }
  if (timeout != 0) {
    response_obj_->reader_get_event_fd().wait(static_cast<int>(timeout * 1000));
    return receive_unlocked();
  }

  throw std::logic_error("Timeout on receive request");
}

void LiteClientActorEngine::get_time() {
  auto query = ton::create_tl_object<ton::lite_api::liteServer_getTime>();
  auto q = ton::create_tl_object<ton::lite_api::liteServer_query>(ton::serialize_tl_object(query, true));

  td::actor::send_closure(
      client, &ton::adnl::AdnlExtClient::send_query, "query", serialize_tl_object(q, true), td::Timestamp::in(10.0),
      [&](td::Result<td::BufferSlice> res) -> void {
        if (res.is_error()) {
          output_queue->writer_put(
              ResponseWrapper(std::make_unique<GetTimeResponse>(GetTimeResponse(0, false, "cannot get server time"))));
          return;
        } else {
          auto F = ton::fetch_tl_object<ton::lite_api::liteServer_currentTime>(res.move_as_ok(), true);

          if (F.is_error()) {
            output_queue->writer_put(ResponseWrapper(std::make_unique<GetTimeResponse>(
                GetTimeResponse(0, false, "cannot parse answer to liteServer.getTime"))));
          } else {
            int x = F.move_as_ok()->now_;
            GetTimeResponse t{x, true, ""};
            output_queue->writer_put(ResponseWrapper(std::make_unique<GetTimeResponse>(t)));
          }
        }
      });
}

void LiteClientActorEngine::get_MasterchainInfoExt(int mode) {
  auto query = ton::create_tl_object<ton::lite_api::liteServer_getMasterchainInfoExt>();
  auto q = ton::create_tl_object<ton::lite_api::liteServer_query>(ton::serialize_tl_object(query, true));

  td::actor::send_closure(client, &ton::adnl::AdnlExtClient::send_query, "query", serialize_tl_object(q, true),
                          td::Timestamp::in(10.0), [&](td::Result<td::BufferSlice> res) -> void {
                            if (res.is_error()) {
                              output_queue->writer_put(ResponseWrapper(
                                  std::make_unique<ResponseObj>(ResponseObj(false, "Error while fetch"))));
                              return;
                            } else {
                              auto F = res.move_as_ok();
                              std::unique_ptr<td::BufferSlice> x = std::make_unique<td::BufferSlice>(std::move(F));

                              output_queue->writer_put(ResponseWrapper(
                                  std::make_unique<GetMasterchainInfoExt>(GetMasterchainInfoExt(std::move(x)))));
                            }
                          });
}

void LiteClientActorEngine::send_message(vm::Ref<vm::Cell> cell) {
  auto q = ton::serialize_tl_object(
      ton::create_tl_object<ton::lite_api::liteServer_sendMessage>(vm::std_boc_serialize(cell).move_as_ok()), true);

  //  auto q = ton::create_tl_object<ton::lite_api::liteServer_query>(ton::serialize_tl_object(query, true));

  td::actor::send_closure(
      client, &ton::adnl::AdnlExtClient::send_query, "query", std::move(q), td::Timestamp::in(0.01),
      [&](td::Result<td::BufferSlice> res) -> void {
        if (res.is_error()) {
          output_queue->writer_put(
              ResponseWrapper(std::make_unique<ResponseObj>(ResponseObj(false, res.move_as_error().to_string()))));
          return;
        } else {
          output_queue->writer_put(ResponseWrapper(std::make_unique<ResponseObj>(ResponseObj(false, "all good"))));
        }
      });
}

std::unique_ptr<ResponseObj> PyLiteClient::wait_response() {
  auto is_locked = receive_lock_.exchange(true);
  CHECK(!is_locked);
  auto response = receive_unlocked();
  is_locked = receive_lock_.exchange(false);
  CHECK(is_locked);
  return std::move(response.object);
}

}  // namespace pylite