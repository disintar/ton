// Copyright 2023 Disintar LLP / andrey@head-labs.com

#include "mc-config.h"
#include "block/block.h"
#include "block/block-parse.h"
#include "block/block-auto.h"
#include "common/bitstring.h"
#include "vm/dict.h"
#include "td/utils/bits.h"
#include "td/utils/uint128.h"
#include "ton/ton-types.h"
#include "ton/ton-shard.h"
#include "openssl/digest.hpp"
#include <stack>
#include <algorithm>
#include "third-party/pybind11/include/pybind11/pybind11.h"
#include "emulator/transaction-emulator.h"
#include "emulator/tvm-emulator.hpp"
#include "PyCellSlice.h"
#include "PyCell.h"
#include "PyEmulator.h"

bool PyEmulator::set_rand_seed(const std::string &rand_seed_hex) {
    auto rand_seed_hex_slice = td::Slice(rand_seed_hex);
    if (rand_seed_hex_slice.size() != 64) {
        throw std::invalid_argument("Rand seed expected as 64 characters hex string");
    }

    auto rand_seed_bytes = td::hex_decode(rand_seed_hex_slice);
    if (rand_seed_bytes.is_error()) {
        throw std::invalid_argument("Can't decode hex rand seed");
    }

    td::BitArray<256> rand_seed{};
    rand_seed.as_slice().copy_from(rand_seed_bytes.move_as_ok());

    emulator->set_rand_seed(rand_seed);
    return true;
}

bool PyEmulator::set_ignore_chksig(bool ignore_chksig) {
    emulator->set_ignore_chksig(ignore_chksig);
    return true;
}

bool PyEmulator::set_libs(const PyCell &shardchain_libs_cell) {
    emulator->set_libs(vm::Dictionary(shardchain_libs_cell.my_cell, 256));
    return true;
}

bool PyEmulator::set_debug_enabled(bool debug_enabled) {
    emulator->set_debug_enabled(debug_enabled);
    return true;
}

bool PyEmulator::emulate_transaction(const PyCell &shard_account_cell, const PyCell &message_cell,
                                     const std::string &unixtime, const std::string &lt_str, int vm_ver,
                                     bool force_uninit) {
    py::gil_scoped_release release;
    auto message_cs = vm::load_cell_slice(message_cell.my_cell);
    int msg_tag = block::gen::t_CommonMsgInfo.get_tag(message_cs);

    block::gen::ShardAccount::Record shard_account;
    if (!tlb::unpack_cell(shard_account_cell.my_cell, shard_account)) {
        throw std::invalid_argument("Can't unpack shard account cell");
    }

    td::Ref<vm::CellSlice> addr_slice;
    auto account_slice = vm::load_cell_slice(shard_account.account);
    bool account_exists = (block::gen::t_Account.get_tag(account_slice) == block::gen::Account::account);
    if (block::gen::t_Account.get_tag(account_slice) == block::gen::Account::account_none) {
        if (msg_tag == block::gen::CommonMsgInfo::ext_in_msg_info) {
            block::gen::CommonMsgInfo::Record_ext_in_msg_info info;
            if (!tlb::unpack(message_cs, info)) {
                throw std::invalid_argument("Can't unpack inbound external message");
            }
            addr_slice = std::move(info.dest);
        } else if (msg_tag == block::gen::CommonMsgInfo::int_msg_info) {
            block::gen::CommonMsgInfo::Record_int_msg_info info;
            if (!tlb::unpack(message_cs, info)) {
                throw std::invalid_argument("Can't unpack inbound internal message");
            }
            addr_slice = std::move(info.dest);
        } else {
            throw std::invalid_argument("Only ext in and int message are supported");
        }
    } else if (block::gen::t_Account.get_tag(account_slice) == block::gen::Account::account) {
        block::gen::Account::Record_account account_record;
        if (!tlb::unpack(account_slice, account_record)) {
            throw std::invalid_argument("Can't unpack account cell");
        }
        addr_slice = std::move(account_record.addr);
    } else {
        throw std::invalid_argument("Can't parse account cell");
    }

    ton::WorkchainId wc;
    ton::StdSmcAddress addr;
    if (!block::tlb::t_MsgAddressInt.extract_std_address(addr_slice, wc, addr)) {
        throw std::invalid_argument("Can't extract account address");
    }

    auto account = block::Account(wc, addr.bits());
    ton::UnixTime now = static_cast<td::uint32>(std::stoull(unixtime));
    auto lt = static_cast<td::uint64>(std::stoull(lt_str));

    account.now_ = now;
    account.block_lt = lt - lt % block::ConfigInfo::get_lt_align();

    bool is_special = wc == ton::masterchainId && emulator->get_config().is_special_smartcontract(addr);
    if (account_exists) {
        if (!account.unpack(vm::load_cell_slice_ref(shard_account_cell.my_cell), now, is_special)) {
            throw std::invalid_argument("Can't unpack shard account");
        }
    } else {
        if (!account.init_new(now)) {
            throw std::invalid_argument("Can't init new account");
        }
        account.last_trans_lt_ = shard_account.last_trans_lt;
        account.last_trans_hash_ = shard_account.last_trans_hash;
    }

    if (force_uninit) {
        if (account.orig_status == 0 or account.orig_status == 1 or account.orig_status == 4) {
            account.orig_status = 1;  // acc_uninit
            account.status = 1;
        } else {
            throw std::invalid_argument("Can't force uninited state on inited account");
        }
    }

    auto result = emulator->emulate_transaction(std::move(account), message_cell.my_cell, now, lt,
                                                block::transaction::Transaction::tr_ord, vm_ver);

    if (result.is_error()) {
        throw std::invalid_argument("Emulate transaction failed: " + result.move_as_error().to_string());
    }

    auto emulation_result = result.move_as_ok();

    auto external_not_accepted =
            dynamic_cast<emulator::TransactionEmulator::EmulationExternalNotAccepted *>(emulation_result.get());
    if (external_not_accepted) {
        transaction_cell = td::Ref<vm::Cell>();
        actions_cell = td::Ref<vm::Cell>();
        vm_log = std::move(external_not_accepted->vm_log);
        vm_exit_code = external_not_accepted->vm_exit_code;
        elapsed_time = external_not_accepted->elapsed_time;
        return false;
    }

    auto emulation_success = dynamic_cast<emulator::TransactionEmulator::EmulationSuccess &>(*emulation_result);
    transaction_cell = std::move(emulation_success.transaction);

    auto new_shard_account_cell = vm::CellBuilder()
            .store_ref(emulation_success.account.total_state)
            .store_bits(emulation_success.account.last_trans_hash_.as_bitslice())
            .store_long(emulation_success.account.last_trans_lt_)
            .finalize();

    account_cell = std::move(new_shard_account_cell);
    actions_cell = std::move(emulation_success.actions);

    return true;
}

