// Copyright 2023 Disintar LLP / andrey@head-labs.com

#include "third-party/pybind11/include/pybind11/pybind11.h"
#include <string>
#include "vm/vmstate.h"
#include <utility>
#include <optional>
#include "vm/cp0.h"
#include "vm/cellslice.h"
#include "crypto/vm/cellslice.h"
#include "PyCellSlice.h"
#include "PyCell.h"
#include "PyCellBuilder.h"
#include "PyFift.h"

namespace py = pybind11;

int PyFift::run(std::string code_text) {
  std::stringstream ss;
  ss << td::read_file_str(base_path + "Fift.fif").ok();

  for (auto f : libs) {
    const auto fiftLib = td::read_file_str(base_path + f).ok();
    ss << fiftLib;
  }
  ss << code_text;

  std::ostringstream output;
  fift::Fift::Config config;
  config.source_lookup = fift::SourceLookup(std::make_unique<fift::OsFileLoader>());
  config.source_lookup.add_include_path(base_path);

  fift::init_words_common(config.dictionary);
  fift::init_words_vm(config.dictionary);
  fift::init_words_ton(config.dictionary);

  fift::Fift fift(std::move(config));
  ctx = fift::IntCtx{ss, "stdin", base_path, 0};
  ctx.ton_db = &fift.config().ton_db;
  ctx.output_stream = &output;
  ctx.source_lookup = &fift.config().source_lookup;
  ctx.dictionary = ctx.context = ctx.main_dictionary = fift.config().dictionary;
  ctx.error_stream = fift.config().error_stream;

  auto res = ctx.run(td::make_ref<fift::InterpretCont>());

  if (!silent) {
    py::print(output.str());
  }

  if (res.is_ok()) {
    return res.move_as_ok();
  } else {
    std::ostringstream os;
    ctx.print_error_backtrace(os);
    std::cerr << "Backtrace:" << os.str();
    throw std::invalid_argument(res.move_as_error().message().str());
  }
}

PyStack PyFift::get_stack() {
  return PyStack(ctx.stack);
}