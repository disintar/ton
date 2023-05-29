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
#include <string>
#include <cassert>
#include <codecvt>
#include <iostream>
#include <locale>
#include <sstream>

std::string PyCellSlice::load_uint(unsigned n) {
  if (!my_cell_slice.have(n)) {
    throw std::invalid_argument("Not enough bits in cell slice");
  }

  const auto tmp = my_cell_slice.fetch_int256(n, false);
  return tmp->to_dec_string();
}

std::string PyCellSlice::preload_uint(unsigned n) {
  if (!my_cell_slice.have(n)) {
    throw std::invalid_argument("Not enough bits in cell slice");
  }

  const auto tmp = my_cell_slice.fetch_int256(n, false);
  return tmp->to_dec_string();
}

std::string PyCellSlice::load_int(unsigned n) {
  if (!my_cell_slice.have(n)) {
    throw std::invalid_argument("Not enough bits in cell slice");
  }

  const auto tmp = my_cell_slice.fetch_int256(n, true);
  return tmp->to_dec_string();
}

std::string PyCellSlice::preload_int(unsigned n) const {
  if (!my_cell_slice.have(n)) {
    throw std::invalid_argument("Not enough bits in cell slice");
  }

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

std::string map_to_utf8(const long long val) {
  std::wstring_convert<std::codecvt_utf8<char32_t>, char32_t> converter;
  return converter.to_bytes(static_cast<char32_t>(val));
}

std::string fetch_string(vm::CellSlice &cs, unsigned int text_size, bool convert_to_utf8 = true) {
  if (convert_to_utf8) {
    std::string text;

    while (text_size > 0) {
      text += map_to_utf8(cs.fetch_long(8));
      text_size -= 1;
    }

    return text;
  } else {
    unsigned char b[text_size];
    cs.fetch_bytes(b, text_size);
    std::string tmp(b, b + sizeof b / sizeof b[0]);
    return tmp;
  }
}

std::string PyCellSlice::load_string(unsigned int text_size, bool convert_to_utf8) {
  if (text_size == 0) {
    text_size = my_cell_slice.size() / 8;
  }

  if (!my_cell_slice.have(text_size * 8)) {
    throw std::invalid_argument("Not enough bits in cell slice");
  }

  return fetch_string(my_cell_slice, text_size, convert_to_utf8);
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
  if (!my_cell_slice.have_refs(1)) {
    throw std::invalid_argument("Not enough refs in cell slice");
  }

  const auto r = my_cell_slice.fetch_ref();
  if (r.is_null()) {
    throw vm::CellBuilder::CellWriteError();
  }
  return PyCellSlice(r);
}

PyCellSlice PyCellSlice::prefetch_ref(int offset) const {
  if (!my_cell_slice.have_refs(offset + 1)) {
    throw std::invalid_argument("Not enough refs in cell slice");
  }

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

  if (!my_cell_slice.have(len_bits)) {
    throw std::invalid_argument("Not enough bits in cell slice");
  }

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

std::string PyCellSlice::get_hash() const {
  vm::CellBuilder cb;
  cb.append_cellslice(my_cell_slice.clone());
  const auto c = cb.finalize();

  return c->get_hash().to_hex();
}

bool PyCellSlice::skip_bits(unsigned int bits, bool last) {
  if (!my_cell_slice.have(bits)) {
    throw std::invalid_argument("Not enough bits in cell slice");
  }

  bool cs;

  if (!last) {
    cs = my_cell_slice.skip_first(bits);
  } else {
    cs = my_cell_slice.skip_last(bits);
  }

  return cs;
}

bool PyCellSlice::skip_refs(unsigned n, bool last) {
  if (!my_cell_slice.have_refs(n)) {
    throw std::invalid_argument("Not enough refs in cell slice");
  }

  bool cs;

  if (!last) {
    cs = my_cell_slice.skip_first(0, n);
  } else {
    cs = my_cell_slice.skip_first(0, n);
  }

  return cs;
}

PyCellSlice PyCellSlice::load_tlb(std::string tlb_type) {
  tlb::TypenameLookup tlb_dict0;
  tlb_dict0.register_types(block::gen::register_simple_types);

  auto _template = tlb_dict0.lookup(std::move(tlb_type));

  if (!_template) {
    throw std::invalid_argument("Parse tlb error: not valid tlb type");
  }

  vm::CellBuilder cb;
  auto fetched_tlb = _template->fetch(my_cell_slice);
  cb.append_cellslice(fetched_tlb);

  return PyCellSlice(cb.finalize());
}

int PyCellSlice::bselect(unsigned bits, std::string mask) {
  return my_cell_slice.bselect(bits, std::strtoull(mask.c_str(), nullptr, 10));
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

std::string fetch_string(vm::CellSlice &cs, bool convert_to_utf8 = true) {
  if (convert_to_utf8) {
    auto text_size = cs.size() / 8;

    std::string text;

    while (text_size > 0) {
      text += map_to_utf8(cs.fetch_long(8));
      text_size -= 1;
    }

    return text;
  } else {
    const unsigned int text_size = cs.size() / 8;

    unsigned char b[text_size];
    cs.fetch_bytes(b, text_size);
    std::string tmp(b, b + sizeof b / sizeof b[0]);
    return tmp;
  }
}

std::string parse_snake_data_string(vm::CellSlice &cs, bool convert_to_utf8 = true) {
  bool has_next_ref = cs.have_refs();
  std::string text = fetch_string(cs, convert_to_utf8);
  vm::CellSlice rcf = cs;

  while (has_next_ref) {
    rcf = load_cell_slice(rcf.prefetch_ref());
    auto x = fetch_string(rcf, convert_to_utf8);

    text += x;

    has_next_ref = rcf.have_refs();
  }

  return text;
}

std::string PyCellSlice::load_snake_string() {
  return parse_snake_data_string(my_cell_slice);
}

unsigned PyCellSlice::bits() const {
  return my_cell_slice.size();
}

unsigned PyCellSlice::refs() const {
  return my_cell_slice.size_refs();
}