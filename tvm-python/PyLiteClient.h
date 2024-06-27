// Copyright 2023 Disintar LLP / andrey@head-labs.com
#include "adnl/adnl-ext-client.h"
#include "PyKeys.h"
#include "PyCell.h"
#include "PyDict.h"
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
#include "block/check-proof.h"
#include "lite-client/lite-client.h"
#include "tl/generate/auto/tl/tonlib_api.h"
#include "tonlib/tonlib/TonlibClient.h"
#include "third-party/pybind11/include/pybind11/embed.h"


#ifndef TON_PYLITECLIENT_H
#define TON_PYLITECLIENT_H

namespace pylite {

    std::string ipv4_int_to_str(int ipv4);

// Response objects

    using PubKeyHex = std::string;
    using ShortKeyHex = std::string;

    void throw_lite_error(td::BufferSlice b);

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
        Connected(bool c) : ResponseObj(true), connected(c) {};
        bool connected;
    };

    class GetTimeResponse : public ResponseObj {
    public:
        GetTimeResponse(std::int32_t t, bool success, std::string error_message = "")
                : ResponseObj(success, std::move(error_message)), now(t) {
        }

        std::int32_t now;
    };

    struct BlockTransactionsExt {
        ton::BlockIdExt id;
        int req_count;
        bool incomplete;
        std::vector<PyCell> transactions;
    };

    class SuccessBufferSlice : public ResponseObj {
    public:
        SuccessBufferSlice(std::unique_ptr<td::BufferSlice> obj_) : ResponseObj(true, std::move("")),
                                                                    obj(std::move(obj_)) {
        }

        std::unique_ptr<td::BufferSlice> obj;
    };

// Actor

    using OutputQueue = td::MpscPollableQueue<pylite::ResponseWrapper>;

    class LiteClientActorEngine : public td::actor::Actor {
    public:
        LiteClientActorEngine(std::string host, int port, td::Ed25519::PublicKey public_key,
                              std::shared_ptr<OutputQueue> output_queue_, double timeout_);

        void conn_ready() {
            connected = true;
        }

        void conn_closed() {
            connected = false;
        }

        void get_connected() {
            output_queue->writer_put(ResponseWrapper(std::make_unique<Connected>(Connected(connected))));
        }

        void send_message(vm::Ref<vm::Cell> message);

        void lookupBlock(int mode, ton::BlockId block, long long lt, long long time);

        void ready();

        void wait_connected(double wait);

        void get_time();

        void get_MasterchainInfoExt(int mode);

        void get_AccountState(int workchain, td::Bits256 address, ton::BlockIdExt blkid);

        void
        get_Transactions(int count, int workchain, td::Bits256 address_bits, unsigned long long lt, td::Bits256 hash);

        void get_OneTransaction(ton::BlockIdExt blkid, int workchain, td::Bits256 address_bits, unsigned long long lt);

        void get_ConfigAll(int mode, ton::BlockIdExt blkid);

        void get_BlockHeader(ton::BlockIdExt blkid, int mode);

        void get_Block(ton::BlockIdExt blkid);

        void get_AllShardsInfo(ton::BlockIdExt blkid);

        void get_Libraries(std::vector<td::Bits256> libs);

        void get_listBlockTransactionsExt(ton::BlockIdExt blkid, int mode, int count,
                                          std::optional<td::Bits256> account = std::optional<td::Bits256>(),
                                          std::optional<unsigned long long> lt = std::optional<unsigned long long>());

        void admin_AddUser(td::Bits256 pubkey, td::int64 valid_until, td::int32 ratelimit);

        void admin_GetStatData();

        void wait_masterchain_seqno(int seqno, int tm);

        void run();

        void exit() {
            client.reset();
            hangup();
        };

    private:
        ton::adnl::AdnlNodeIdFull adnl_id;
        td::IPAddress remote_addr;
        std::shared_ptr<OutputQueue> output_queue;
        bool connected = false;
        td::actor::ActorOwn<ton::adnl::AdnlExtClient> client;

        std::unique_ptr<ton::adnl::AdnlExtClient::Callback> make_callback();

        void qprocess(td::BufferSlice q);

        void admin_qprocess(td::BufferSlice q);

        td::Timestamp wait_timeout_;
        double timeout;
    };

