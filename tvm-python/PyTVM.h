// Copyright 2023 Disintar LLP / andrey@head-labs.com

#include <string>
#include <utility>
#include <optional>
#include "block/block.h"
#include "block/block-parse.h"
#include "vm/dumper.hpp"
#include "crypto/vm/cellslice.h"
#include "PyCellSlice.h"
#include "PyCell.h"
#include "PyCellBuilder.h"
#include "PyStack.h"
#include "PyDict.h"
#include "third-party/pybind11/include/pybind11/pybind11.h"

#ifndef TON_TVM_H
#define TON_TVM_H

namespace py = pybind11;
using namespace pybind11::literals;  // to bring in the `_a` literal

const int LOG_DEBUG = 2;
const int LOG_INFO = 1;

struct PyStackInfo {
  PyStack stack;
  long long gas_consumed;
  long long gas_remaining;
};

class PyTVM {
 public:
  PyCell code;
  PyCell data;

  vm::GasLimits gas_limits;
  std::vector<td::Ref<vm::Cell>> lib_set;
  vm::Stack stackVm;
  bool allowDebug;
  bool sameC3;
  int log_level;
  bool skip_c7 = false;
  bool enable_vm_dumper = true;

  std::optional<PyStackEntry> c7;

  int exit_code_out{};
  long long vm_steps_out{};
  long long gas_used_out{};
  long long gas_credit_out{};
  bool success_out{};
  std::string vm_final_state_hash_out;
  std::string vm_init_state_hash_out;
  PyCell new_data_out;
  PyCell actions_out;

  std::vector<vm::StackInfo> stacks;
  std::vector<std::string> vm_ops;
  std::vector<std::tuple<long long, long long>> gas_info;

  // constructor
  explicit PyTVM(int log_level_ = 0, std::optional<PyCell> code_ = std::optional<PyCell>(),
                 std::optional<PyCell> data_ = std::optional<PyCell>(), bool allowDebug_ = false, bool sameC3_ = true,
                 bool skip_c7_ = false, bool enable_vm_dumper_ = true) {
    allowDebug = allowDebug_;
    sameC3 = sameC3_;
    skip_c7 = skip_c7_;
    enable_vm_dumper = enable_vm_dumper_;

    this->log_level = log_level_;

    if (code_) {
      set_code(code_.value());
    }

    if (data_) {
      set_data(data_.value());
    }
  }

  void set_c7(PyStackEntry x);

  void log(const std::string& log_string, int level = LOG_INFO);

  void log_debug(const std::string& log_string) {
    log(log_string, LOG_DEBUG);
  }
  void log_info(const std::string& log_string) {
    log(log_string, LOG_INFO);
  }

  void set_gasLimit(const std::string& gas_limit_s, const std::string& gas_max_s = "");
  void set_data(PyCell data_);
  void set_code(PyCell code_);
  void set_state_init(PyCell state_init_);
  void set_stack(PyStack pystack);
  void set_libs(PyCell libs);
  PyStack run_vm();
  std::vector<PyStackInfo> get_stacks();

  std::vector<std::string> get_ops() {
    return vm_ops;
  }

  void clear_stack() {
    stackVm.clear();
  }

  PyCell get_data() {
    return data;
  }

  PyCell get_code() const {
    return code;
  }

  int get_exit_code() const {
    return exit_code_out;
  }

  long long get_vm_steps() const {
    return vm_steps_out;
  }

  long long get_gas_used() const {
    return gas_used_out;
  }

  long long get_gas_credit() const {
    return gas_credit_out;
  }

  bool get_success() const {
    return success_out;
  }

  PyCell get_new_data() const {
    return new_data_out;
  }

  PyCell get_actions() const {
    return actions_out;
  }

  std::string get_vm_final_state_hash() const {
    return vm_final_state_hash_out;
  }

  std::string get_vm_init_state_hash() const {
    return vm_init_state_hash_out;
  }

  static void dummy_set() {
    throw std::invalid_argument("Not settable");
  }
};

#endif  //TON_TVM_H
