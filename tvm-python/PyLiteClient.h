// Copyright 2023 Disintar LLP / andrey@head-labs.com
#include "adnl/adnl-ext-client.h"
#include "PyKeys.h"
#include "td/utils/MpscPollableQueue.h"
#include "tl/generate/auto/tl/lite_api.h"
#include "ton/lite-tl.hpp"
#include "tl-utils/lite-utils.hpp"
#include "tl/generate/auto/tl/tonlib_api_json.h"
#include "tl/generate/auto/tl/lite_api.h"
#include "tl/tl/tl_json.h"
#include "tl-utils/common-utils.hpp"
#include "tl/tl/TlObject.h"
#include "td/actor/PromiseFuture.h"

#ifndef TON_PYLITECLIENT_H
#define TON_PYLITECLIENT_H

namespace pylite {

std::string ipv4_int_to_str(int ipv4);

// Response objects

class ResponseObj {
 public:
  ResponseObj(bool success_, std::string error_message_ = "")
      : success(success_), error_message(std::move(error_message_)) {
  }
  virtual ~ResponseObj() {
  }
  bool success;
  std::string error_message;
};

class ResponseWrapper {
 public:
  ResponseWrapper(std::unique_ptr<ResponseObj> object_) : object(std::move(object_)) {
  }
  std::unique_ptr<ResponseObj> object;
};

class Connected : public ResponseObj {
 public:
  Connected(bool c) : ResponseObj(true), connected(c){};
  bool connected;
};

class GetTimeResponse : public ResponseObj {
 public:
  GetTimeResponse(std::int32_t t, bool success, std::string error_message = "")
      : ResponseObj(success, std::move(error_message)), now(t) {
  }
  std::int32_t now;
};

class GetMasterchainInfoExt : public ResponseObj {
 public:
  GetMasterchainInfoExt(std::unique_ptr<td::BufferSlice> obj_)
      : ResponseObj(true, std::move("")), obj(std::move(obj_)) {
  }
  std::unique_ptr<td::BufferSlice> obj;
};

// Actor

using OutputQueue = td::MpscPollableQueue<pylite::ResponseWrapper>;

class LiteClientActorEngine : public td::actor::Actor {
 public:
  LiteClientActorEngine(std::string host, int port, td::Ed25519::PublicKey public_key,
                        std::shared_ptr<OutputQueue> output_queue_);

  void conn_ready() {
    connected = true;
  }

  void conn_closed() {
    connected = false;
  }

  void get_connected() {
    output_queue->writer_put(ResponseWrapper(std::make_unique<Connected>(Connected(connected))));
  }

  void get_time();
  void get_MasterchainInfoExt(int mode);

  void run();

 private:
  ton::adnl::AdnlNodeIdFull adnl_id;
  td::IPAddress remote_addr;
  std::shared_ptr<OutputQueue> output_queue;
  bool connected = false;
  td::actor::ActorOwn<ton::adnl::AdnlExtClient> client;
  std::unique_ptr<ton::adnl::AdnlExtClient::Callback> make_callback();
};

// Python wrapper
// The idea of communication with actors from tonlib/tonlib/Client.h

class PyLiteClient {
 public:
  td::actor::ActorOwn<LiteClientActorEngine> engine;
  double timeout;

  PyLiteClient(std::string ipv4, int port, PyPublicKey public_key, double timeout_ = 5) : timeout(timeout_) {
    response_obj_ = std::make_shared<OutputQueue>();
    response_obj_->init();

    scheduler_.run_in_context([&] {
      engine = td::actor::create_actor<LiteClientActorEngine>("LiteClientActorEngine", ipv4, port,
                                                              std::move(public_key.key), response_obj_);

      scheduler_.run_in_context_external([&] { send_closure(engine, &LiteClientActorEngine::run); });
    });
    scheduler_thread_ = td::thread([&] { scheduler_.run(); });
  };

  ~PyLiteClient() {
    scheduler_.run_in_context_external([&] { engine.reset(); });
    scheduler_.run_in_context_external([] { td::actor::SchedulerContext::get()->stop(); });
    scheduler_thread_.join();
  }

  bool get_connected() {
    scheduler_.run_in_context_external([&] { send_closure(engine, &LiteClientActorEngine::get_connected); });
    auto response = wait_response();
    Connected* connected = static_cast<Connected*>(response.get());
    return connected->connected;
  }

  std::int32_t get_time() {
    scheduler_.run_in_context_external([&] { send_closure(engine, &LiteClientActorEngine::get_time); });
    auto response = wait_response();
    GetTimeResponse* time = dynamic_cast<GetTimeResponse*>(response.get());
    if (time->success) {
      return time->now;
    } else {
      throw std::logic_error(time->error_message);
    }
  }

  std::unique_ptr<ton::lite_api::liteServer_masterchainInfoExt> get_MasterchainInfoExt() {
    if (!get_connected()) {
      throw std::logic_error("Client not connected");
    }
    scheduler_.run_in_context_external(
        [&] { send_closure(engine, &LiteClientActorEngine::get_MasterchainInfoExt, 0); });

    auto response = wait_response();
    if (response->success) {
      GetMasterchainInfoExt* data = dynamic_cast<GetMasterchainInfoExt*>(response.get());
      auto x =
          ton::fetch_tl_object<ton::lite_api::liteServer_masterchainInfoExt>(data->obj->clone(), true).move_as_ok();
      return std::move(x);
    } else {
      throw std::logic_error("Request made with error");
    }
  }

 private:
  std::shared_ptr<OutputQueue> response_obj_;
  std::atomic<bool> receive_lock_{false};
  int response_obj_queue_ready_ = 0;

  td::actor::Scheduler scheduler_{{6}};
  td::thread scheduler_thread_;

  std::unique_ptr<ResponseObj> wait_response();
  ResponseWrapper receive_unlocked();
};
}  // namespace pylite

#endif  //TON_PYLITECLIENT_H
