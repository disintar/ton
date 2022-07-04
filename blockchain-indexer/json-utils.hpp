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


inline std::list<std::tuple<int, std::string>> parse_extra_currency(const Ref<vm::Cell> &extra);

inline std::map<std::string, std::variant<int, std::string>> parse_anycast(vm::CellSlice anycast);

inline std::string dump_as_boc(Ref<vm::Cell> root_cell);

inline json parse_address(vm::CellSlice address);

inline json parse_libraries(Ref<vm::Cell> lib_cell);

inline json parse_state_init(vm::CellSlice state_init);

inline json parse_message(Ref<vm::Cell> message_any);

inline std::string parse_type(char type);

inline std::string parse_status_change(char type);

inline json parse_storage_used_short(vm::CellSlice storage_used_short);

inline json parse_storage_ph(vm::CellSlice item);

inline json parse_credit_ph(vm::CellSlice item);

inline json parse_action_ph(const vm::CellSlice &item);

inline json parse_bounce_phase(vm::CellSlice bp);

inline json parse_compute_ph(vm::CellSlice item);

inline json parse_split_prepare(vm::CellSlice item);

inline json parse_transaction_descr(const Ref<vm::Cell> &transaction_descr);

inline json parse_transaction(const Ref<vm::CellSlice> &tvalue, int workchain);

inline json parse_in_msg_descr(vm::CellSlice in_msg, int workchain);

inline json parse_out_msg_descr(vm::CellSlice out_msg, int workchain);

#endif  //TON_JSON_UTILS_HPP
