//
// Created by Andrey Tvorozhkov on 5/9/23.
//

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