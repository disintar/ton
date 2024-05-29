// Copyright 2023 Disintar LLP / andrey@head-labs.com

#include "third-party/pybind11/include/pybind11/pybind11.h"
#include "third-party/pybind11/include/pybind11/cast.h"
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

bool PyAugmentationCheckData::eval_leaf(vm::CellBuilder& cb, vm::CellSlice& cs) const {
  vm::CellBuilder tmp_cb;
  tmp_cb.append_cellslice(cs.clone());
  td::Ref<vm::Cell> cell = tmp_cb.finalize(cs.is_special());
  auto tmp_cs = PyCellSlice(std::move(cell));

  py::object result_py = py_eval_leaf(tmp_cs);
  py::tuple t = py::reinterpret_borrow<py::tuple>(result_py);
  bool is_ok = t[0].cast<bool>();
  if (is_ok) {
    PyCellSlice& result = t[1].cast<PyCellSlice&>();
    cb.append_cellslice(result.my_cell_slice);
    return true;
  } else {
    return is_ok;
  }
}

bool PyAugmentationCheckData::skip_extra(vm::CellSlice& cs) const {
  vm::CellBuilder tmp_cb;
  tmp_cb.append_cellslice(cs.clone());
  td::Ref<vm::Cell> cell = tmp_cb.finalize(cs.is_special());
  auto tmp_cs = PyCellSlice(std::move(cell));

  py::object result_py = py_skip_extra(tmp_cs);
  py::tuple t = py::reinterpret_borrow<py::tuple>(result_py);
  bool is_ok = t[0].cast<bool>();
  if (is_ok) {
    PyCellSlice& result = t[1].cast<PyCellSlice&>();
    // TODO: find better way
    cs = result.my_cell_slice;
    return true;
  } else {
    return is_ok;
  }
}

bool PyAugmentationCheckData::eval_fork(vm::CellBuilder& cb, vm::CellSlice& left_cs, vm::CellSlice& right_cs) const {
  vm::CellBuilder ltmp_cb;
  ltmp_cb.append_cellslice(left_cs.clone());
  td::Ref<vm::Cell> lcell = ltmp_cb.finalize(left_cs.is_special());
  auto tmp_cs_left = PyCellSlice(std::move(lcell));

  vm::CellBuilder tmp_cb;
  tmp_cb.append_cellslice(right_cs.clone());
  td::Ref<vm::Cell> rcell = tmp_cb.finalize(right_cs.is_special());
  auto tmp_cs_right = PyCellSlice(std::move(rcell));

  py::object result_py = py_eval_fork(tmp_cs_left, tmp_cs_right);
  py::tuple t = py::reinterpret_borrow<py::tuple>(result_py);
  bool is_ok = t[0].cast<bool>();
  if (is_ok) {
    PyCellSlice& result = t[1].cast<PyCellSlice&>();
    cb.append_cellslice(result.my_cell_slice);
    return true;
  } else {
    return is_ok;
  }
}

bool PyAugmentationCheckData::eval_empty(vm::CellBuilder& cb) const {
  py::object result_py = py_eval_empty();
  py::tuple t = py::reinterpret_borrow<py::tuple>(result_py);
  bool is_ok = t[0].cast<bool>();
  if (is_ok) {
    PyCellSlice& result = t[1].cast<PyCellSlice&>();
    cb.append_cellslice(result.my_cell_slice);
    return true;
  } else {
    return is_ok;
  }
}

PyCell PyDict::get_pycell() const {
    if (is_augmented) {
        auto dict = static_cast<vm::AugmentedDictionary*>(my_dict.get());
        return PyCell(dict->get_root()->get_base_cell());
    }
    td::Ref<vm::Cell> root = my_dict->get_root_cell();
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

  auto* tmp = new unsigned char[key_len_];
  x.export_bits(td::BitPtr{tmp}, key_len_, sgnd_);

  const auto mdict = my_dict.get();
  mdict->set(td::BitPtr{tmp}, key_len_, td::make_ref<vm::CellSlice>(value.my_cell_slice.clone()), get_mode(mode));
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
  x.export_bits(td::BitPtr{tmp}, key_len_, sgnd_);

  const auto mdict = my_dict.get();

  mdict->set_ref(td::BitPtr{tmp}, key_len_, value.my_cell, get_mode(mode));
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
  x.export_bits(td::BitPtr{tmp}, key_len_, sgnd_);

  const auto mdict = my_dict.get();

  mdict->set_builder(td::BitPtr{tmp}, key_len_, value.my_builder, get_mode(mode));
  return this;
}

