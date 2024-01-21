// Copyright 2023 Disintar LLP / andrey@head-labs.com
#include <string>
#include <block/block.h>
#include <ton/ton-shard.h>
#include "PyCellSlice.h"
#include "PyCellBuilder.h"

#ifndef TON_PYSMCADDRESS_H
#define TON_PYSMCADDRESS_H

class PySmcAddress {
 public:
  PySmcAddress(block::StdAddress my_address_) {
    my_address = std::move(my_address_);
  }

  PySmcAddress() = default;

  int wc() {
    return my_address.workchain;
  }

  void set_workchain(int wc) {
    my_address.workchain = wc;
  }

  std::string address() {
    return my_address.addr.to_hex();
  }

  bool bounceable() {
    return my_address.bounceable;
  }

  void set_bounceable(bool flag) {
    my_address.bounceable = flag;
  }

  bool testnet() {
    return my_address.testnet;
  }

  void set_testnet(bool flag) {
    my_address.testnet = flag;
  }

  bool operator==(const PySmcAddress& other) {
    return other.my_address == my_address;
  };

  std::string rserialize(bool base64_url) {
    return my_address.rserialize(base64_url);
  }

  unsigned long long shard_prefix(int len) {
    return ton::shard_prefix(my_address.addr, len);
  }

  bool append_to_builder(PyCellBuilder& cb);
  PyCellSlice pack();

  block::StdAddress my_address;
};

PySmcAddress address_from_string(std::string address);
PySmcAddress address_from_cell_slice(PyCellSlice& cs);

#endif  //TON_PYSMCADDRESS_H
