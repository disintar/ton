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
#include "crypto/vm/cellslice.h"
#include <queue>
#include "PyCellSlice.h"
#include "PyCellBuilder.h"
#include "PyDict.h"

auto get_mode(const std::string& s) {
  if (s == "add") {
    return vm::Dictionary::SetMode::Add;
  } else if (s == "replace") {
    return vm::Dictionary::SetMode::Replace;
  } else {
    return vm::Dictionary::SetMode::Set;
  }
}

PyCell PyDict::get_pycell() const {
  const auto d = *my_dict;
  td::Ref<vm::Cell> root = d.get_root_cell();
  return PyCell(root);
}

//  bool set(td::ConstBitPtr key, int key_len, Ref<CellSlice> value, SetMode mode = SetMode::Set);
PyDict* PyDict::set(const std::string& key, PyCellSlice& value, const std::string& mode, int key_len_, int sgnd_) {
  if (sgnd_ == -1) {
    sgnd_ = sgnd;
  }

  if (key_len_ == 0) {
    key_len_ = key_len;
  }

  td::BigInt256 x;
  x.enforce(x.parse_dec(key));
  td::BufferSlice s(key_len_);

  auto* tmp = new unsigned char[key_len_];
  x.export_bits(tmp, key_len_, sgnd_);

  const auto mdict = my_dict.get();
  mdict->set(tmp, key_len_, td::make_ref<vm::CellSlice>(value.my_cell_slice.clone()), get_mode(mode));
  return this;
}

//  bool Dictionary::set_ref(td::ConstBitPtr key, int key_len, Ref<Cell> val_ref, SetMode mode)
PyDict* PyDict::set_ref(const std::string& key, PyCell& value, const std::string& mode, int key_len_, int sgnd_) {
  if (sgnd_ == -1) {
    sgnd_ = sgnd;
  }

  if (key_len_ == 0) {
    key_len_ = key_len;
  }

  td::BigInt256 x;
  x.enforce(x.parse_dec(key));
  auto* tmp = new unsigned char[key_len_];
  x.export_bits(tmp, key_len_, sgnd_);

  const auto mdict = my_dict.get();

  mdict->set_ref(tmp, key_len_, value.my_cell, get_mode(mode));
  return this;
}

// bool Dictionary::set_builder(td::ConstBitPtr key, int key_len, Ref<CellBuilder> val_b, SetMode mode) {
PyDict* PyDict::set_builder(const std::string& key, PyCellBuilder& value, const std::string& mode, int key_len_,
                            int sgnd_) {
  if (sgnd_ == -1) {
    sgnd_ = sgnd;
  }

  if (key_len_ == 0) {
    key_len_ = key_len;
  }

  td::BigInt256 x;
  x.enforce(x.parse_dec(key));
  auto* tmp = new unsigned char[key_len_];
  x.export_bits(tmp, key_len_, sgnd_);

  const auto mdict = my_dict.get();

  mdict->set_builder(tmp, key_len_, value.my_builder, get_mode(mode));
  return this;
}

PyCellSlice PyDict::lookup(const std::string& key, int key_len_, int sgnd_) const {
  if (sgnd_ == -1) {
    sgnd_ = sgnd;
  }

  if (key_len_ == 0) {
    key_len_ = key_len;
  }

  td::BigInt256 x;
  x.enforce(x.parse_dec(key));
  auto* tmp = new unsigned char[key_len_];
  x.export_bits(tmp, key_len_, sgnd_);
  const auto mdict = my_dict.get();

  auto cs = mdict->lookup(tmp, key_len_);
  vm::CellBuilder cb;
  cb.append_cellslice(cs);

  return PyCellSlice(cb.finalize());
}

PyCellSlice PyDict::lookup_delete(const std::string& key, int key_len_, int sgnd_) const {
  if (sgnd_ == -1) {
    sgnd_ = sgnd;
  }

  if (key_len_ == 0) {
    key_len_ = key_len;
  }

  td::BigInt256 x;
  x.enforce(x.parse_dec(key));
  auto* tmp = new unsigned char[key_len_];
  x.export_bits(tmp, key_len_, sgnd_);
  const auto mdict = my_dict.get();

  auto cs = mdict->lookup_delete(tmp, key_len_);
  vm::CellBuilder cb;
  cb.append_cellslice(cs);

  return PyCellSlice(cb.finalize());
}

