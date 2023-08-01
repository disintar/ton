//
// Created by Andrey Tvorozhkov on 5/9/23.
//

#include "third-party/pybind11/include/pybind11/pybind11.h"
#include <string>
#include "td/utils/base64.h"
#include <utility>
#include "vm/boc.h"
#include "vm/cellslice.h"
#include "crypto/vm/cellslice.h"
#include <queue>
#include "block/block-auto.h"
#include "tvm-python/PyCell.h"
#include "crypto/fift/Fift.h"
#include "crypto/fift/IntCtx.h"
#include "crypto/fift/words.h"
#include "td/utils/filesystem.h"
#include "td/utils/PathView.h"
#include "td/utils/port/path.h"
#include "PyTools.h"

std::string code_disassemble(const td::Ref<vm::Cell>& codeCell, const std::string& basePath) {
  fift::Fift::Config config;

  config.source_lookup = fift::SourceLookup(std::make_unique<fift::OsFileLoader>());
  config.source_lookup.add_include_path("./lib");

  fift::init_words_common(config.dictionary);
  fift::init_words_vm(config.dictionary);
  fift::init_words_ton(config.dictionary);

  fift::Fift fift(std::move(config));

  std::stringstream ss;
  std::stringstream output;

  const auto fiftLib = td::read_file_str(basePath + "Fift.fif");
  const auto listsLib = td::read_file_str(basePath + "Lists.fif");
  const auto disasmLib = td::read_file_str(basePath + "Disasm.fif");

  // Fift.fif & Lists.fif & Disasm.fif
  ss << fiftLib.ok();
  ss << listsLib.ok();
  ss << disasmLib.ok();
  ss << "<s std-disasm disasm ";

  fift::IntCtx ctx{ss, "stdin", basePath, 0};
  ctx.stack.push_cell(codeCell);

  ctx.ton_db = &fift.config().ton_db;
  ctx.source_lookup = &fift.config().source_lookup;
  ctx.dictionary = ctx.context = ctx.main_dictionary = fift.config().dictionary;
  ctx.output_stream = &output;
  ctx.error_stream = fift.config().error_stream;

  try {
    auto res = ctx.run(td::make_ref<fift::InterpretCont>());
    if (res.is_error()) {
      std::cerr << "Disasm error: " << res.move_as_error().to_string();
      throw std::invalid_argument("Error in disassembler");
    } else {
      auto disasm_out = output.str();
      // cheap no-brainer
      std::string_view pattern = " ok\n";
      std::string::size_type n = pattern.length();
      for (std::string::size_type i = disasm_out.find(pattern); i != std::string::npos; i = disasm_out.find(pattern)) {
        disasm_out.erase(i, n);
      }

      return disasm_out;
    }
  } catch (const std::exception& e) {
    std::cerr << "Disasm error: " << e.what();
    throw std::invalid_argument("Error in disassembler");
  }
}

std::string code_dissemble_str(const std::string& code, const std::string& basePath) {
  auto codeCell = parseStringToCell(code);
  return code_disassemble(std::move(codeCell.my_cell), basePath);
}

std::string code_dissemble_cell(const PyCell& codeCell, const std::string& basePath) {
  return code_disassemble(codeCell.my_cell, basePath);
}
