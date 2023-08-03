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

class PyContinuation {
 public:
  td::Ref<vm::Continuation> cnt;

  PyContinuation(PyCellSlice cs) {
    cnt = td::Ref<vm::Continuation>();
    cnt->deserialize(cs.my_cell_slice);
  }

  PyContinuation(td::Ref<vm::Continuation> cnt_) {
    cnt = std::move(cnt_);
  }

  std::string type() {
    return cnt->type();
  }

  PyCell serialize() {
    vm::CellBuilder cb;
    cnt->serialize(cb);
    return PyCell(cb.finalize());
  }
};

class PyStack;

class PyStackEntry {
 public:
  PyStackEntry(std::optional<PyCell> cell = std::optional<PyCell>(),
               std::optional<PyCellSlice> cell_slice = std::optional<PyCellSlice>(),
               std::optional<PyCellSlice> cell_builder = std::optional<PyCellSlice>(),
               std::optional<PyContinuation> continuation = std::optional<PyContinuation>(), std::string big_int = "") {
    if (cell) {
      entry = vm::StackEntry(cell.value().my_cell);
    } else if (cell_slice) {
      entry = vm::StackEntry(vm::Ref<vm::CellSlice>{false, cell_slice.value().my_cell_slice});
    } else if (cell_builder) {
      auto cb = td::Ref<vm::CellBuilder>{true};
      cb.write().append_cellslice(cell_builder.value().my_cell_slice);
      entry = vm::StackEntry(cb);
    } else if (!big_int.empty()) {
      td::RefInt256 x = td::string_to_int256(big_int);
      entry = vm::StackEntry(x);
    } else if (continuation) {
      entry = vm::StackEntry(continuation.value().cnt);
    } else {
      entry = vm::StackEntry();
    }
  };

  PyStackEntry(td::Ref<vm::Stack> stk) {
    entry = vm::StackEntry(stk);
  }

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

  std::vector<PyStackEntry> as_tuple() {
    auto x = entry.as_tuple();
    std::vector<PyStackEntry> tmp;
    for (const auto& e : *x) {
      tmp.push_back(PyStackEntry(e));
    }

    return tmp;
  }

  PyContinuation as_cont() {
    auto x = entry.as_cont();
    return PyContinuation(x);
  }

  PyCellSlice as_cell_slice() {
    auto x = entry.as_slice();
    return PyCellSlice(x->get_base_cell());
  }

  PyCellBuilder as_cell_builder() {
    auto x = entry.as_builder();
    bool special;
    auto cs = vm::load_cell_slice_special(x->finalize_copy(), special);
    return PyCellBuilder(cs);
  }

  std::string as_int() {
    auto x = entry.as_int();
    return x->to_dec_string();
  }

  PyCell serialize(int mode = 0) {
    vm::CellBuilder cb;
    auto x = entry.serialize(cb, mode);
    if (!x) {
      throw std::invalid_argument("Can't serialize object");
    }
    return PyCell(cb.finalize());
  }
};

PyStackEntry make_tuple(std::vector<PyStackEntry> e);

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

  void push(PyStackEntry value) {
    stack.push(value.entry);
  }

  PyStackEntry pop() {
    return PyStackEntry(stack.pop());
  }

  PyCell serialize(int mode = 0) {
    vm::CellBuilder cb;
    auto x = stack.serialize(cb, mode);
    if (!x) {
      throw std::invalid_argument("Can't serialize object");
    }
    return PyCell(cb.finalize());
  }

  bool is_empty() {
    return stack.is_empty();
  }

  int depth() {
    return stack.depth();
  }

  vm::Stack stack;
};

PyStackEntry deserialize_stack_entry(PyCellSlice cs);
PyStack deserialize_stack(PyCellSlice cs);

#endif  //TON_STACK_H
