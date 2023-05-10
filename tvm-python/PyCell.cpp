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
#include "tvm-python/PyCellSlice.h"
#include "PyCell.h"

PyCellSlice PyCell::begin_parse() const {
  return PyCellSlice(my_cell);
}

std::string PyCell::get_hash() const {
  return my_cell->get_hash().to_hex();
}

std::string PyCell::toString() const {
  if (my_cell.not_null()) {
    std::stringstream os;
    vm::load_cell_slice(my_cell).dump(os);

    auto t = os.str();
    t.pop_back();

    return "<Cell " + t + ">";
  } else {
    return "<Cell null>";
  }
}

std::string PyCell::dump() const {
  std::stringstream os;
  vm::load_cell_slice(my_cell).print_rec(os);

  return os.str();
}

std::string PyCell::dump_as_tlb(std::string tlb_type) const {
  tlb::TypenameLookup tlb_dict0;
  tlb_dict0.register_types(block::gen::register_simple_types);

  auto _template = tlb_dict0.lookup(std::move(tlb_type));

  if (!_template) {
    throw std::invalid_argument("Parse tlb error: not valid tlb type");
  }

  std::stringstream ss;
  _template->print_ref(9 << 20, ss, my_cell);

  auto output = ss.str();
  return output;
}

std::string PyCell::to_boc() const {
  return td::base64_encode(std_boc_serialize(my_cell, 31).move_as_ok());
}

bool PyCell::is_null() const {
  return my_cell.is_null();
}

// type converting utils
PyCell parseStringToCell(const std::string& base64string) {
  auto base64decoded = td::base64_decode(td::Slice(base64string));

  if (base64decoded.is_error()) {
    throw std::invalid_argument("Parse code error: invalid base64");
  }

  auto boc_decoded = vm::std_boc_deserialize(base64decoded.move_as_ok());

  if (boc_decoded.is_error()) {
    throw std::invalid_argument("Parse code error: invalid BOC");
  }

  return PyCell(boc_decoded.move_as_ok());
}
