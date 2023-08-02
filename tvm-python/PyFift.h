//
// Created by Andrey Tvorozhkov on 5/9/23.
//

#include <string>
#include <utility>
#include <optional>
#include "block/block.h"
#include "block/block-parse.h"
#include "vm/dumper.hpp"
#include "crypto/vm/cellslice.h"
#include "PyCellSlice.h"
#include "PyCell.h"
#include "PyStack.h"
#include "PyCellBuilder.h"
#include "PyDict.h"
#include "third-party/pybind11/include/pybind11/pybind11.h"
#include "crypto/fift/Fift.h"
#include "crypto/fift/IntCtx.h"
#include "crypto/fift/words.h"
#include "td/utils/filesystem.h"

#ifndef TON_FIFT_H
#define TON_FIFT_H

class PyFift {
 public:
  explicit PyFift(std::string base_path_, bool silent_) {
    base_path = std::move(base_path_);
    silent = silent_;
  }

  int run(std::string code_text);
  void add_lib(std::string lib) {
    libs.push_back(lib);
  }
  void clear_libs() {
    libs.clear();
  }

  PyCell pop_cell();
  PyStack get_stack();


 private:
  std::string base_path;
  std::string code;
  fift::IntCtx ctx;
  std::vector<std::string> libs;
  bool silent;
};

#endif  //TON_FIFT_H
