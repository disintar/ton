//
// Created by Andrey Tvorozhkov on 5/9/23.
//

#include "third-party/pybind11/include/pybind11/pybind11.h"
#include <string>
#include "vm/vmstate.h"
#include "td/utils/base64.h"
#include <utility>
#include "vm/boc.h"
#include "vm/cellslice.h"
#include "block/block.h"
#include "block/block-parse.h"
#include "vm/dumper.hpp"
#include "crypto/vm/cellslice.h"
#include <queue>
#include "block/block-auto.h"
#include "PyCellSlice.h"

std::string PyCellSlice::load_uint(unsigned n) {
  const auto tmp = my_cell_slice.fetch_int256(n, false);
  return tmp->to_dec_string();
}

std::string PyCellSlice::preload_uint(unsigned n) {
  const auto tmp = my_cell_slice.fetch_int256(n, false);
  return tmp->to_dec_string();
}

std::string PyCellSlice::load_int(unsigned n) {
  const auto tmp = my_cell_slice.fetch_int256(n, true);
  return tmp->to_dec_string();
}

std::string PyCellSlice::preload_int(unsigned n) const {
  const auto tmp = my_cell_slice.prefetch_int256(n, true);
  return tmp->to_dec_string();
}

std::string PyCellSlice::toString() const {
  std::stringstream os;
  my_cell_slice.dump(os);

  auto t = os.str();
  t.pop_back();

  return "<CellSlice " + t + ">";
}

std::string PyCellSlice::load_addr() {
  ton::StdSmcAddress addr;
  ton::WorkchainId workchain;
  if (!block::tlb::t_MsgAddressInt.extract_std_address(my_cell_slice, workchain, addr)) {
    throw std::invalid_argument("Parse address error: not valid address");
  }
  auto friendlyAddr = block::StdAddress(workchain, addr);

  return friendlyAddr.rserialize(true);
}

PyCellSlice PyCellSlice::fetch_ref() {
  const auto r = my_cell_slice.fetch_ref();
  if (r.is_null()) {
    throw vm::CellBuilder::CellWriteError();
  }
  return PyCellSlice(r);
}

PyCellSlice PyCellSlice::prefetch_ref(int offset) const {
  const auto r = my_cell_slice.prefetch_ref(offset);
  if (r.is_null()) {
    throw vm::CellBuilder::CellWriteError();
  }

  return PyCellSlice(r);
}

std::string PyCellSlice::dump() const {
  std::stringstream os;
  my_cell_slice.print_rec(os);

  return os.str();
}

std::string PyCellSlice::load_var_integer_str(unsigned int varu, bool sgnd) {
  td::RefInt256 x;

  const unsigned int len_bits = 32 - td::count_leading_zeroes32(varu - 1);
  int len;
  if (!(my_cell_slice.fetch_uint_to(len_bits, len) && my_cell_slice.fetch_int256_to(len * 8, x, sgnd))) {
    throw vm::VmError{vm::Excno::cell_und, "cannot deserialize a variable-length integer"};
  }

  return x->to_dec_string();
}

std::string PyCellSlice::to_boc() const {
  vm::CellBuilder cb;
  cb.append_cellslice(my_cell_slice.clone());

  return td::base64_encode(std_boc_serialize(cb.finalize(), 31).move_as_ok());
}

std::string PyCellSlice::dump_as_tlb(std::string tlb_type) const {
  tlb::TypenameLookup tlb_dict0;
  tlb_dict0.register_types(block::gen::register_simple_types);

  auto _template = tlb_dict0.lookup(std::move(tlb_type));

  if (!_template) {
    throw std::invalid_argument("Parse tlb error: not valid tlb type");
  }

  vm::CellBuilder cb;
  cb.append_cellslice(my_cell_slice.clone());

  std::stringstream ss;
  _template->print_ref(9 << 20, ss, cb.finalize_copy());

  auto output = ss.str();
  return output;
}

unsigned PyCellSlice::bits() const {
  return my_cell_slice.size();
}

unsigned PyCellSlice::refs() const {
  return my_cell_slice.size_refs();
}