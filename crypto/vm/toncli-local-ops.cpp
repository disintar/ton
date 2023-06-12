/*
    This file is part of TON Blockchain Library.

    TON Blockchain Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    TON Blockchain Library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with TON Blockchain Library.  If not, see <http://www.gnu.org/licenses/>.

    Copyright 2017-2020 Telegram Systems LLP
*/
#include <functional>
#include "vm/toncli-local-ops.h"
#include "vm/log.h"
#include "vm/opctable.h"
#include "vm/stack.hpp"
#include "vm/excno.hpp"
#include "vm/vm.h"
#include "Ed25519.h"

namespace vm {

int exec_gas_limits_temp(VmState* st) {
  VM_LOG(st) << "execute GASLIMITSTEMP";
  auto gas = st->get_gas_limits();
  Stack& stack = st->get_stack();
  stack.push_smallint(gas.gas_limit);
  stack.push_smallint(gas.gas_remaining);
  return 0;
}

int exec_priv_to_pub(VmState* st) {
  VM_LOG(st) << "execute PRIVTOPUB";
  Stack& stack = st->get_stack();
  auto key_int = stack.pop_int();
  unsigned char key_bytes[32];
  if (!key_int->export_bytes(key_bytes, 32, false)) {
    throw VmError{Excno::range_chk, "Ed25519 private key must fit in an unsigned 256-bit integer"};
  }
  td::Ed25519::PrivateKey priv_key{td::SecureString(td::Slice{key_bytes, 32})};
  auto pub_key = priv_key.get_public_key();
  if (pub_key.is_error()) {
    throw VmError{Excno::unknown, pub_key.error().to_string()};
  }
  auto pub_key_str = pub_key.ok().as_octet_string();
  td::RefInt256 pub_key_int{true};
  CHECK(pub_key_int.write().import_bytes((unsigned char*)pub_key_str.data(), pub_key_str.size(), false));
  stack.push_int(std::move(pub_key_int));
  return 0;
}

int exec_sign(VmState* st) {
  VM_LOG(st) << "execute SIGN";
  Stack& stack = st->get_stack();
  auto key_int = stack.pop_int();
  auto x = stack.pop_int();
  unsigned char key_bytes[32];
  if (!key_int->export_bytes(key_bytes, 32, false)) {
    throw VmError{Excno::range_chk, "Ed25519 private key must fit in an unsigned 256-bit integer"};
  }
  unsigned char data_bytes[32];
  if (!x->export_bytes(data_bytes, 32, false)) {
    throw VmError{Excno::range_chk, "Hash must fit in an unsigned 256-bit integer"};
  }
  td::Ed25519::PrivateKey priv_key{td::SecureString(td::Slice{key_bytes, 32})};
  auto signature = priv_key.sign(td::Slice(data_bytes, 32));
  if (signature.is_error()) {
    throw VmError{Excno::unknown, signature.error().to_string()};
  }
  CellBuilder cb;
  cb.store_bytes(signature.ok().as_slice());
  stack.push_cellslice(td::Ref<CellSlice>(true, cb.finalize()));
  return 0;
}

int exec_reset_loaded_cells(VmState* st) {
  VM_LOG(st) << "execute RESETLOADEDCELLS";
  st->reset_loaded_cells();
  return 0;
}

void register_toncli_local_ops(OpcodeTable& cp0) {
  using namespace std::placeholders;
  cp0.insert(OpcodeInstr::mksimple(0xfeef10, 24, "GASLIMITSTEMP", exec_gas_limits_temp))
     .insert(OpcodeInstr::mksimple(0xfeef11, 24, "PRIVTOPUB", exec_priv_to_pub))
     .insert(OpcodeInstr::mksimple(0xfeef12, 24, "SIGN", exec_sign))
     .insert(OpcodeInstr::mksimple(0xfeef13, 24, "RESETLOADEDCELLS", exec_reset_loaded_cells));
}

}  // namespace vm