std::tuple<std::string, PyCellSlice> PyDict::get_minmax_key(bool fetch_max, bool inver_first, int key_len_,
                                                            int sgnd_) const {
  if (sgnd_ == -1) {
    sgnd_ = sgnd;
  }

  if (key_len_ == 0) {
    key_len_ = key_len;
  }
  auto* tmp = new unsigned char[key_len_];
  const auto mdict = my_dict.get();
  const auto cs = mdict->get_minmax_key(tmp, key_len_, fetch_max, inver_first);

  vm::CellBuilder cb;
  cb.append_cellslice(cs);

  td::BigInt256 x;
  x.import_bits(tmp, key_len_, sgnd_);

  return std::make_tuple(x.to_dec_string(), PyCellSlice(cb.finalize()));
}

std::tuple<std::string, PyCell> PyDict::get_minmax_key_ref(bool fetch_max, bool inver_first, int 1,
                                                           int sgnd_) const {
  if (sgnd_ == -1) {
    sgnd_ = sgnd;
  }

  if (key_len_ == 0) {
    key_len_ = key_len;
  }
  auto* tmp = new unsigned char[key_len_];
  const auto mdict = my_dict.get();
  const auto cell = mdict->get_minmax_key_ref(tmp, key_len_, fetch_max, inver_first);

  if (cell.is_null()) {
    throw vm::CellBuilder::CellCreateError();
  }

  td::BigInt256 x;
  x.import_bits(tmp, key_len_, sgnd_);

  return std::make_tuple(x.to_dec_string(), PyCell(cell));
}

std::tuple<std::string, PyCellSlice> PyDict::lookup_nearest_key(const std::string& key, bool fetch_next, bool allow_eq,
                                                                bool inver_first, int key_len_, int sgnd_) const {
  if (sgnd_ == -1) {
    sgnd_ = sgnd;
  }

  if (key_len_ == 0) {
    key_len_ = key_len;
  }

  td::BigInt256 x;
  x.enforce(x.parse_dec(key));

  auto* tmp = new unsigned char[key_len_];
  x.export_bits(tmp, key_len_, sgnd_);

  const auto mdict = my_dict.get();
  const auto cs = mdict->lookup_nearest_key(tmp, key_len_, fetch_next, allow_eq, inver_first);

  td::BigInt256 x2;
  x2.import_bits(tmp, key_len_, sgnd_);

  vm::CellBuilder cb;
  cb.append_cellslice(cs);

  return std::make_tuple(x2.to_dec_string(), PyCellSlice(cb.finalize()));
}

bool PyDict::is_empty() {
  return my_dict->is_empty();
}

PyCell PyDict::lookup_ref(const std::string& key, int key_len_, int sgnd_) const {
  if (sgnd_ == -1) {
    sgnd_ = sgnd;
  }

  if (key_len_ == 0) {
    key_len_ = key_len;
  }

  td::BigInt256 x;
  x.enforce(x.parse_dec(key));
  auto* tmp = new unsigned char[key_len_];
  x.export_bits(tmp, key_len_, sgnd_);
  const auto mdict = my_dict.get();

  auto cr = mdict->lookup_ref(tmp, key_len_);

  if (cr.is_null()) {
    throw vm::CellBuilder::CellCreateError();
  }

  return PyCell(cr);
}

PyCell PyDict::lookup_delete_ref(const std::string& key, int key_len_, int sgnd_) const {
  if (sgnd_ == -1) {
    sgnd_ = sgnd;
  }

  if (key_len_ == 0) {
    key_len_ = key_len;
  }

  td::BigInt256 x;
  x.enforce(x.parse_dec(key));
  auto* tmp = new unsigned char[key_len_];
  x.export_bits(tmp, key_len_, sgnd_);
  const auto mdict = my_dict.get();

  auto cr = mdict->lookup_delete_ref(tmp, key_len_);

  if (cr.is_null()) {
    throw vm::CellBuilder::CellCreateError();
  }

  return PyCell(cr);
}

std::string PyDict::to_boc() const {
  const auto d = *my_dict;
  td::Ref<vm::Cell> root = d.get_root_cell();

  return td::base64_encode(std_boc_serialize(root, 31).move_as_ok());
}

std::string PyDict::toString() const {
  std::stringstream os;
  const auto d = *my_dict;
  td::Ref<vm::Cell> root = d.get_root_cell();
  if (root.is_null()) {
    return "<VmDict null>";
  }

  vm::load_cell_slice(root).dump(os);

  auto t = os.str();
  t.pop_back();

  return "<VmDict " + t + ">";
}

std::string PyDict::dump() const {
  std::stringstream os;
  const auto d = *my_dict;
  td::Ref<vm::Cell> root = d.get_root_cell();
  vm::load_cell_slice(root).print_rec(os);

  return os.str();
}