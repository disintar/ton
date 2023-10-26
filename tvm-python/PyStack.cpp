// Copyright 2023 Disintar LLP / andrey@head-labs.com

#include "third-party/pybind11/include/pybind11/pybind11.h"
#include "PyStack.h"

namespace py = pybind11;

td::Ref<vm::Tuple> make_tuple_ref_from_vector(std::vector<vm::StackEntry> tmp) {
  return td::make_cnt_ref<std::vector<vm::StackEntry>>(std::move(tmp));
}

PyStackEntry make_tuple(std::vector<PyStackEntry> e) {
  std::vector<vm::StackEntry> tmp;
  for (auto x : e) {
    tmp.push_back(x.entry);
  }
  return PyStackEntry(vm::StackEntry(make_tuple_ref_from_vector(tmp)));
}

PyStackEntry deserialize_stack_entry(PyCellSlice cs) {
  vm::StackEntry x;
  x.deserialize(cs.my_cell_slice);
  return PyStackEntry(x);
}

PyStack deserialize_stack(PyCellSlice cs) {
  vm::Stack x;
  x.deserialize(cs.my_cell_slice);
  return PyStack(x);
}