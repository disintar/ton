#pragma once
#include "vm/stack.hpp"

namespace vm {
using td::Ref;

class VmDumper {
 public:
  bool enable;
  std::vector<std::vector<StackEntry>>* stacks{};
  std::vector<std::string>* vm_ops{};
  std::vector<long long>* gase_consumed{};

  VmDumper(bool enable_, std::vector<std::vector<StackEntry>>* stacks_, std::vector<std::string>* vm_ops_,
           std::vector<long long>* gase_consumed_) {
    enable = enable_;
    stacks = stacks_;
    gase_consumed = gase_consumed_;
    vm_ops = vm_ops_;
  }

  explicit VmDumper(VmDumper* dumper_) {
    stacks = dumper_->stacks;
    vm_ops = dumper_->vm_ops;
    gase_consumed = dumper_->gase_consumed;
    enable = true;
  }

  VmDumper() {
    enable = false;
  }

  void dump_gas_consumed(long long gas) const {
    if (!enable) {
      throw std::invalid_argument("Must be enabled to dump");
    }

    gase_consumed->push_back(gas);
  }

  void dump_stack(const Ref<vm::Stack>& stack) const {
    if (!enable) {
      throw std::invalid_argument("Must be enabled to dump");
    }

    std::vector<StackEntry> current_stack;

    stack->for_each_scalar([&current_stack](const StackEntry& stackPointer) { current_stack.push_back(stackPointer); });

    stacks->push_back(std::move(current_stack));
  };

  void dump_op(std::string op) const {
    if (!enable) {
      throw std::invalid_argument("Must be enabled to dump");
    }

    vm_ops->push_back(std::move(op));
  };
};

}  // namespace vm
