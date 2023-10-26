// Copyright 2023 Disintar LLP / andrey@head-labs.com

#include "PySmcAddress.h"
#include "block/block-parse.h"

PyCellSlice PySmcAddress::pack() {
  auto x = block::tlb::MsgAddressInt().pack_std_address(my_address);
  vm::CellBuilder cb;
  cb.append_cellslice(x);
  return PyCellSlice(cb.finalize(), false);
}

bool PySmcAddress::append_to_builder(PyCellBuilder& cb) {
  return block::tlb::MsgAddressInt().store_std_address(cb.my_builder, my_address.workchain, my_address.addr);
}

PySmcAddress address_from_string(std::string address) {
  PySmcAddress a;
  if (!a.my_address.parse_addr(address) || (a.my_address.workchain != -1 && a.my_address.workchain != 0)) {
    throw std::invalid_argument("Invalid address, can't parse");
  };

  return a;
}

PySmcAddress address_from_cell_slice(PyCellSlice& cs) {
  PySmcAddress addr;
  if (!block::tlb::MsgAddressInt().extract_std_address(cs.my_cell_slice, addr.my_address)) {
    throw std::invalid_argument("Invalid address, can't parse");
  };
  return addr;
}