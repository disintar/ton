// Copyright 2023 Disintar LLP / andrey@head-labs.com

#include "third-party/pybind11/include/pybind11/pybind11.h"
#include "blockchain-indexer/json.hpp"
#include "blockchain-indexer/json-utils.hpp"
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
#include <string>
#include <cassert>
#include <codecvt>
#include <iostream>
#include <locale>

#include "vm/vm.h"
#include "vm/vmstate.h"
#include "td/utils/base64.h"
#include <utility>
#include "vm/boc.h"
#include "vm/cellslice.h"
#include "vm/cp0.h"
#include "third-party/pybind11/include/pybind11/stl.h"
#include "block/block.h"
#include "block/block-parse.h"
#include "td/utils/crypto.h"
#include "crypto/fift/Fift.h"
#include "crypto/fift/IntCtx.h"
#include "crypto/fift/words.h"
#include "td/utils/filesystem.h"
#include "td/utils/PathView.h"
#include "td/utils/port/path.h"
#include "crypto/common/refint.h"
#include "vm/dumper.hpp"
#include "PyCellSlice.h"
#include "PyCell.h"

namespace py = pybind11;
using json = nlohmann::json;

using namespace pybind11::literals;  // to bring in the `_a` literal'

std::string code_disassemble(const td::Ref<vm::Cell> &codeCell, const std::string &basePath,
                             const td::Ref<vm::Cell> &vmlibsCell) {
  fift::Fift::Config config;

  config.source_lookup = fift::SourceLookup(std::make_unique<fift::OsFileLoader>());
  config.source_lookup.add_include_path(basePath);

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
  ss << "@vmlibs ! std-disasm disasmc ";

  fift::IntCtx ctx{ss, "stdin", basePath, 0};
  ctx.stack.push_cell(codeCell);
  ctx.stack.push_maybe_cell(vmlibsCell);

  ctx.source_lookup = &fift.config().source_lookup;
  ctx.dictionary = ctx.context = ctx.main_dictionary = fift.config().dictionary;
  ctx.output_stream = &output;
  ctx.error_stream = fift.config().error_stream;

  try {
    auto res = ctx.run(td::make_ref<fift::InterpretCont>());
    if (res.is_error()) {
      std::cerr << "Disasm error: " << res.move_as_error().to_string();
      std::cerr << "Backtrace:";
      std::ostringstream os;
      ctx.print_error_backtrace(os);
      std::cerr << os.str();
      throw std::invalid_argument("Error in disassembler");
    } else {
      auto disasm_out = output.str();
      // cheap no-brainer
      std::string pattern = " ok\n";
      std::string::size_type n = pattern.length();
      for (std::string::size_type i = disasm_out.find(pattern); i != std::string::npos; i = disasm_out.find(pattern)) {
        disasm_out.erase(i, n);
      }

      return disasm_out;
    }
  } catch (const std::exception &e) {
    std::cerr << "Disasm error: " << e.what();
    throw std::invalid_argument("Error in disassembler");
  }
}

std::string code_dissemble_str(const std::string &code, const std::string &basePath, const PyCell &vmlibs) {
  auto codeCell = parse_string_to_cell(code);
  if (vmlibs.is_null()) {
    return code_disassemble(std::move(codeCell.my_cell), basePath, {});
  } else {
    auto c = vmlibs.my_cell;
    return code_disassemble(std::move(codeCell.my_cell), basePath, c.is_null() ? td::Ref<vm::Cell>{} : c);
  }
}

std::string code_dissemble_cell(const PyCell &codeCell, const std::string &basePath, const PyCell &vmlibs) {
  if (vmlibs.is_null()) {
    return code_disassemble(codeCell.my_cell, basePath, {});
  } else {
    auto c = vmlibs.my_cell;
    return code_disassemble(codeCell.my_cell, basePath, c.is_null() ? td::Ref<vm::Cell>{} : c);
  }
}

std::string parse_chunked_data(vm::CellSlice &cs) {
  vm::Dictionary dict{cs, 32};

  std::string slice;

  while (!dict.is_empty()) {
    td::BitArray<32> key{};
    dict.get_minmax_key(key);

    auto val = load_cell_slice(dict.lookup_delete_ref(key));
    slice += parse_snake_data_string(val, false);
  }

  return td::base64_encode(slice);
}

std::string onchain_hash_key_to_string(std::string hash) {
  std::vector<td::string> s = {"uri", "name", "description", "image", "image_data", "symbol",
                               "decimals", "amount_style", "render_type", "jetton", "master", "address"};

  for (const auto &it: s) {
    td::Bits256 tmp;
    td::sha256(td::Slice(it), tmp.as_slice());

    if (hash == tmp.to_hex()) {
      return it;
    }
  }

  return hash;
}

