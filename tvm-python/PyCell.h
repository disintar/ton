// Copyright 2023 Disintar LLP / andrey@head-labs.com

#include "third-party/pybind11/include/pybind11/pybind11.h"
#include "vm/vm.h"

namespace py = pybind11;


#ifndef TON_PYCELL_H
#define TON_PYCELL_H

class PyCell {
 public:
  vm::Ref<vm::Cell> my_cell;

  // constructor
  explicit PyCell(const vm::Ref<vm::Cell>& cell) {
    my_cell = cell;
  }

  explicit PyCell() = default;
  ~PyCell() = default;
  std::string get_hash() const;
  int get_depth() const;
  std::string toString() const;
  std::string dump() const;
  std::string dump_as_tlb(std::string tlb_type) const;
  std::string to_boc() const;
  py::bytes to_slice() const;
  PyCell copy() const;
  bool is_null() const;

  static void dummy_set() {
    throw std::invalid_argument("Not settable");
  }
};

PyCell parse_string_to_cell(const std::string& base64string);

#endif  //TON_PYCELL_H
