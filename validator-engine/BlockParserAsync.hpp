//
// Created by Andrey Tvorozhkov on 5/25/23.
//

#include "td/utils/logging.h"
#include "td/actor/actor.h"
#include "td/utils/Time.h"
#include "td/utils/filesystem.h"
#include "td/utils/JsonBuilder.h"
#include "auto/tl/ton_api_json.h"
#include "crypto/vm/cp0.h"
#include "validator/validator.h"
#include "validator/manager-disk.h"
#include "ton/ton-types.h"
#include "ton/ton-tl.hpp"
#include "tl/tlblib.hpp"
#include "block/block.h"
#include "block/block-parse.h"
#include "block/block-auto.h"
#include "vm/dict.h"
#include "vm/cells/MerkleProof.h"
#include "vm/vm.h"
#include "td/utils/Slice.h"
#include "td/utils/common.h"
#include "td/utils/OptionParser.h"
#include "td/utils/port/user.h"
#include <utility>
#include <fstream>
#include "auto/tl/lite_api.h"
#include "adnl/utils.hpp"
#include "tuple"
#include "crypto/block/mc-config.h"
#include <algorithm>
#include <queue>
#include <chrono>
#include <thread>
#include "validator/interfaces/validator-manager.h"
#include "blockchain-indexer/json.hpp"
#include "blockchain-indexer/json-utils.hpp"

namespace ton::validator {

    class AsyncStateIndexer : public td::actor::Actor {
        td::unique_ptr<vm::AugmentedDictionary> accounts;
        std::vector<json> json_accounts;
        std::string block_id_string;
        td::Timer timer;
        BlockIdExt block_id;
        std::mutex accounts_mtx_;
        std::mutex accounts_count_mtx_;
        unsigned long total_accounts;
        json answer;
        td::Promise<std::string> final_promise;
        vm::Ref<vm::Cell> root_cell;
        td::optional<td::Ref<vm::Cell>> prev_root_cell;
        std::vector<std::pair<td::Bits256, int>> accounts_keys;
        bool with_prev_state;
        td::unique_ptr<vm::AugmentedDictionary> prev_accounts;

    public:
        AsyncStateIndexer(std::string block_id_string_, vm::Ref<vm::Cell> root_cell_,
                          td::optional<td::Ref<vm::Cell>> prev_root_cell_,
                          std::vector<std::pair<td::Bits256, int>> accounts_keys_, BlockIdExt block_id_,
                          td::Promise<std::string> final_promise_) {
            block_id = block_id_;
            block_id_string = std::move(block_id_string_);
            total_accounts = accounts_keys_.size();
            root_cell = std::move(root_cell_);
            with_prev_state = true;
            if (!prev_root_cell_) {
                with_prev_state = false;
            }

            LOG(INFO) << "Parse state: " << block_id.id.to_str() << " with prev state: " << with_prev_state;

            prev_root_cell = std::move(prev_root_cell_);
            accounts_keys = std::move(accounts_keys_);
            final_promise = std::move(final_promise_);
        }

        void start_up() override;

        void processAccount(td::Bits256 account, int tx_count);

        bool finalize();
    };

    class BlockParserAsync : public td::actor::Actor {
    public:
        BlockParserAsync(BlockIdExt id_, ConstBlockHandle handle_, td::Ref<BlockData> data_, td::Ref<vm::Cell> state_,
                         td::optional<td::Ref<vm::Cell>> prev_state_,
                         td::Promise<std::tuple<td::string, td::string>> P_,
                         td::Promise<std::vector<json>> out_messages_promise_) {
            id = id_;
            handle = std::move(handle_);
            data = std::move(data_);
            state = std::move(state_);
            prev_state = std::move(prev_state_);
            P = std::move(P_);
            out_messages_promise = std::move(out_messages_promise_);
        }

        void start_up() override {
            td::actor::send_closure(actor_id(this), &BlockParserAsync::parseBlockData);
        }

        void parseBlockData();

        void saveStateData(std::string tmp_state);

        void finalize();

    private:
        BlockIdExt id;
        ConstBlockHandle handle;
        td::Ref<BlockData> data;
        td::Ref<vm::Cell> state;
        td::optional<td::Ref<vm::Cell>> prev_state;
        td::Promise<std::tuple<td::string, td::string>> P;
        td::Promise<std::vector<json>> out_messages_promise;
        std::string parsed_data;
        std::string parsed_state;
    };

