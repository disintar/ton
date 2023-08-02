//
// Created by Andrey Tvorozhkov on 5/9/23.
//

#include "third-party/pybind11/include/pybind11/pybind11.h"
#include <string>
#include <utility>
#include <optional>
#include "block/block.h"
#include "block/block-parse.h"
#include "vm/dumper.hpp"
#include "crypto/vm/cellslice.h"
#include "crypto/vm/cells/CellBuilder.h"
#include "PyCellSlice.h"
#include "PyCell.h"
#include "PyCellBuilder.h"
#include "crypto/fift/Fift.h"
#include "crypto/fift/IntCtx.h"
#include "crypto/fift/words.h"
#include "td/utils/filesystem.h"
#include "crypto/vm/stack.hpp"

#ifndef TON_STACK_H
#define TON_STACK_H

class PyStackEntry {
 public:
  PyStackEntry(std::optional<PyCell> cell = std::optional<PyCell>(),
               std::optional<PyCellSlice> cell_slice = std::optional<PyCellSlice>(), std::string big_int = "") {
    if (cell) {
      entry = vm::StackEntry(cell.value().my_cell);
    } else if (cell_slice) {
      entry = vm::StackEntry(vm::Ref<vm::CellSlice>{false, cell_slice.value().my_cell_slice});
    } else if (!big_int.empty()) {
      td::RefInt256 x = td::string_to_int256(big_int);
      entry = vm::StackEntry(x);
    } else {
      entry = vm::StackEntry();
    }
  };

  PyStackEntry(vm::StackEntry stk) {
    entry = std::move(stk);
  }

  vm::StackEntry entry;

  int type() {
    return entry.type();
  }

  PyCell as_cell() {
    return PyCell(entry.as_cell());
  }

  PyCellSlice as_cell_slice() {
    auto x = entry.as_slice();
    return PyCellSlice(x->get_base_cell());
  }

  std::string as_int() {
    auto x = entry.as_int();
    return x->to_dec_string();
  }
};

class PyStack {
 public:
  PyStack() {
    stack = vm::Stack();
  }

  PyStack(vm::Stack x) {
    stack = std::move(x);
  }

  PyStackEntry at(int idx) {
    return PyStackEntry(stack.at(idx));
  }

  PyStackEntry pop() {
    return PyStackEntry(stack.pop());
  }

  bool is_empty() {
    return stack.is_empty();
  }

  int depth() {
    return stack.depth();
  }

  vm::Stack stack;
};

#endif  //TON_STACK_H
