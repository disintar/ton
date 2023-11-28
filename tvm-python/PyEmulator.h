// Copyright 2023 Disintar LLP / andrey@head-labs.com

#include "third-party/pybind11/include/pybind11/pybind11.h"
#include <string>
#include "td/utils/base64.h"
#include <utility>
#include "vm/boc.h"
#include "vm/cellslice.h"
#include "crypto/vm/cellslice.h"
#include <queue>
#include "block/block.h"
#include "block/block-auto.h"
#include "tvm-python/PyCellSlice.h"
#include "PyCell.h"
#include "emulator/transaction-emulator.h"
#include "emulator/tvm-emulator.hpp"
#include "td/utils/base64.h"
#include "td/utils/Status.h"
#include "td/utils/JsonBuilder.h"
#include "td/utils/logging.h"
#include "td/utils/Variant.h"
#include "td/utils/overloaded.h"
#include "transaction-emulator.h"
#include "tvm-emulator.hpp"
#include "crypto/vm/stack.hpp"

#ifndef TON_PYEMULATOR_H
#define TON_PYEMULATOR_H

class PyEmulator {
 public:
  td::unique_ptr<emulator::TransactionEmulator> emulator;

  // Result:
  td::Ref<vm::Cell> transaction_cell;
  td::Ref<vm::Cell> account_cell;
  td::Ref<vm::Cell> actions_cell;
  std::string vm_log;
  double elapsed_time;
  int vm_exit_code;

  PyEmulator(const PyCell& config_params_cell) {
    // todo: pass ConfigParams as root cell
    const block::StdAddress res =
        block::StdAddress::parse("-1:5555555555555555555555555555555555555555555555555555555555555555").move_as_ok();

    auto global_config = block::Config(
        config_params_cell.my_cell, res.addr,
        block::Config::needWorkchainInfo | block::Config::needSpecialSmc | block::Config::needCapabilities);

    if (global_config.unpack().is_error()) {
      throw std::invalid_argument("Can't unpack config params");
    }

    emulator = td::make_unique<emulator::TransactionEmulator>(std::move(global_config), 0);
  };

  ~PyEmulator() = default;

  bool set_rand_seed(const std::string& rand_seed_hex);
  bool set_ignore_chksig(bool ignore_chksig);
  bool set_libs(const PyCell& shardchain_libs_cell);
  bool set_debug_enabled(bool debug_enabled);
  bool emulate_transaction(const PyCell& shard_account_cell, const PyCell& message_cell,
                           const std::string& unixtime = "0", const std::string& lt_str = "0", int vm_ver = 1, bool force_uninit = false);
  bool emulate_tick_tock_transaction(const PyCell& shard_account_boc, bool is_tock, const std::string& unixtime,
                                     const std::string& lt_str, int vm_ver);
  std::string get_vm_log();
  double get_elapsed_time();
  PyCell get_transaction_cell();
  PyCell get_account_cell();
  PyCell get_actions_cell();

  static void dummy_set() {
    throw std::invalid_argument("Not settable");
  }
};

#endif  //TON_PYEMULATOR_H
