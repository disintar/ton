//
// Created by Andrey Tvorozhkov on 5/9/23.
//

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
#include <queue>
#include "PyCellSlice.h"
#include "PyCell.h"
#include "PyCellBuilder.h"
#include "PyDict.h"
#include "PyStack.h"
#include "PyTVM.h"

namespace py = pybind11;

void PyTVM::set_c7(int c7_unixtime_, const std::string& c7_blocklt_, const std::string& c7_translt_,
                   const std::string& c7_randseed_, const std::string& c7_balanceRemainingGrams_,
                   const std::string& c7_myaddress_, std::optional<PyCell> c7_globalConfig_) {
  if (!skip_c7) {
    c7_unixtime = c7_unixtime_;
    c7_blocklt = td::dec_string_to_int256(c7_blocklt_);
    c7_translt = td::dec_string_to_int256(c7_translt_);
    c7_randseed = td::dec_string_to_int256(c7_randseed_);
    c7_balanceRemainingGrams = td::dec_string_to_int256(c7_balanceRemainingGrams_);
    if (!c7_myaddress_.empty()) {
      CHECK(c7_myaddress.parse_addr(c7_myaddress_));
    } else {
      c7_myaddress.parse_addr("Ef9Tj6fMJP+OqhAdhKXxq36DL+HYSzCc3+9O6UNzqsgPfYFX");
    }

    c7_globalConfig = c7_globalConfig_;
  } else {
    throw std::invalid_argument("C7 will be skipped, because skip_c7=true");
  }
}

void PyTVM::log(const std::string& log_string, int level) {
  if (this->log_level >= level && level == LOG_INFO) {
    py::print("INFO: " + log_string);
  } else if (this->log_level >= level && level == LOG_DEBUG) {
    py::print("DEBUG: " + log_string);
  }
}

void PyTVM::set_gasLimit(const std::string& gas_limit_s, const std::string& gas_max_s) {
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

void PyTVM::set_libs(PyDict dict_) {
  lib_set.clear();  // remove old libs
  lib_set.push_back(dict_.my_dict->get_root_cell());
}

PyStack PyTVM::run_vm() {
  pybind11::gil_scoped_acquire gil;

  if (code.is_null()) {
    throw std::invalid_argument("To run VM, please pass code");
  }

  auto stack_ = td::make_ref<vm::Stack>();

  vm::VmLog vm_log;
  vm::VmDumper vm_dumper{true, &stacks, &vm_ops};

  vm_log = vm::VmLog();

  //  auto pyLogger = new PythonLogger();
  //  pyLogger->set_vm_dumper(&vm_dumper);
  //
  //  if (log_level < LOG_DEBUG) {
  //    pyLogger->mute();
  //  }
  //
  //  vm_log.log_interface = pyLogger;

  auto balance = block::CurrencyCollection{c7_balanceRemainingGrams};

  td::Ref<vm::CellSlice> my_addr = block::tlb::MsgAddressInt().pack_std_address(c7_myaddress);

  td::Ref<vm::Cell> global_config;
  if (c7_globalConfig) {
    global_config = c7_globalConfig.value().my_cell;
  }

  td::Ref<vm::Tuple> init_c7;

  if (!skip_c7) {
    init_c7 =
        vm::make_tuple_ref(td::make_refint(0x076ef1ea),              // [ magic:0x076ef1ea
                           td::make_refint(0),                       //   actions:Integer
                           td::make_refint(0),                       //   msgs_sent:Integer
                           td::make_refint(c7_unixtime),             //   unixtime:Integer
                           td::make_refint(c7_blocklt->to_long()),   //   block_lt:Integer
                           td::make_refint(c7_translt->to_long()),   //   trans_lt:Integer
                           td::make_refint(c7_randseed->to_long()),  //   rand_seed:Integer
                           balance.as_vm_tuple(),                    //   balance_remaining:[Integer (Maybe Cell)]
                           std::move(my_addr),                       //  myself:MsgAddressInt
                           vm::StackEntry::maybe(global_config));  //  global_config:(Maybe Cell) ] = SmartContractInfo;
  } else {
    init_c7 = vm::make_tuple_ref();
  }

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
      code.my_cell,       td::make_ref<vm::Stack>(stackVm),      &vm_dumper, gas_limits, flags, data.my_cell, vm_log,
      std::move(lib_set), vm::make_tuple_ref(std::move(init_c7))};

  vm_init_state_hash_out = vm_local.get_state_hash().to_hex();
  exit_code_out = vm_local.run();

  vm_final_state_hash_out = vm_local.get_final_state_hash(exit_code_out).to_hex();
  vm_steps_out = (int)vm_local.get_steps_count();

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
//
//std::vector<std::vector<py::object>> PyTVM::get_stacks() {
//  std::vector<std::vector<py::object>> AllPyStack;
//
//  for (const auto& stack : stacks) {
//    std::vector<py::object> pyStack;
//    for (const auto& stackEntry : stack) {
//      pyStack.push_back(cast_stack_item_to_python_object(stackEntry));
//    }
//
//    AllPyStack.push_back(pyStack);
//  }
//
//  return AllPyStack;
//}
