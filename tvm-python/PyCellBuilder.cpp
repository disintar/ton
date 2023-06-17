//
// Created by Andrey Tvorozhkov on 5/9/23.
//

#include "pybind11/pybind11.h"
#include <string>
#include "vm/vmstate.h"
#include "td/utils/base64.h"
#include <utility>
#include "vm/boc.h"
#include "vm/cellslice.h"
#include "block/block.h"
#include "block-auto.h"
#include "block/block-parse.h"
#include "td/utils/PathView.h"
#include "PyCellBuilder.h"
#include "PyCellSlice.h"
#include "PyCell.h"

PyCellBuilder* PyCellBuilder::store_uint_str(const std::string& str, unsigned int bits) {
  td::BigInt256 x;
  x.enforce(x.parse_dec(str));

  my_builder.store_int256(x, bits, false);

  return this;
}

PyCellBuilder* PyCellBuilder::store_int_str(const std::string& str, unsigned int bits) {
  td::BigInt256 x;
  x.enforce(x.parse_dec(str));

  my_builder.store_int256(x, bits);

  return this;
}

PyCellBuilder* PyCellBuilder::store_bitstring(const std::string& s) {
  unsigned char buff[128];
  const auto tmp = td::Slice{s};
  int bits = (int)td::bitstring::parse_bitstring_binary_literal(buff, sizeof(buff), tmp.begin(), tmp.end());
  auto cs = td::Ref<vm::CellSlice>{true, vm::CellBuilder().store_bits(td::ConstBitPtr{buff}, bits).finalize()};
  my_builder = my_builder.append_cellslice(cs);

  return this;
}

PyCellBuilder* PyCellBuilder::store_slice(const PyCellSlice& cs) {
  my_builder.append_cellslice(cs.my_cell_slice);
  return this;
}

PyCellBuilder* PyCellBuilder::store_grams_str(const std::string& str) {
  td::BigInt256 x;
  x.enforce(x.parse_dec(str));

  int k = x.bit_size(false);
  const auto success = k <= 15 * 8 && my_builder.store_long_bool((k + 7) >> 3, 4) &&
                       my_builder.store_int256_bool(x, (k + 7) & -8, false);

  if (!success) {
    throw vm::CellBuilder::CellWriteError();
  }

  return this;
}

PyCellBuilder* PyCellBuilder::store_var_integer(const std::string& str, unsigned int varu, bool sgnd) {
  td::BigInt256 x;
  x.enforce(x.parse_dec(std::move(str)));

  const unsigned int len_bits = 32 - td::count_leading_zeroes32(varu - 1);
  unsigned len = (((unsigned)x.bit_size(sgnd) + 7) >> 3);

  if (len >= (1u << len_bits)) {
    throw vm::VmError{vm::Excno::range_chk};
  }

  if (!(my_builder.store_long_bool(len, len_bits) && my_builder.store_int256_bool(x, len * 8, sgnd))) {
    throw vm::VmError{vm::Excno::cell_ov, "cannot serialize a variable-length integer"};
  }

  return this;
}

PyCellBuilder* PyCellBuilder::store_ref(const PyCell& c) {
  my_builder.store_ref(c.my_cell);
  return this;
}

PyCellBuilder* PyCellBuilder::store_zeroes(unsigned int bits) {
  my_builder.store_zeroes(bits);
  return this;
}

PyCellBuilder* PyCellBuilder::store_ones(unsigned int bits) {
  my_builder.store_ones(bits);
  return this;
}

PyCellBuilder* PyCellBuilder::store_builder(const PyCellBuilder& cb) {
  if (!my_builder.can_extend_by(cb.my_builder.size(), cb.my_builder.size_refs())) {
    throw vm::CellBuilder::CellWriteError();
  }

  my_builder.append_builder(std::move(cb.my_builder));
  return this;
}

PyCellBuilder* PyCellBuilder::store_address(const std::string& addr) {
  const block::StdAddress res = block::StdAddress::parse(addr).move_as_ok();

  block::tlb::t_MsgAddressInt.store_std_address(my_builder, res);
  return this;
}

PyCell PyCellBuilder::get_cell() {
  return PyCell(my_builder.finalize());
}

std::string PyCellBuilder::toString() const {
  std::stringstream os;
  vm::CellSlice tmp(my_builder.finalize_copy());
  tmp.dump(os);
  auto t = os.str();
  t.pop_back();

  return "<CellBuilder " + t + ">";
}

std::string PyCellBuilder::dump() const {
  std::stringstream os;
  vm::CellSlice tmp(my_builder.finalize_copy());
  tmp.print_rec(os);

  return os.str();
}

std::string PyCellBuilder::get_hash() const {
  const auto c = my_builder.finalize_copy();

  return c->get_hash().to_hex();
}

std::string PyCellBuilder::dump_as_tlb(std::string tlb_type) const {
  tlb::TypenameLookup tlb_dict0;
  tlb_dict0.register_types(block::gen::register_simple_types);

  auto _template = tlb_dict0.lookup(std::move(tlb_type));

  if (!_template) {
    throw std::invalid_argument("Parse tlb error: not valid tlb type");
  }

  std::stringstream ss;
  _template->print_ref(9 << 20, ss, my_builder.finalize_copy());

  auto output = ss.str();
  return output;
}

std::string PyCellBuilder::to_boc() const {
  return td::base64_encode(std_boc_serialize(my_builder.finalize_copy(), 31).move_as_ok());
}
