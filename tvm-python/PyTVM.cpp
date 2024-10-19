// Copyright 2023 Disintar LLP / andrey@head-labs.com

#include "third-party/pybind11/include/pybind11/pybind11.h"
#include <string>
#include "vm/vmstate.h"
#include "td/utils/base64.h"
#include <utility>
#include <optional>
#include "vm/boc.h"
#include "vm/cp0.h"
#include "vm/cellslice.h"
#include "block/block.h"
#include "crypto/vm/cellslice.h"
#include "crypto/block/mc-config.h"
#include <queue>
#include "PyCellSlice.h"
#include "PyCell.h"
#include "PyCellBuilder.h"
#include "PyDict.h"
#include "PyStack.h"
#include "PyTVM.h"
#include "third-party/pybind11/include/pybind11/embed.h"


namespace py = pybind11;

void PyTVM::set_c7(PyStackEntry x) {
  if (!skip_c7) {
    c7 = x.entry;
  } else {
    throw std::invalid_argument("C7 will be skipped, because skip_c7=true");
  }
}

void PyTVM::log(const std::string &log_string, int level) {
  if (log_level >= level && level == LOG_INFO) {
    py::print("INFO: " + log_string);
  } else if (log_level >= level && level == LOG_DEBUG) {
    py::print("DEBUG: " + log_string);
  }
}

void PyTVM::set_gasLimit(const std::string &gas_limit_s, const std::string &gas_max_s) {
  auto gas_limit = strtoll(gas_limit_s.c_str(), nullptr, 10);

  if (gas_max_s.empty()) {
    gas_limits = vm::GasLimits{gas_limit, gas_limit};
  } else {
    auto gas_max = strtoll(gas_max_s.c_str(), nullptr, 10);

    gas_limits = vm::GasLimits{gas_limit, gas_max};
  }
}

void PyTVM::set_data(PyCell data_) {
  if (data_.is_null()) {
    throw std::invalid_argument("Data root need to have at least 1 root cell");
  }

  data = data_;

  log_debug("Data loaded: " + data.my_cell->get_hash().to_hex());
}

void PyTVM::set_code(PyCell code_) {
  if (code_.is_null()) {
    throw std::invalid_argument("Code root need to have at least 1 root cell");
  }

  code = code_;
  log_debug("Code loaded: " + code.my_cell->get_hash().to_hex());
}

void PyTVM::set_state_init(PyCell state_init_) {
  auto state_init = load_cell_slice(state_init_.my_cell);
  code = PyCell(state_init.fetch_ref());
  data = PyCell(state_init.fetch_ref());
}

void PyTVM::set_stack(PyStack pystack) {
  stackVm.clear();
  stackVm = pystack.stack;
}

void PyTVM::set_libs(PyCell libs) {
  lib_set.clear();  // remove old libs
  lib_set.push_back(libs.my_cell);
}

td::Result<block::Config> decode_config(vm::Ref<vm::Cell> config_params_cell) {
  auto config_dict = std::make_unique<vm::Dictionary>(config_params_cell, 32);
  auto config_addr_cell = config_dict->lookup_ref(td::BitArray<32>::zero());
  if (config_addr_cell.is_null()) {
    return td::Status::Error("Can't find config address (param 0) is missing in config params");
  }
  auto config_addr_cs = vm::load_cell_slice(std::move(config_addr_cell));
  if (config_addr_cs.size() != 0x100) {
    return td::Status::Error(PSLICE() << "configuration parameter 0 with config address has wrong size");
  }
  ton::StdSmcAddress config_addr;
  config_addr_cs.fetch_bits_to(config_addr);
  auto global_config = block::Config(config_params_cell, std::move(config_addr),
                                     block::Config::needWorkchainInfo | block::Config::needSpecialSmc |
                                     block::Config::needCapabilities);
  TRY_STATUS_PREFIX(global_config.unpack(), "Can't unpack config params: ");
  return std::move(global_config);
}

// Vm logger
class PythonLogger : public td::LogInterface {
public:
    bool muted = false;
    vm::VmDumper *vm_dumper{0};

    void set_vm_dumper(vm::VmDumper *vm_dumper_) {
      vm_dumper = vm_dumper_;
    }

    void mute() {
      muted = true;
    }

    void append(td::CSlice slice) override {
      if (vm_dumper->enable) {
        if (slice.str().find("execute") != std::string::npos) {
          vm_dumper->dump_op(slice.str());
        }
      }

      if (!muted) {
        py::print(slice.str());
      }
    }

