// Copyright 2023 Disintar LLP / andrey@head-labs.com

#include "third-party/pybind11/include/pybind11/pybind11.h"
#include <string>
#include "vm/vmstate.h"
#include "td/utils/base64.h"
#include "blockchain-indexer/json-utils.hpp"
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
#include "PyCell.h"
#include <string>
#include <cassert>
#include <codecvt>
#include <iostream>
#include <locale>
#include <sstream>
#include <td/utils/utf8.h>

std::string PyCellSlice::load_uint(unsigned n) {
  if (!my_cell_slice.have(n)) {
    throw std::invalid_argument("Not enough bits in cell slice");
  }

  const auto tmp = my_cell_slice.fetch_int256(n, false);
  return tmp->to_dec_string();
}

bool PyCellSlice::is_special() {
  return my_cell_slice.is_special();
}

bool PyCellSlice::empty_ext() {
  return my_cell_slice.empty_ext();
}

int PyCellSlice::special_type() {
  return static_cast<int>(my_cell_slice.special_type());
}

std::string PyCellSlice::preload_uint(unsigned n) {
  if (!my_cell_slice.have(n)) {
    throw std::invalid_argument("Not enough bits in cell slice");
  }

  const auto tmp = my_cell_slice.prefetch_int256(n, false);
  return tmp->to_dec_string();
}

int PyCellSlice::bit_at(unsigned int n) const {
  return my_cell_slice.bit_at(n);
}

bool PyCellSlice::begins_with_bits(unsigned bits, const std::string &n) const {
  return my_cell_slice.begins_with(bits, std::stoull(n));
}

bool PyCellSlice::begins_with(const std::string &n) const {
  return my_cell_slice.begins_with(std::stoull(n));
}

bool PyCellSlice::begins_with_skip_bits(int bits, const std::string &value) {
  auto n = std::stoull(value);
  return my_cell_slice.begins_with_skip(bits, n);
}

PyCellSlice PyCellSlice::load_subslice(unsigned int bits, unsigned int refs) {
  if (!my_cell_slice.have(bits, refs)) {
    throw std::runtime_error("Not enough bits or refs");
  } else {
    vm::CellSlice x{my_cell_slice, bits, refs};
    advance(bits);
    advance_refs(refs);
    vm::CellBuilder cb;
    cb.append_cellslice(x);

    return PyCellSlice(cb.finalize(), true);
  }
}

bool PyCellSlice::cut_tail(const PyCellSlice &cs) {
  return my_cell_slice.cut_tail(cs.my_cell_slice);
}

PyCellSlice PyCellSlice::copy() const {
  vm::CellSlice cs{my_cell_slice};
  auto tmp = PyCellSlice(std::move(cs));
  return tmp;
}

PyCellSlice PyCellSlice::load_subslice_ext(unsigned int size) {
  return load_subslice(size & 0xffff, size >> 16);
}

PyCellSlice PyCellSlice::preload_subslice_ext(unsigned int size) {
  return preload_subslice(size & 0xffff, size >> 16);
}

bool PyCellSlice::begins_with_skip(const std::string &value) {
  auto n = std::stoull(value);
  return my_cell_slice.begins_with_skip(n);
}

std::string PyCellSlice::load_int(unsigned n) {
  if (!my_cell_slice.have(n)) {
    throw std::invalid_argument("Not enough bits in cell slice");
  }

  const auto tmp = my_cell_slice.fetch_int256(n, true);
  return tmp->to_dec_string();
}

