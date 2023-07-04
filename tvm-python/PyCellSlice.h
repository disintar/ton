//
// Created by Andrey Tvorozhkov on 5/9/23.
//

#include "vm/vm.h"

#ifndef TON_PYCELLSLICE_H
#define TON_PYCELLSLICE_H

class PyCellSlice {
 public:
  vm::CellSlice my_cell_slice;

  // constructor
  explicit PyCellSlice(const vm::Ref<vm::Cell>& cell_slice) {
    my_cell_slice = vm::load_cell_slice(cell_slice);
  }

  explicit PyCellSlice() = default;

  std::string load_uint(unsigned n);
  std::string preload_uint(unsigned int n);
  std::string load_int(unsigned n);
  std::string preload_int(unsigned n) const;
  bool advance(unsigned int n);
  bool advance_ext(unsigned int n);
  bool skip_bits(unsigned int n, bool last = false);
  bool skip_refs(unsigned n, bool last = false);
  std::string toString() const;
  std::string load_addr();
  PyCellSlice fetch_ref();
  PyCellSlice prefetch_ref(int offset = 0) const;
  std::string dump() const;
  std::string load_var_integer_str(unsigned int varu, bool sgnd);
  std::string to_boc() const;
  std::string get_hash() const;
  std::string dump_as_tlb(std::string tlb_type) const;
  std::string load_string(unsigned int text_size = 0, bool convert_to_utf8 = true);
  PyCellSlice load_tlb(std::string tlb_type);
  std::string load_snake_string();
  int bselect(unsigned bits, std::string mask);
  int bselect_ext(unsigned bits, std::string mask);
  int bit_at(unsigned int n);
  bool begins_with_skip_bits(int bits, const std::string& value);
  bool begins_with_skip(const std::string& value);
  unsigned bits() const;
  unsigned refs() const;

  static void dummy_set() {
    throw std::invalid_argument("Not settable");
  }
};

#endif  //TON_PYCELLSLICE_H