    void append(td::CSlice slice, int mode) override {
      append(std::move(slice));
    }
};

std::vector<vm::StackEntry> ref_tuple_to_vector(const td::Ref<vm::Tuple> &ref_tuple) {
  return *ref_tuple;
}


PyStack PyTVM::run_vm() {
  py::gil_scoped_release release;
  if (code.is_null()) {
    throw std::invalid_argument("To run VM, please pass code");
  }

  auto stack_ = td::make_ref<vm::Stack>();

  vm::VmLog vm_log;
  vm::VmDumper vm_dumper{enable_vm_dumper, &stacks, &vm_ops};

  vm_log = vm::VmLog();

  auto pyLogger = new PythonLogger();
  pyLogger->set_vm_dumper(&vm_dumper);

  if (log_level < LOG_DEBUG) {
    pyLogger->mute();
  }

  vm_log.log_interface = pyLogger;
  vm_log.log_options = td::LogOptions(VERBOSITY_NAME(DEBUG), true, false);

  std::vector<vm::StackEntry> init_c7;
  int supported_version = ton::SUPPORTED_VERSION;
  td::uint16 max_vm_data_depth = 512;

  if (!skip_c7 && c7) {
    init_c7 = ref_tuple_to_vector(c7.value().entry.as_tuple());

    if (init_c7.size() >= 10) {
      auto config = init_c7[9];

      if (config.is_cell()) {
        auto cfg = decode_config(config.as_cell());

        if (cfg.is_ok()) {
          auto config_parsed = cfg.move_as_ok();

          supported_version = config_parsed.get_global_version();
          auto r_limits = config_parsed.get_size_limits_config();
          if (r_limits.is_ok()) {
            max_vm_data_depth = r_limits.ok().max_vm_data_depth;
          }
          if (supported_version >= 6 && init_c7.size() > 17) {
            if (init_c7[3].is_int()) {
              auto now = static_cast<unsigned int>(init_c7[3].as_int()->to_long());
              init_c7[15] = config_parsed.get_unpacked_config_tuple(now);
            }

            td::optional<block::PrecompiledContractsConfig::Contract> precompiled;
            precompiled = config_parsed.get_precompiled_contracts_config().get_contract(
                    code.my_cell->get_hash().bits());
            init_c7[17] = precompiled ? td::make_refint(precompiled.value().gas_usage) : vm::StackEntry();
          }
        }
      }
    }
  }

  auto tuple_ref = td::make_cnt_ref<std::vector<vm::StackEntry>>(std::move(init_c7));

  log_debug("Use code: " + code.my_cell->get_hash().to_hex());

  log_debug("Load cp0");
  vm::init_op_cp0(allowDebug);

  int flags = 0;
  if (sameC3) {
    flags += 1;
  }

  if (log_level > LOG_DEBUG) {
    flags += 4;  // dump stack
  }

  vm::VmState vm_local{
          code.my_cell,
          td::make_ref<vm::Stack>(stackVm),
          &vm_dumper,
          gas_limits,
          flags,
          data.my_cell,
          vm_log,
          std::move(lib_set)};

  vm_local.set_c7(vm::make_tuple_ref(std::move(tuple_ref)));
  vm_local.set_global_version(supported_version);
  vm_local.set_chksig_always_succeed(ignore_chksig);
  vm_local.set_max_data_depth(max_vm_data_depth);

  vm_init_state_hash_out = vm_local.get_state_hash().to_hex();
  exit_code_out = vm_local.run();

  vm_final_state_hash_out = vm_local.get_final_state_hash(exit_code_out).to_hex();
  vm_steps_out = (int) vm_local.get_steps_count();

  auto gas = vm_local.get_gas_limits();
  gas_used_out = std::min<long long>(gas.gas_consumed(), gas.gas_limit);
  gas_credit_out = gas.gas_credit;
  success_out = (gas_credit_out == 0 && vm_local.committed());

  if (success_out) {
    new_data_out = PyCell(vm_local.get_committed_state().c4);
    actions_out = PyCell(vm_local.get_committed_state().c5);
  }

  log_debug("VM terminated with exit code " + std::to_string(exit_code_out));

  return PyStack(vm_local.get_stack());
}

std::vector<PyStackInfo> PyTVM::get_stacks() {
  std::vector<PyStackInfo> all_stacks;

  for (const auto &stack: stacks) {
    all_stacks.push_back(PyStackInfo{PyStack(std::move(stack.stack)), stack.gas_consumed, stack.gas_remaining});
  }

  return all_stacks;
}