//  bool set(td::ConstBitPtr key, int key_len, Ref<CellSlice> value, SetMode mode = SetMode::Set);
PyDict* PyDict::set_keycs(PyCellSlice& key, PyCellSlice& value, const std::string& mode, int key_len_) {
  if (key_len_ == 0) {
    key_len_ = key_len;
  }

  const auto mdict = my_dict.get();
  mdict->set(key.my_cell_slice.prefetch_bits(key_len_).bits(), key_len_,
             td::make_ref<vm::CellSlice>(value.my_cell_slice.clone()), get_mode(mode));
  return this;
}

//  bool Dictionary::set_ref(td::ConstBitPtr key, int key_len, Ref<Cell> val_ref, SetMode mode)
PyDict* PyDict::set_keycs_ref(PyCellSlice& key, PyCell& value, const std::string& mode, int key_len_) {

  if (key_len_ == 0) {
    key_len_ = key_len;
  }

  const auto mdict = my_dict.get();

  mdict->set_ref(key.my_cell_slice.prefetch_bits(key_len_).bits(), key_len_, value.my_cell, get_mode(mode));
  return this;
}

// bool Dictionary::set_builder(td::ConstBitPtr key, int key_len, Ref<CellBuilder> val_b, SetMode mode) {
PyDict* PyDict::set_keycs_builder(PyCellSlice& key, PyCellBuilder& value, const std::string& mode, int key_len_) {
  if (key_len_ == 0) {
    key_len_ = key_len;
  }

  const auto mdict = my_dict.get();

  mdict->set_builder(key.my_cell_slice.prefetch_bits(key_len_).bits(), key_len_, value.my_builder, get_mode(mode));
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
  x.export_bits(td::BitPtr{tmp}, key_len_, sgnd_);
  const auto mdict = my_dict.get();
  auto cs = mdict->lookup(td::BitPtr{tmp}, key_len_);
  vm::CellBuilder cb;
  cb.append_cellslice(cs);

  return PyCellSlice(cb.finalize(), true);
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
  x.export_bits(td::BitPtr{tmp}, key_len_, sgnd_);
  const auto mdict = my_dict.get();

  auto cs = mdict->lookup_delete(td::BitPtr{tmp}, key_len_);
  vm::CellBuilder cb;
  cb.append_cellslice(cs);

  return PyCellSlice(cb.finalize(), true);
}

PyCellSlice PyDict::lookup_keycs(PyCellSlice& key, int key_len_) const {
  if (key_len_ == 0) {
    key_len_ = key_len;
  }

  const auto mdict = my_dict.get();
  auto cs = mdict->lookup(key.my_cell_slice.prefetch_bits(key_len_).bits(), key_len_);
  vm::CellBuilder cb;
  cb.append_cellslice(cs);

  return PyCellSlice(cb.finalize(), true);
}

PyCellSlice PyDict::lookup_keycs_delete(PyCellSlice& key, int key_len_) const {
  if (key_len_ == 0) {
    key_len_ = key_len;
  }

  const auto mdict = my_dict.get();

  auto cs = mdict->lookup_delete(key.my_cell_slice.prefetch_bits(key_len_).bits(), key_len_);
  vm::CellBuilder cb;
  cb.append_cellslice(cs);

  return PyCellSlice(cb.finalize(), true);
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
  const auto cs = mdict->get_minmax_key(td::BitPtr{tmp}, key_len_, fetch_max, inver_first);

  vm::CellBuilder cb;
  cb.append_cellslice(cs);

  td::BigInt256 x;
  x.import_bits(td::BitPtr{tmp}, key_len_, sgnd_);

  return std::make_tuple(x.to_dec_string(), PyCellSlice(cb.finalize(), true));
}

