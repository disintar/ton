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

namespace ton::validator {

class BlockParserAsync : public td::actor::Actor {
 public:
  BlockParserAsync(BlockIdExt id_, ConstBlockHandle handle_, td::Ref<BlockData> data_, td::Ref<vm::Cell> state_,
                   td::optional<td::Ref<vm::Cell>> prev_state_, td::Promise<std::tuple<td::string, td::string>> P_) {
    id = id_;
    handle = std::move(handle_);
    data = std::move(data_);
    state = std::move(state_);
    prev_state = std::move(prev_state_);
    P = std::move(P_);
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
  void parse_shard(ton::BlockIdExt shard_id, bool pad = true);
  void parse_other();
  void receive_shard_handle(ConstBlockHandle handle);
  void receive_block(ConstBlockHandle handle, td::Ref<BlockData> block);
  void receive_states(ConstBlockHandle handle, td::Ref<BlockData> block, td::Ref<vm::Cell> state);
  void start_wait_next(BlockSeqno block);
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
  const int k = 100;
  int next_download = 100;
};

}  // namespace ton::validator