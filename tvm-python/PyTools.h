//
// Created by Andrey Tvorozhkov on 5/9/23.
//

#include "third-party/pybind11/include/pybind11/pybind11.h"
#include "vm/vm.h"
#include "tvm-python/PyCell.h"

#ifndef TON_PYTOOLS_H
#define TON_PYTOOLS_H

namespace py = pybind11;

std::string code_disassemble(const td::Ref<vm::Cell>& codeCell, const std::string& basePath);
std::string code_dissemble_str(const std::string& code, const std::string& basePath);
std::string code_dissemble_cell(const PyCell& codeCell, const std::string& basePath);
py::dict parse_token_data(const PyCell& codeCell);
PyCell run_asm(const std::string& code, const std::string& base_path);

#endif  //TON_PYTOOLS_H
