// Copyright 2023 Disintar LLP / andrey@head-labs.com

#include "third-party/pybind11/include/pybind11/pybind11.h"
#include <string>
#include "td/utils/base64.h"
#include <utility>
#include "vm/boc.h"
#include "vm/cellslice.h"
#include "crypto/vm/cellslice.h"
#include <queue>
#include "block/block-auto.h"
#include "PyCell.h"

std::string PyCell::get_hash() const {
  if (my_cell.not_null()) {
    return my_cell->get_hash().to_hex();
  } else {
    throw std::invalid_argument("Cell is null");
  }
}

int PyCell::get_depth() const {
  if (my_cell.not_null()) {
    return my_cell->get_depth();
  } else {
    throw std::invalid_argument("Cell is null");
  }
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

py::bytes PyCell::to_slice() const {
  return std_boc_serialize(my_cell, 31).move_as_ok().data();
}

std::string PyCell::dump() const {
  if (my_cell.is_null()) {
    throw std::invalid_argument("Cell is null");
  }

  std::stringstream os;
  vm::load_cell_slice(my_cell).print_rec(os);

  return os.str();
}

std::string PyCell::dump_as_tlb(std::string tlb_type) const {
  if (my_cell.is_null()) {
    throw std::invalid_argument("Cell is null");
  }

  tlb::TypenameLookup tlb_dict0;
  tlb_dict0.register_types(block::gen::register_simple_types);

  auto _template = tlb_dict0.lookup(std::move(tlb_type));

  if (!_template) {
    throw std::invalid_argument("Parse tlb error: not valid tlb type");
  }

  std::ostringstream ss;
  _template->print_ref(9 << 20, ss, my_cell);

  auto output = ss.str();
  return output;
}

std::string PyCell::to_boc() const {
  if (my_cell.is_null()) {
    throw std::invalid_argument("Cell is null");
  }

  return td::base64_encode(std_boc_serialize(my_cell, 31).move_as_ok());
}

bool PyCell::is_null() const {
  return my_cell.is_null();
}

PyCell PyCell::copy() const {
  vm::CellBuilder cb;
  bool special;
  bool success = cb.append_cellslice_bool(vm::load_cell_slice_special(my_cell, special));
  if (!success) {
    throw std::invalid_argument("Can't create cell copy");
  }

  return PyCell(cb.finalize_copy());
}

// type converting utils
PyCell parse_string_to_cell(const std::string& base64string) {
  auto base64decoded = td::base64_decode(base64string);

  if (base64decoded.is_error()) {
    throw std::invalid_argument("Parse code error: invalid base64");
  }

  auto boc_decoded = vm::std_boc_deserialize(base64decoded.move_as_ok());

  if (boc_decoded.is_error()) {
    auto m = boc_decoded.move_as_error().message();
    LOG(ERROR) << m;
    throw std::invalid_argument(m.str());
  }

  return PyCell(boc_decoded.move_as_ok());
}
