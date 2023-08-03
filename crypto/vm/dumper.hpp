#pragma once
#include "vm/stack.hpp"

namespace vm {
using td::Ref;

struct StackInfo {
  Stack stack;
  long long gas_consumed;
  long long gas_remaining;
};

class VmDumper {
 public:
  bool enable;
  std::vector<StackInfo>* stacks{};
  std::vector<std::string>* vm_ops{};
  //  std::vector<std::tuple<long long, long long>>* gas_info{};

  VmDumper(bool enable_, std::vector<StackInfo>* stacks_, std::vector<std::string>* vm_ops_) {
    enable = enable_;
    stacks = stacks_;
    vm_ops = vm_ops_;
  }

  explicit VmDumper(VmDumper* dumper_) {
    stacks = dumper_->stacks;
    vm_ops = dumper_->vm_ops;
    enable = true;
  }

  VmDumper() {
    enable = false;
  }

  void dump_stack(const Ref<vm::Stack>& stack, long long gas_consumed, long long gas_remaining) const {
    if (!enable) {
      throw std::invalid_argument("Must be enabled to dump");
    }

    vm::Stack current_stack;
    stack->for_each_scalar([&current_stack](const StackEntry& stackPointer) { current_stack.push(stackPointer); });
    StackInfo x{current_stack, gas_consumed, gas_remaining};
    stacks->push_back(std::move(x));
    //    gas_info->push_back(std::make_tuple(gas_consumed, gas_remaining));
  };

  void dump_op(std::string op) const {
    if (!enable) {
      throw std::invalid_argument("Must be enabled to dump");
    }

    vm_ops->push_back(std::move(op));
  };
};

}  // namespace vm
