//
// Created by Andrey Tvorozhkov on 5/9/23.
//

#include "vm/vm.h"
#include "PyCell.h"
#include "pybind11/pybind11.h"

#ifndef TON_PYCELLSLICE_H
#define TON_PYCELLSLICE_H

class PyCellSlice {
 public:
  vm::CellSlice my_cell_slice;

  // constructor
  explicit PyCellSlice(const vm::Ref<vm::Cell>& cell, bool allow_special = false) {
    if (allow_special) {
      my_cell_slice = vm::load_cell_slice_special(cell, allow_special);
    } else {
      my_cell_slice = vm::load_cell_slice(cell);
    }
  }

  explicit PyCellSlice() = default;

  std::string load_uint(unsigned n);
  bool is_special();
  int special_type();
  std::string preload_uint(unsigned int n);
  std::string load_int(unsigned n);
  std::string preload_int(unsigned n) const;
  bool advance(unsigned int n);
  bool advance_ext(unsigned int bits_refs);
  bool advance_bits_refs(unsigned int bits, unsigned int refs);
  bool advance_refs(unsigned int refs);
  bool skip_bits(unsigned int n, bool last = false);
  bool skip_refs(unsigned n, bool last = false);
  std::string toString() const;
  std::string load_addr();
  bool begins_with(const std::string& n) const;
  bool begins_with_bits(unsigned bits, const std::string& n) const;
  PyCell fetch_ref();
  PyCell prefetch_ref(int offset = 0) const;
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
  int bit_at(unsigned int n) const;
  bool begins_with_skip_bits(int bits, const std::string& value);
  bool begins_with_skip(const std::string& value);
  std::string to_bitstring() const;
  unsigned bits() const;
  unsigned refs() const;

  static void dummy_set() {
    throw std::invalid_argument("Not settable");
  }
};

PyCellSlice load_as_cell_slice(PyCell cell, bool allow_special);

#endif  //TON_PYCELLSLICE_H