// Python wrapper
// The idea of communication with actors from tonlib/tonlib/Client.h

    class PyLiteClient {
    public:
        td::actor::ActorOwn<LiteClientActorEngine> engine;
        double timeout;

        PyLiteClient(std::string ipv4, int port, PyPublicKey public_key, double timeout_ = 5,
                     unsigned long long threads = 5)
                : timeout(timeout_) {
            scheduler_.init_with_new_infos({{threads}});
            response_obj_ = std::make_shared<OutputQueue>();
            response_obj_->init();

            scheduler_.run_in_context([&] {
                engine = td::actor::create_actor<LiteClientActorEngine>("LiteClientActorEngine", ipv4, port,
                                                                        std::move(public_key.key), response_obj_,
                                                                        timeout_);

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
            Connected *connected = static_cast<Connected *>(response.get());
            return connected->connected;
        }

        std::int32_t get_time() {
            scheduler_.run_in_context_external([&] { send_closure(engine, &LiteClientActorEngine::get_time); });
            auto response = wait_response();
            GetTimeResponse *time = dynamic_cast<GetTimeResponse *>(response.get());
            if (time->success) {
                return time->now;
            } else {
                throw std::logic_error(time->error_message);
            }
        }

        int send_message(PyCell &cell) {
            scheduler_.run_in_context_external(
                    [&] { send_closure(engine, &LiteClientActorEngine::send_message, cell.my_cell); });
            auto response = wait_response();
            if (response->success) {
                SuccessBufferSlice *answer = dynamic_cast<SuccessBufferSlice *>(response.get());
                auto R = ton::fetch_tl_object < ton::lite_api::liteServer_sendMsgStatus >
                         (std::move(answer->obj->clone()), true);
                if (R.is_error()) {
                    throw_lite_error(answer->obj->clone());
                } else {
                    return R.move_as_ok()->status_;
                }
            } else {
                throw std::logic_error(response->error_message);
            }
            return 0;
        }

        std::unique_ptr<ton::lite_api::liteServer_masterchainInfoExt> get_MasterchainInfoExt();

        std::unique_ptr<block::AccountState::Info> get_AccountState(int workchain, std::string address_string,
                                                                    ton::BlockIdExt &blk);

        std::pair<ton::BlockIdExt, PyCell> get_ConfigAll(int mode, ton::BlockIdExt block,
                                                         bool force_check_on_key_block = true);

        block::TransactionList::Info get_Transactions(int count, int workchain, std::string address_string,
                                                      unsigned long long lt, std::string hash_int_string);

        PyCell
        get_OneTransaction(ton::BlockIdExt blkid, int workchain, std::string address_string, unsigned long long lt);

        TestNode::BlockHdrInfo get_BlockHeader(ton::BlockIdExt blkid, int mode);

        TestNode::BlockHdrInfo lookupBlock(int mode, ton::BlockId block, long long lt, long long time);

        PyCell get_Block(ton::BlockIdExt blkid);

        PyDict get_Libraries(std::vector<std::string> libs);

        BlockTransactionsExt get_listBlockTransactionsExt(
                ton::BlockIdExt blkid, int mode, int count,
                std::optional<td::string> account = std::optional<std::string>(),
                std::optional<unsigned long long> lt = std::optional<unsigned long long>());

        std::vector<ton::BlockId> get_AllShardsInfo(ton::BlockIdExt req_blkid);

        bool wait_connected(double wait);

        bool dummy_wait(int wait) {
            py::gil_scoped_release release;
            sleep(wait);
            return false;
        };

        std::unique_ptr<ton::lite_api::liteServer_masterchainInfoExt> wait_masterchain_seqno(int seqno, int tm);

        // Admin functions
        std::tuple<PubKeyHex, ShortKeyHex>
        admin_AddUser(std::string pubkey, td::int64 valid_until, td::int32 ratelimit);

        std::vector<std::tuple<ShortKeyHex, int, td::int64, td::int64, bool>> admin_getStatData();

        void stop() {
            scheduler_.run_in_context_external([&] { engine.reset(); });
            scheduler_.run_in_context_external([] { td::actor::SchedulerContext::get()->stop(); });
            scheduler_thread_.join();
            scheduler_.stop();
        }

    private:
        std::shared_ptr<OutputQueue> response_obj_;
        std::atomic<bool> receive_lock_{false};
        int response_obj_queue_ready_ = 0;

        td::actor::Scheduler scheduler_{{6}, false, td::actor::Scheduler::Mode::Wait};
        td::thread scheduler_thread_;

        std::unique_ptr<ResponseObj> wait_response();

        ResponseWrapper receive_unlocked();
    };
}  // namespace pylite

#endif  //TON_PYLITECLIENT_H
