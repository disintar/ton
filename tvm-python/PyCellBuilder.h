//
// Created by Andrey Tvorozhkov on 5/9/23.
//

#include "vm/vm.h"
#include "PyCellSlice.h"
#include "PyCell.h"

#ifndef TON_PYCELLBUILDER_H
#define TON_PYCELLBUILDER_H

class PyCellBuilder {
 public:
  vm::CellBuilder my_builder;
  ~PyCellBuilder() = default;

  // constructor
  explicit PyCellBuilder() {
    my_builder = vm::CellBuilder();
  }

  PyCellBuilder* store_uint_str(const std::string& str, unsigned int bits);
  PyCellBuilder* store_int_str(const std::string& str, unsigned int bits);
  PyCellBuilder* store_bitstring(const std::string& s);
  PyCellBuilder* store_slice(const PyCellSlice& cs);
  PyCellBuilder* store_grams_str(const std::string& str);
  PyCellBuilder* store_var_integer(const std::string& str, unsigned int varu, bool sgnd);
  PyCellBuilder* store_ref(const PyCell& c);
  PyCellBuilder* store_zeroes(unsigned int bits);
  PyCellBuilder* store_ones(unsigned int bits);
  PyCellBuilder* store_builder(const PyCellBuilder& cb);
  PyCellBuilder* store_address(const std::string& addr);
  PyCell get_cell();
  std::string toString() const;
  std::string dump() const;
  std::string dump_as_tlb(std::string tlb_type) const;
  std::string to_boc() const;

  static void dummy_set() {
    throw std::invalid_argument("Not settable");
  }
};

#endif  //TON_PYCELLBUILDER_H
