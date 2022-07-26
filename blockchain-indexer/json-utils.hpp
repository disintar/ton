#ifndef TON_JSON_UTILS_HPP
#define TON_JSON_UTILS_HPP

#include "json.hpp"
#include "td/utils/logging.h"
#include "crypto/vm/cp0.h"
#include "tl/tlblib.hpp"
#include "block/block-parse.h"
#include "block/block-auto.h"
#include "vm/dict.h"
#include "vm/cells/MerkleProof.h"
#include "vm/vm.h"
#include "td/utils/Slice.h"
#include "td/utils/common.h"
#include "td/utils/base64.h"
#include "auto/tl/lite_api.h"
#include "tuple"
#include "vm/boc.h"


// TODO: use td/utils/json
// TODO: use tlb auto deserializer to json (PrettyPrintJson)
using json = nlohmann::json;
using td::Ref;


std::vector<std::tuple<int, std::string>> parse_extra_currency(const Ref<vm::Cell> &extra);

std::map<std::string, std::variant<int, std::string>> parse_anycast(vm::CellSlice anycast);

std::string dump_as_boc(Ref<vm::Cell> root_cell);

json parse_address(vm::CellSlice address);

json parse_libraries(Ref<vm::Cell> lib_cell);

json parse_state_init(vm::CellSlice state_init);

json parse_message(Ref<vm::Cell> message_any);

json parse_intermediate_address(vm::CellSlice intermediate_address);

json parse_msg_envelope(Ref<vm::Cell> message_envelope);

std::string parse_type(char type);

std::string parse_status_change(char type);

json parse_storage_used_short(vm::CellSlice storage_used_short);

json parse_storage_ph(vm::CellSlice item);

json parse_credit_ph(vm::CellSlice item);

json parse_action_ph(const vm::CellSlice &item);

json parse_bounce_phase(vm::CellSlice bp);

json parse_compute_ph(vm::CellSlice item);

json parse_split_prepare(vm::CellSlice item);

json parse_transaction_descr(const Ref<vm::Cell> &transaction_descr);

json parse_transaction(const Ref<vm::CellSlice> &tvalue, int workchain);

json parse_in_msg_descr(vm::CellSlice in_msg, int workchain);

json parse_out_msg_descr(vm::CellSlice out_msg, int workchain);

bool clear_cache();

#endif  //TON_JSON_UTILS_HPP
