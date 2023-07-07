//
// Created by Andrey Tvorozhkov on 5/9/23.
//

#include "vm/vm.h"

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
  std::string toString() const;
  std::string dump() const;
  std::string dump_as_tlb(std::string tlb_type) const;
  std::string to_boc() const;
  bool is_null() const;

  static void dummy_set() {
    throw std::invalid_argument("Not settable");
  }
};

PyCell parseStringToCell(const std::string& base64string);

#endif  //TON_PYCELL_H
