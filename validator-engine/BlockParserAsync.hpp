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

namespace ton::validator {

class BlockParserAsync : public td::actor::Actor {
 public:
  BlockParserAsync(BlockIdExt id_, BlockHandle handle_, td::Ref<BlockData> data_, td::Ref<vm::Cell> state_,
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
  BlockHandle handle;
  td::Ref<BlockData> data;
  td::Ref<vm::Cell> state;
  td::optional<td::Ref<vm::Cell>> prev_state;
  td::Promise<std::tuple<td::string, td::string>> P;
  std::string parsed_data;
  std::string parsed_state;
};

}  // namespace ton::validator