PyCellSlice PyCellSlice::preload_subslice(unsigned int bits, unsigned int refs) {
  if (!my_cell_slice.have(bits, refs)) {
    throw std::runtime_error("Not enough bits or refs");
  } else {
    vm::CellSlice x{my_cell_slice, bits, refs};
    vm::CellBuilder cb;
    cb.append_cellslice(x);

    return PyCellSlice(cb.finalize(), false);
  }
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

std::string fetch_string_limited(vm::CellSlice &cs, unsigned int text_size, bool convert_to_utf8) {
  if (convert_to_utf8) {
    std::string text;
    while (text_size > 0) {
      // Fetch the first byte to determine the length of the character
      auto first_byte = cs.fetch_ulong(8);

      // Determine the number of bytes for the UTF-8 character
      size_t char_size = 0;
      if ((first_byte & 0x80) == 0) {
        char_size = 1;
      } else if ((first_byte & 0xE0) == 0xC0) {
        char_size = 2;
      } else if ((first_byte & 0xF0) == 0xE0) {
        char_size = 3;
      } else if ((first_byte & 0xF8) == 0xF0) {
        char_size = 4;
      } else {
        // Handle invalid UTF-8 sequence or adjust as needed
        char_size = 1;
      }

      std::ostringstream utf8Stream;
      utf8Stream << static_cast<char>(first_byte);
      for (size_t i = 1; i < char_size; ++i) {
        auto next_byte = cs.fetch_ulong(8);
        utf8Stream << static_cast<char>(next_byte);
      }
      text += utf8Stream.str();

      // Decrement the text_size based on the consumed bytes
      text_size -= char_size;
    }

    return text;
  } else {
    // Your existing code for non-UTF8 conversion
    td::BufferSlice s(text_size);
    cs.fetch_bytes((td::uint8 *)s.data(), text_size);
    return s.as_slice().str();
  }
}

std::string PyCellSlice::load_string(unsigned int text_size, bool convert_to_utf8) {
  if (text_size == 0) {
    text_size = my_cell_slice.size() / 8;
  }

  if (!my_cell_slice.have(text_size * 8)) {
    throw std::invalid_argument("Not enough bits in cell slice");
  }

  return fetch_string_limited(my_cell_slice, text_size, convert_to_utf8);
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

PyCell PyCellSlice::fetch_ref() {
  if (!my_cell_slice.have_refs(1)) {
    throw std::invalid_argument("Not enough refs in cell slice");
  }

  const auto r = my_cell_slice.fetch_ref();
  if (r.is_null()) {
    throw vm::CellBuilder::CellWriteError();
  }
  return PyCell(r);
}

PyCell PyCellSlice::prefetch_ref(int offset) const {
  if (!my_cell_slice.have_refs(offset + 1)) {
    throw std::invalid_argument("Not enough refs in cell slice");
  }

  const auto r = my_cell_slice.prefetch_ref(offset);
  if (r.is_null()) {
    throw vm::CellBuilder::CellWriteError();
  }

  return PyCell(r);
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

unsigned PyCellSlice::size_ext() const {
  return my_cell_slice.size_ext();
}

bool PyCellSlice::advance(unsigned bits) {
  return my_cell_slice.advance(bits);
}

bool PyCellSlice::advance_ext(unsigned bits_refs) {
  return my_cell_slice.advance_ext(bits_refs & 0xffff, bits_refs >> 16);
}

bool PyCellSlice::advance_refs(unsigned refs) {
  return my_cell_slice.advance_refs(refs);
}

bool PyCellSlice::advance_bits_refs(unsigned bits, unsigned refs) {
  return my_cell_slice.advance_ext(bits, refs);
}

bool PyCellSlice::skip_refs(unsigned n, bool last) {
  if (!my_cell_slice.have_refs(n)) {
    throw std::invalid_argument("Not enough refs in cell slice");
  }

  bool cs;

  if (!last) {
    cs = my_cell_slice.skip_first(0, n);
  } else {
    cs = my_cell_slice.skip_last(0, n);
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
  vm::Ref<vm::Cell> x = cb.finalize();
  return PyCellSlice(std::move(x));
}

int PyCellSlice::bselect(unsigned bits, std::string mask) {
  return my_cell_slice.bselect(bits, std::strtoull(mask.c_str(), nullptr, 10));
}

int PyCellSlice::bselect_ext(unsigned bits, std::string mask) {
  return my_cell_slice.bselect_ext(bits, std::strtoull(mask.c_str(), nullptr, 10));
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

  std::ostringstream ss;
  _template->print_ref(9 << 20, ss, cb.finalize_copy());

  auto output = ss.str();
  return output;
}

std::string PyCellSlice::load_snake_string() {
  return parse_snake_data_string(my_cell_slice);
}

std::string PyCellSlice::to_bitstring() const {
  return my_cell_slice.data_bits().to_binary(my_cell_slice.size());
}

int PyCellSlice::fetch_uint_less(unsigned upper_bound) {
  int value;
  my_cell_slice.fetch_uint_less(upper_bound, value);
  return value;
}

int PyCellSlice::fetch_uint_leq(unsigned upper_bound) {
  int value;
  my_cell_slice.fetch_uint_leq(upper_bound, value);
  return value;
}

unsigned PyCellSlice::bits() const {
  return my_cell_slice.size();
}

unsigned PyCellSlice::refs() const {
  return my_cell_slice.size_refs();
}

PyCellSlice load_as_cell_slice(PyCell cell, bool allow_special) {
  return PyCellSlice(cell.my_cell, allow_special);
}