py::dict parse_token_data(const PyCell &codeCell) {
  try {
    auto cell = codeCell.my_cell;
    auto cs = load_cell_slice(cell);

    if (cs.size() < 8) {
      throw std::invalid_argument("Not valid cell slice, must be at least 8 bits");
    }

    int content_type;
    cs.fetch_uint_to(8, content_type);

    if (content_type == 0) {
      auto data = cs.fetch_ref();

      if (data.is_null()) {
        throw std::invalid_argument("Can't find ref with dictionary");
      }

      vm::Dictionary data_dict{data, 256};
      py::dict py_dict;

      while (!data_dict.is_empty()) {
        td::BitArray<256> key{};
        data_dict.get_minmax_key(key);

        auto key_text = onchain_hash_key_to_string(key.to_hex());

        td::Ref<vm::Cell> value = data_dict.lookup_delete(key)->prefetch_ref();
        if (value.not_null()) {
          std::stringstream a;

          auto vs = load_cell_slice(value);

          if (vs.size() >= 8) {
            int value_type;
            vs.fetch_uint_to(8, value_type);

            if (value_type == 0) {
              py::dict d("type"_a = "snake", "value"_a = parse_snake_data_string(vs));
              py_dict[py::str(key_text)] = d;
            } else if (value_type == 1) {
              py::dict d("type"_a = "chunks", "value"_a = parse_chunked_data(vs));
              py_dict[py::str(key_text)] = d;
            } else {
              py::dict d("type"_a = "unknown", "value"_a = "");
              py_dict[py::str(key_text)] = d;
            }
          } else {
            py::dict d("type"_a = "unknown", "value"_a = "");
            py_dict[py::str(key_text)] = d;
          }
        }
      };

      py::dict d("type"_a = "onchain", "value"_a = py_dict);
      return d;
    } else if (content_type == 1) {
      py::dict d("type"_a = "offchain", "value"_a = parse_snake_data_string(cs));
      return d;
    } else {
      throw std::invalid_argument("Not valid prefix, must be 0x00 / 0x01");
    }
  } catch (const vm::VmError &e) {
    throw std::runtime_error(e.get_msg());
  } catch (const std::exception &e) {
    throw std::invalid_argument("Can't parse token data");
  }
}

PyCellSlice pack_address(const std::string &address) {
  auto paddr_parse = block::StdAddress::parse(address);

  if (paddr_parse.is_ok()) {
    auto paddr = paddr_parse.move_as_ok();
    td::BigInt256 dest_addr;
    vm::CellBuilder cb;

    dest_addr.import_bits(paddr.addr.as_bitslice());
    cb.store_ones(1).store_zeroes(2).store_long(paddr.workchain, 8).store_int256(dest_addr, 256);
    return PyCellSlice(cb.finalize(), false);
  } else {
    throw std::invalid_argument("Parse address error: not valid address");
  }
}

std::string parse_shard_account(PyCell &shard_account) {
  block::gen::ShardAccount::Record sa;

  if (!tlb::unpack_cell(shard_account.my_cell, sa)) {
    throw std::invalid_argument("Cell is invalid ShardAccount::Record");
  }

  json data;

  data["account"] = {{"last_trans_hash", sa.last_trans_hash.to_hex()},
                     {"last_trans_lt",   sa.last_trans_lt}};

  auto account_cell = load_cell_slice(sa.account);
  auto acc_tag = block::gen::t_Account.get_tag(account_cell);

  if (acc_tag == block::gen::t_Account.account) {
    data["account"]["type"] = "account";
    block::gen::Account::Record_account acc;
    block::gen::StorageInfo::Record si;
    block::gen::AccountStorage::Record as;
    block::gen::StorageUsed::Record su;
    block::gen::CurrencyCollection::Record balance;

    if (!tlb::unpack(account_cell, acc)) {
      throw std::invalid_argument("Cell is invalid Account::Record_account");
    }

    if (!tlb::unpack(acc.storage.write(), as)) {
      throw std::invalid_argument("Cell is invalid StorageInfo::Record");
    };

    if (!tlb::unpack(acc.storage_stat.write(), si)) {
      throw std::invalid_argument("Cell is invalid AccountStorage::Record");
    };

    if (!tlb::unpack(si.used.write(), su)) {
      throw std::invalid_argument("Cell is invalid StorageUsed::Record");
    };

    if (!tlb::unpack(as.balance.write(), balance)) {
      throw std::invalid_argument("Cell is invalid CurrencyCollection::Record ");
    };

    data["account"]["addr"] = parse_address(acc.addr.write());
    std::string due_payment;

    if (si.due_payment->prefetch_ulong(1) > 0) {
      auto due = si.due_payment.write();
      due.fetch_bits(1);  // maybe
      due_payment = block::tlb::t_Grams.as_integer(due)->to_dec_string();
    }

    data["account"]["storage_stat"] = {{"last_paid",   si.last_paid},
                                       {"due_payment", due_payment}};

    data["account"]["storage_stat"]["used"] = {
            {"cells", block::tlb::t_VarUInteger_7.as_uint(su.cells.write())},
            {"bits",  block::tlb::t_VarUInteger_7.as_uint(su.bits.write())},
    };

    data["account"]["storage"] = {{"last_trans_lt", as.last_trans_lt}};

    std::vector<std::tuple<int, std::string>> dummy;
    data["account"]["storage"]["balance"] = {
            {"grams", block::tlb::t_Grams.as_integer(balance.grams)->to_dec_string()},
            {"extra", balance.other->have_refs() ? parse_extra_currency(balance.other->prefetch_ref()) : dummy}};

    auto tag = block::gen::t_AccountState.get_tag(as.state.write());

    if (tag == block::gen::t_AccountState.account_uninit) {
      data["account"]["state"] = {{"type", "uninit"}};
    } else if (tag == block::gen::t_AccountState.account_active) {
      block::gen::AccountState::Record_account_active active_account;
      CHECK(tlb::unpack(as.state.write(), active_account));

      data["account"]["state"] = {{"type",       "active"},
                                  {"state_init", parse_state_init(active_account.x.write())}};

    } else if (tag == block::gen::t_AccountState.account_frozen) {
      block::gen::AccountState::Record_account_frozen f{};
      CHECK(tlb::unpack(as.state.write(), f))
      data["account"]["state"] = {{"type",       "frozen"},
                                  {"state_hash", f.state_hash.to_hex()}};
    }
  } else {
    data["account"]["type"] = "account_none";
  }

  return data.dump(-1);
};