bool PyEmulator::emulate_tick_tock_transaction(const PyCell &shard_account_boc, bool is_tock,
                                               const std::string &unixtime, const std::string &lt_str, int vm_ver) {
    py::gil_scoped_release release;
    auto shard_account_cell = shard_account_boc.my_cell;

    if (shard_account_cell.is_null()) {
        throw std::invalid_argument("Shard account is null");
    }
    auto shard_account_slice = vm::load_cell_slice(shard_account_cell);
    block::gen::ShardAccount::Record shard_account;
    if (!tlb::unpack(shard_account_slice, shard_account)) {
        throw std::invalid_argument("Can't unpack shard account cell");
    }

    td::Ref<vm::CellSlice> addr_slice;
    auto account_slice = vm::load_cell_slice(shard_account.account);
    if (block::gen::t_Account.get_tag(account_slice) == block::gen::Account::account_none) {
        throw std::invalid_argument("Can't run tick/tock transaction on account_none");
    }
    block::gen::Account::Record_account account_record;
    if (!tlb::unpack(account_slice, account_record)) {
        throw std::invalid_argument("Can't unpack account cell");
    }
    addr_slice = std::move(account_record.addr);
    ton::WorkchainId wc;
    ton::StdSmcAddress addr;
    if (!block::tlb::t_MsgAddressInt.extract_std_address(addr_slice, wc, addr)) {
        throw std::invalid_argument("Can't extract account address");
    }

    auto account = block::Account(wc, addr.bits());
    ton::UnixTime now = static_cast<td::uint32>(std::stoull(unixtime));
    auto lt = static_cast<td::uint64>(std::stoull(lt_str));
    account.now_ = now;
    account.block_lt = lt - lt % block::ConfigInfo::get_lt_align();

    bool is_special = wc == ton::masterchainId && emulator->get_config().is_special_smartcontract(addr);
    if (!account.unpack(vm::load_cell_slice_ref(shard_account_cell), now, is_special)) {
        throw std::invalid_argument("Can't unpack shard account");
    }

    auto trans_type = is_tock ? block::transaction::Transaction::tr_tock : block::transaction::Transaction::tr_tick;
    auto result = emulator->emulate_transaction(std::move(account), {}, now, lt, trans_type, vm_ver);
    if (result.is_error()) {
        throw std::invalid_argument("Emulate transaction failed: " + result.move_as_error().to_string());
    }

    auto emulation_result = result.move_as_ok();

    auto external_not_accepted =
            dynamic_cast<emulator::TransactionEmulator::EmulationExternalNotAccepted *>(emulation_result.get());
    if (external_not_accepted) {
        transaction_cell = td::Ref<vm::Cell>();
        actions_cell = td::Ref<vm::Cell>();
        vm_log = std::move(external_not_accepted->vm_log);
        vm_exit_code = external_not_accepted->vm_exit_code;
        elapsed_time = external_not_accepted->elapsed_time;
        return false;
    }

    auto emulation_success = dynamic_cast<emulator::TransactionEmulator::EmulationSuccess &>(*emulation_result);
    transaction_cell = std::move(emulation_success.transaction);

    auto new_shard_account_cell = vm::CellBuilder()
            .store_ref(emulation_success.account.total_state)
            .store_bits(emulation_success.account.last_trans_hash_.as_bitslice())
            .store_long(emulation_success.account.last_trans_lt_)
            .finalize();

    account_cell = std::move(new_shard_account_cell);
    actions_cell = std::move(emulation_success.actions);

    return true;
}

std::string PyEmulator::get_vm_log() {
    return vm_log;
}

void PyEmulator::set_prev_blocks_info(PyStackEntry entry) {
    emulator->set_prev_blocks_info(entry.entry.as_tuple());
}

double PyEmulator::get_elapsed_time() {
    return elapsed_time;
}

PyCell PyEmulator::get_transaction_cell() {
    return PyCell(transaction_cell);
}

PyCell PyEmulator::get_account_cell() {
    return PyCell(account_cell);
}

PyCell PyEmulator::get_actions_cell() {
    return PyCell(actions_cell);
}
