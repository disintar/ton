//
// Created by Andrey Tvorozhkov on 5/9/23.
//

#include "vm/vm.h"
#include "tvm-python/PyCell.h"

#ifndef TON_PYTOOLS_H
#define TON_PYTOOLS_H

std::string code_disassemble(const td::Ref<vm::Cell>& codeCell, const std::string& basePath);
std::string code_dissemble_str(const std::string& code, const std::string& basePath);
std::string code_dissemble_cell(const PyCell& codeCell, const std::string& basePath);

#endif  //TON_PYTOOLS_H
