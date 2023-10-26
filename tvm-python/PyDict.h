// Copyright 2023 Disintar LLP / andrey@head-labs.com

#include "third-party/pybind11/include/pybind11/pybind11.h"
#include "third-party/pybind11/include/pybind11/cast.h"
#include <string>
#include <optional>
#include <utility>
#include "block/block.h"
#include "block/block-parse.h"
#include "vm/dumper.hpp"
#include "crypto/vm/cellslice.h"
#include "PyCellSlice.h"
#include "PyCell.h"
#include "PyCellBuilder.h"

#ifndef TON_PYDICT_H
#define TON_PYDICT_H

namespace py = pybind11;

struct PyAugmentationCheckData : vm::dict::AugmentationData {
  py::function py_eval_leaf;
  py::function py_skip_extra;
  py::function py_eval_fork;
  py::function py_eval_empty;

  PyAugmentationCheckData(py::function& py_eval_leaf_, py::function& py_skip_extra_, py::function& py_eval_fork_,
                          py::function& py_eval_empty_)
      : py_eval_leaf(py_eval_leaf_)
      , py_skip_extra(py_skip_extra_)
      , py_eval_fork(py_eval_fork_)
      , py_eval_empty(py_eval_empty_) {
  }

  PyAugmentationCheckData() {}

  bool eval_leaf(vm::CellBuilder& cb, vm::CellSlice& cs) const override;
  bool skip_extra(vm::CellSlice& cs) const override;
  bool eval_fork(vm::CellBuilder& cb, vm::CellSlice& left_cs, vm::CellSlice& right_cs) const override;
  bool eval_empty(vm::CellBuilder& cb) const override;
};

class PyDict {
 public:
  std::unique_ptr<vm::DictionaryFixed> my_dict;
  PyAugmentationCheckData aug;
  unsigned int key_len;
  bool sgnd, is_augmented;

  explicit PyDict(int key_len_, bool sgnd_ = false, std::optional<PyCellSlice> cs_root = std::optional<PyCellSlice>()) {
    if (cs_root) {
      vm::Dictionary my_dict_t{vm::DictNonEmpty(), cs_root.value().my_cell_slice, key_len_};
      my_dict = std::make_unique<vm::Dictionary>(my_dict_t);
    } else {
      vm::Dictionary my_dict_t{key_len_};
      my_dict = std::make_unique<vm::Dictionary>(my_dict_t);
    }
    key_len = key_len_;
    sgnd = sgnd_;
    aug = PyAugmentationCheckData();
    is_augmented = false;
  }

  explicit PyDict(int key_len_, PyAugmentationCheckData aug_, bool sgnd_ = false,
                  std::optional<PyCellSlice> cs_root = std::optional<PyCellSlice>()) {
    aug = std::move(aug_);

    if (cs_root) {
      td::Ref<vm::CellSlice> csr = td::make_ref<vm::CellSlice>(cs_root.value().my_cell_slice.clone());
      vm::AugmentedDictionary my_dict_t{vm::DictNonEmpty(), csr, key_len_, aug};
      my_dict = std::make_unique<vm::AugmentedDictionary>(my_dict_t);
    } else {
      vm::AugmentedDictionary my_dict_t{key_len_, aug};
      my_dict = std::make_unique<vm::AugmentedDictionary>(my_dict_t);
    }
    key_len = key_len_;
    sgnd = sgnd_;
    is_augmented = true;
  }

  PyCell get_pycell() const;
  PyDict* set(const std::string& key, PyCellSlice& value, const std::string& mode, int key_len_ = 0, int sgnd_ = -1);
  PyDict* set_ref(const std::string& key, PyCell& value, const std::string& mode, int key_len_ = 0, int sgnd_ = -1);
  PyDict* set_builder(const std::string& key, PyCellBuilder& value, const std::string& mode, int key_len_ = 0,
                      int sgnd_ = -1);
  PyCellSlice lookup(const std::string& key, int key_len_ = 0, int sgnd_ = -1) const;
  PyCellSlice lookup_delete(const std::string& key, int key_len_ = 0, int sgnd_ = -1) const;
  std::tuple<std::string, PyCellSlice> get_minmax_key(bool fetch_max = false, bool inver_first = false,
                                                      int key_len_ = 0, int sgnd_ = -1) const;
  std::tuple<std::string, PyCell> get_minmax_key_ref(bool fetch_max = false, bool inver_first = false, int key_len_ = 0,
                                                     int sgnd_ = -1) const;
  std::tuple<std::string, PyCellSlice> lookup_nearest_key(const std::string& key, bool fetch_next = true,
                                                          bool allow_eq = false, bool inver_first = false,
                                                          int key_len_ = 0, int sgnd_ = -1) const;
  PyCell lookup_ref(const std::string& key, int key_len_ = 0, int sgnd_ = -1) const;
  bool is_empty();
  PyCell lookup_delete_ref(const std::string& key, int key_len_ = 0, int sgnd_ = -1) const;
  std::string to_boc() const;
  std::string toString() const;
  std::string dump() const;
  void map(py::function& f);

  static void dummy_set() {
    throw std::invalid_argument("Not settable");
  }
};

auto get_mode(const std::string& s);

#endif  //TON_PYDICT_H