    class StartupBlockParser : public td::actor::Actor {
    public:
        StartupBlockParser(td::actor::ActorId<ValidatorManagerInterface> validator_id,
                           BlockHandle last_masterchain_block_handle_,
                           td::Promise<std::tuple<std::vector<ConstBlockHandle>, std::vector<td::Ref<BlockData>>,
                                   std::vector<td::Ref<vm::Cell>>, std::vector<td::Ref<vm::Cell>>>>
                           P_) {
            LOG(WARNING) << "Start worker StartupBlockParser";

            ConstBlockHandle h(last_masterchain_block_handle_);
            last_masterchain_block_handle = std::move(h);
            manager = std::move(validator_id);
            P_final = std::move(P_);


            const char *env_var_value = std::getenv("STARTUP_BLOCKS_DOWNLOAD_BEFORE");
            if (env_var_value == nullptr) {
                LOG(WARNING) << "Environment variable " << "STARTUP_BLOCKS_DOWNLOAD_BEFORE" << " is not set.";
                k = 1;
            } else {
                try {
                    k = std::stoi(env_var_value);
                } catch (const std::invalid_argument &e) {
                    LOG(WARNING) << "Invalid value for environment variable" << "STARTUP_BLOCKS_DOWNLOAD_BEFORE"
                                 << ": not an integer.";
                    k = 1;
                } catch (const std::out_of_range &e) {
                    LOG(WARNING) << "Invalid value for environment variable " << "STARTUP_BLOCKS_DOWNLOAD_BEFORE"
                                 << ": out of range.";
                    k = 1;
                }
            }

            const char *env_var_value_2 = std::getenv("STARTUP_BLOCKS_DOWNLOAD_AFTER");
            if (env_var_value_2 == nullptr) {
                LOG(WARNING) << "Environment variable " << "STARTUP_BLOCKS_DOWNLOAD_AFTER" << " is not set.";
                next_download = 1;
            } else {
                try {
                    next_download = std::stoi(env_var_value_2);
                } catch (const std::invalid_argument &e) {
                    LOG(WARNING) << "Invalid value for environment variable" << "STARTUP_BLOCKS_DOWNLOAD_AFTER"
                                 << ": not an integer.";
                    next_download = 1;
                } catch (const std::out_of_range &e) {
                    LOG(WARNING) << "Invalid value for environment variable " << "STARTUP_BLOCKS_DOWNLOAD_AFTER"
                                 << ": out of range.";
                    next_download = 1;
                }
            }
        }

        void start_up() override {
            // separate first parse seqno to prevent WC shard seqno leak
            auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<ConstBlockHandle> R) {
                if (R.is_error()) {
                    auto err = R.move_as_error();
                    LOG(ERROR) << "failed query: " << err;
                    td::actor::send_closure(SelfId, &StartupBlockParser::end_with_error, std::move(err));
                } else {
                    auto handle = R.move_as_ok();
                    LOG(WARNING) << "got latest data handle for block " << handle->id().to_str();
                    td::actor::send_closure(SelfId, &StartupBlockParser::receive_first_handle, handle);
                }
            });

            ton::AccountIdPrefixFull pfx{-1, 0x8000000000000000};
            LOG(WARNING) << "Run get block";
            td::actor::send_closure(manager, &ValidatorManagerInterface::get_block_by_seqno_from_db, pfx,
                                    last_masterchain_block_handle->id().seqno() - k, std::move(P));
        }

        void receive_first_handle(std::shared_ptr<const BlockHandleInterface> handle);

        void end_with_error(td::Status err);

        void receive_handle(std::shared_ptr<const BlockHandleInterface> handle);

        void parse_shard(ton::BlockIdExt shard_id, bool pad = true, bool sleep = false);

        void parse_other();

        void receive_shard_handle(ConstBlockHandle handle);

        void receive_block(ConstBlockHandle handle, td::Ref<BlockData> block);

        void receive_states(ConstBlockHandle handle, td::Ref<BlockData> block, td::Ref<vm::Cell> state);

        void start_wait_next(BlockSeqno block, bool sleep = false);

        void set_next_ready(ConstBlockHandle b);

        void request_prev_state(ConstBlockHandle handle, td::Ref<BlockData> block, td::Ref<vm::Cell> state,
                                std::shared_ptr<const BlockHandleInterface> prev_handle);

        void request_prev_state_final(ConstBlockHandle handle, td::Ref<BlockData> block, td::Ref<vm::Cell> state,
                                      td::Ref<vm::Cell> prev_state);

        void pad();

        void ipad();

    private:
        ConstBlockHandle last_masterchain_block_handle;
        td::Promise<std::tuple<std::vector<ConstBlockHandle>, std::vector<td::Ref<BlockData>>, std::vector<td::Ref<vm::Cell>>,
                std::vector<td::Ref<vm::Cell>>>>
                P_final;
        td::actor::ActorId<ValidatorManagerInterface> manager;
        std::vector<ConstBlockHandle> block_handles;
        std::vector<td::Ref<BlockData>> blocks;
        std::vector<td::Ref<vm::Cell>> states;
        std::vector<td::Ref<vm::Cell>> prev_states;
        std::vector<std::string> parsed_shards;
        int padding = 0;
        int k;
        int next_download;
    };

}  // namespace ton::validator