std::tuple<std::string, PyCell> PyDict::get_minmax_key_ref(bool fetch_max, bool inver_first, int key_len_,
                                                           int sgnd_) const {
  if (sgnd_ == -1) {
    sgnd_ = sgnd;
  }

  if (key_len_ == 0) {
    key_len_ = key_len;
  }
  auto* tmp = new unsigned char[key_len_];
  const auto mdict = my_dict.get();
  const auto cell = mdict->get_minmax_key_ref(td::BitPtr{tmp}, key_len_, fetch_max, inver_first);

  if (cell.is_null()) {
    throw vm::CellBuilder::CellCreateError();
  }

  td::BigInt256 x;
  x.import_bits(td::BitPtr{tmp}, key_len_, sgnd_);

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
  x.export_bits(td::BitPtr{tmp}, key_len_, sgnd_);

  const auto mdict = my_dict.get();
  const auto cs = mdict->lookup_nearest_key(td::BitPtr{tmp}, key_len_, fetch_next, allow_eq, inver_first);

  td::BigInt256 x2;
  x2.import_bits(td::BitPtr{tmp}, key_len_, sgnd_);

  vm::CellBuilder cb;
  cb.append_cellslice(cs);

  return std::make_tuple(x2.to_dec_string(), PyCellSlice(cb.finalize(), true));
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
  x.export_bits(td::BitPtr{tmp}, key_len_, sgnd_);
  const auto mdict = my_dict.get();

  auto cr = mdict->lookup_ref(td::BitPtr{tmp}, key_len_);

  if (cr.is_null()) {
    throw vm::CellBuilder::CellCreateError();
  }

  return PyCell(cr);
}

PyCell PyDict::lookup_keycs_ref(PyCellSlice& key, int key_len_) const {
  if (key_len_ == 0) {
    key_len_ = key_len;
  }

  const auto mdict = my_dict.get();

  auto cr = mdict->lookup_ref(key.my_cell_slice.prefetch_bits(key_len_).bits(), key_len_);

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
  x.export_bits(td::BitPtr{tmp}, key_len_, sgnd_);
  const auto mdict = my_dict.get();

  auto cr = mdict->lookup_delete_ref(td::BitPtr{tmp}, key_len_);

  if (cr.is_null()) {
    throw vm::CellBuilder::CellCreateError();
  }

  return PyCell(cr);
}

PyCell PyDict::lookup_keycs_delete_ref(PyCellSlice& key, int key_len_) const {
  if (key_len_ == 0) {
    key_len_ = key_len;
  }
  const auto mdict = my_dict.get();

  auto cr = mdict->lookup_delete_ref(key.my_cell_slice.prefetch_bits(key_len_).bits(), key_len_);

  if (cr.is_null()) {
    throw vm::CellBuilder::CellCreateError();
  }

  return PyCell(cr);
}

void PyDict::map(py::function& f) {
  auto clear_f = std::move(f);

  my_dict->check_for_each([&clear_f](td::Ref<vm::CellSlice> cs, td::ConstBitPtr ptr, int ptr_bits) {
    vm::CellBuilder cb;
    cb.store_bits(ptr, ptr_bits);

    auto key_cs = PyCellSlice(cb.finalize(), false);

    vm::CellBuilder tmp_cb;

    auto css = cs->clone();
    tmp_cb.store_bits(css.fetch_bits(cs->size()));
    for (unsigned i = 0; i < cs->size_refs(); i++) {
        tmp_cb.store_ref(cs->prefetch_ref(i));
    }

    td::Ref<vm::Cell> cell = tmp_cb.finalize(cs->is_special());
    auto value_cs = PyCellSlice(std::move(cell));
    bool result_py = clear_f(key_cs, value_cs).cast<bool>();

    return result_py;
  });
}

std::string PyDict::to_boc() const {
  td::Ref<vm::Cell> root = my_dict->get_root_cell();

  return td::base64_encode(std_boc_serialize(root, 31).move_as_ok());
}

std::string PyDict::toString() const {
  std::stringstream os;
  td::Ref<vm::Cell> root = my_dict->get_root_cell();
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
  td::Ref<vm::Cell> root = my_dict->get_root_cell();
  vm::load_cell_slice(root).print_rec(os);

  return os.str();
}