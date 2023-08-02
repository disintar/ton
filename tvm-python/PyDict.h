//
// Created by Andrey Tvorozhkov on 5/9/23.
//

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

class PyDict {
 public:
  std::unique_ptr<vm::Dictionary> my_dict;
  unsigned int key_len;
  bool sgnd;

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

  static void dummy_set() {
    throw std::invalid_argument("Not settable");
  }
};

auto get_mode(const std::string& s);

#endif  //TON_PYDICT_H
