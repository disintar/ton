// Copyright 2023 Disintar LLP / andrey@head-labs.com

#include "third-party/pybind11/include/pybind11/pybind11.h"
#include <optional>
#include "third-party/pybind11/include/pybind11/stl.h"
#include "tvm-python/PyCellSlice.h"
#include "tvm-python/PyCell.h"
#include "tvm-python/PyCellBuilder.h"
#include "tvm-python/PyDict.h"
#include "tvm-python/PyEmulator.h"
#include "tvm-python/PyTools.h"
#include "tvm-python/PyTVM.h"
#include "tvm-python/PyFift.h"
#include "tvm-python/PyStack.h"
#include "tvm-python/PySmcAddress.h"
//#include "tvm-python/PyKeys.h"
#include "crypto/tl/tlbc-data.h"
#include "td/utils/optional.h"

namespace py = pybind11;
using namespace pybind11::literals;  // to bring in the `_a` literal

void globalSetVerbosity(int vb) {
  int v = VERBOSITY_NAME(FATAL) + vb;
  SET_VERBOSITY_LEVEL(v);
}

std::string test() {
  std::ostringstream os;
  os << "SETCP " << 1;
  std::cerr << os.str() << std::endl;
  return os.str();
}

unsigned method_name_to_id(const std::string& method_name) {
  unsigned crc = td::crc16(method_name);
  const unsigned method_id = (crc & 0xffff) | 0x10000;
  return method_id;
}

std::string codeget_python_tlb(std::string tlb_code) {
  return tlbc::codegen_python_tlb(tlb_code);
}

template <typename T>
struct py::detail::type_caster<td::optional<T>> {
 public:
  PYBIND11_TYPE_CASTER(td::optional<T>, _("td::optional"));

  bool load(handle src, bool) {
    if (!src || src.is(py::none())) {
      value = td::optional<T>();
      return true;
    }

    value = td::optional<T>(py::cast<T>(src));
    return true;
  }

  static handle cast(const td::optional<T>& src, return_value_policy, handle) {
    if (src) {
      return py::cast(src.value());
    } else {
      return py::none();
    }
  }
};

PYBIND11_MODULE(python_ton, m) {
  PSLICE() << "";
  SET_VERBOSITY_LEVEL(verbosity_ERROR);
  static py::exception<vm::VmError> exc(m, "VmError");
  py::register_exception_translator([](std::exception_ptr p) {
    try {
      if (p)
        std::rethrow_exception(p);
    } catch (vm::CellBuilder::CellWriteError) {
      throw std::runtime_error("CellWriteError");
    } catch (vm::CellBuilder::CellCreateError) {
      throw std::runtime_error("CellCreateError");
    } catch (vm::CellSlice::CellReadError) {
      throw std::runtime_error("CellReadError");
    } catch (const vm::VmError& e) {
      throw std::runtime_error(e.get_msg());
    } catch (const vm::VmFatal& e) {
      throw std::runtime_error("VmFatal error");
    } catch (src::ParseError e) {
      throw std::runtime_error(e.message);
    } catch (std::exception& e) {
      throw std::runtime_error(e.what());
    } catch (...) {
      throw std::runtime_error("Unknown error");
    }
  });
  py::class_<PyCellSlice>(m, "PyCellSlice", py::module_local())
      .def(py::init<>())
      .def("load_uint", &PyCellSlice::load_uint, py::arg("bit_len"))
      .def("preload_uint", &PyCellSlice::preload_uint, py::arg("bit_len"))
      .def("dump", &PyCellSlice::dump)
      .def("fetch_ref", &PyCellSlice::fetch_ref)
      .def("prefetch_ref", &PyCellSlice::prefetch_ref, py::arg("offset"))
      .def("load_int", &PyCellSlice::load_int, py::arg("bit_len"))
      .def("preload_int", &PyCellSlice::preload_int, py::arg("bit_len"))
      .def("load_addr", &PyCellSlice::load_addr)
      .def("copy", &PyCellSlice::copy)
      .def("cut_tail", &PyCellSlice::cut_tail, py::arg("cs"))
      .def("empty_ext", &PyCellSlice::empty_ext)
      .def("fetch_uint_less", &PyCellSlice::fetch_uint_less)
      .def("fetch_uint_leq", &PyCellSlice::fetch_uint_leq)
      .def("to_boc", &PyCellSlice::to_boc)
      .def("is_special", &PyCellSlice::is_special)
      .def("special_type", &PyCellSlice::special_type)
      .def("get_hash", &PyCellSlice::get_hash)
      .def("size_ext", &PyCellSlice::size_ext)
      .def("load_snake_string", &PyCellSlice::load_snake_string)
      .def("load_tlb", &PyCellSlice::load_tlb, py::arg("tlb_type"))
      .def("bselect", &PyCellSlice::bselect, py::arg("bits"), py::arg("mask"))
      .def("bselect_ext", &PyCellSlice::bselect_ext, py::arg("bits"), py::arg("mask"))
      .def("bit_at", &PyCellSlice::bit_at, py::arg("position"))
      .def("load_subslice", &PyCellSlice::load_subslice)
      .def("load_subslice_ext", &PyCellSlice::load_subslice_ext)
      .def("preload_subslice", &PyCellSlice::preload_subslice)
      .def("preload_subslice_ext", &PyCellSlice::preload_subslice_ext)
      //      .def("begins_with", &PyCellSlice::begins_with, py::arg("n"))
      //      .def("begins_with_bits", &PyCellSlice::begins_with_bits, py::arg("bits"), py::arg("n"))
      //      .def("begins_with_skip_bits", &PyCellSlice::begins_with_skip_bits, py::arg("bits"), py::arg("value"))
      //      .def("begins_with_skip", &PyCellSlice::begins_with_skip, py::arg("value"))
      .def("to_bitstring", &PyCellSlice::to_bitstring)
      .def("advance", &PyCellSlice::advance, py::arg("n"))
      .def("advance_ext", &PyCellSlice::advance_ext, py::arg("bits_refs"))
      .def("advance_bits_refs", &PyCellSlice::advance_bits_refs, py::arg("bits"), py::arg("refs"))
      .def("advance_refs", &PyCellSlice::advance_refs, py::arg("refs"))
      .def("skip_bits", &PyCellSlice::skip_bits, py::arg("bits"), py::arg("last"))
      .def("skip_refs", &PyCellSlice::skip_refs, py::arg("n"), py::arg("last"))
      .def("load_string", &PyCellSlice::load_string, py::arg("text_size") = 0, py::arg("convert_to_utf8") = true)
      .def("dump_as_tlb", &PyCellSlice::dump_as_tlb, py::arg("tlb_type"))
      .def("load_var_integer_str", &PyCellSlice::load_var_integer_str, py::arg("bit_len"), py::arg("sgnd"))
      .def("__repr__", &PyCellSlice::toString)
      .def_property("bits", &PyCellSlice::bits, &PyCellSlice::dummy_set)
      .def_property("refs", &PyCellSlice::refs, &PyCellSlice::dummy_set);

  py::class_<PyCell>(m, "PyCell", py::module_local())
      .def(py::init<>())
      .def("get_hash", &PyCell::get_hash)
      .def("dump", &PyCell::dump)
      .def("dump_as_tlb", &PyCell::dump_as_tlb, py::arg("tlb_type"))
      .def("to_boc", &PyCell::to_boc)
      .def("__repr__", &PyCell::toString)
      .def("copy", &PyCell::copy)
      .def("is_null", &PyCell::is_null);

  py::class_<PyCellBuilder>(m, "PyCellBuilder", py::module_local())
      .def(py::init<>())
      .def("store_uint_str", &PyCellBuilder::store_uint_str, py::arg("str") = "", py::arg("bits") = "",
           py::return_value_policy::reference_internal)
      .def("store_256uint_str", &PyCellBuilder::store_uint_str, py::arg("str") = "", py::arg("bits") = "",
           py::return_value_policy::reference_internal)
      .def("store_int_str", &PyCellBuilder::store_int_str, py::arg("str") = "", py::arg("bits") = "",
           py::return_value_policy::reference_internal)
      .def("store_builder", &PyCellBuilder::store_builder, py::arg("cb"), py::return_value_policy::reference_internal)
      .def("store_zeroes", &PyCellBuilder::store_zeroes, py::arg("bits"), py::return_value_policy::reference_internal)
      .def("store_ones", &PyCellBuilder::store_ones, py::arg("bits"), py::return_value_policy::reference_internal)
      .def("store_address", &PyCellBuilder::store_address, py::arg("addr"), py::return_value_policy::reference_internal)
      .def("store_bitstring", &PyCellBuilder::store_bitstring, py::arg("bs"),
           py::return_value_policy::reference_internal)
      .def("store_slice", &PyCellBuilder::store_slice, py::arg("cs"), py::return_value_policy::reference_internal)
      .def("store_grams_str", &PyCellBuilder::store_grams_str, py::arg("grams"),
           py::return_value_policy::reference_internal)
      .def("store_var_integer", &PyCellBuilder::store_var_integer, py::arg("int"), py::arg("bit_len"), py::arg("sgnd"),
           py::return_value_policy::reference_internal)
      .def("get_cell", &PyCellBuilder::get_cell, py::arg("special"))
      .def("store_ref", &PyCellBuilder::store_ref, py::return_value_policy::reference_internal)
      .def("dump", &PyCellBuilder::dump)
      .def("get_hash", &PyCellBuilder::get_hash)
      .def("dump_as_tlb", &PyCellBuilder::dump_as_tlb, py::arg("tlb_type"))
      .def("to_boc", &PyCellBuilder::to_boc)
      .def("store_uint_less", &PyCellBuilder::store_uint_less, py::arg("upper_bound"), py::arg("value"),
           py::return_value_policy::reference_internal)
      .def("store_uint_leq", &PyCellBuilder::store_uint_leq, py::arg("upper_bound"), py::arg("value"),
           py::return_value_policy::reference_internal)
      .def("__repr__", &PyCellBuilder::toString)
      .def_property("bits", &PyCellBuilder::get_bits, &PyCellBuilder::dummy_set)
      .def_property("refs", &PyCellBuilder::get_refs, &PyCellBuilder::dummy_set)
      .def_property("remaining_refs", &PyCellBuilder::get_remaining_refs, &PyCellBuilder::dummy_set)
      .def_property("remaining_bits", &PyCellBuilder::get_remaining_bits, &PyCellBuilder::dummy_set);

  py::class_<PyDict>(m, "PyDict", py::module_local())
      .def(py::init<int, bool, td::optional<PyCellSlice>>(), py::arg("bit_len"), py::arg("signed") = false,
           py::arg("cs_root") = py::none())
      .def(py::init<int, PyAugmentationCheckData, bool, td::optional<PyCellSlice>>(), py::arg("bit_len"),
           py::arg("aug"), py::arg("signed") = false, py::arg("cs_root") = py::none())
      .def("get_pycell", &PyDict::get_pycell)
      .def("is_empty", &PyDict::is_empty)
      .def("set_str", &PyDict::set, py::arg("key"), py::arg("value"), py::arg("mode") = "set", py::arg("key_len") = 0,
           py::arg("signed") = false)
      .def("set_ref_str", &PyDict::set_ref, py::arg("key"), py::arg("value"), py::arg("mode") = "set",
           py::arg("key_len") = 0, py::arg("signed") = false)
      .def("set_builder_str", &PyDict::set_builder, py::arg("key"), py::arg("value"), py::arg("mode") = "set",
           py::arg("key_len") = 0, py::arg("signed") = false)
      .def("lookup_str", &PyDict::lookup, py::arg("key"), py::arg("key_len") = 0, py::arg("signed") = false)
      .def("lookup_nearest_key", &PyDict::lookup_nearest_key, py::arg("key"), py::arg("fetch_next") = true,
           py::arg("allow_eq") = false, py::arg("inver_first") = false, py::arg("key_len") = 0,
           py::arg("signed") = false)
      .def("get_minmax_key", &PyDict::get_minmax_key, py::arg("fetch_max") = false, py::arg("inver_first") = false,
           py::arg("key_len") = 0, py::arg("signed") = false)
      .def("get_minmax_key_ref", &PyDict::get_minmax_key_ref, py::arg("fetch_max") = false,
           py::arg("inver_first") = false, py::arg("key_len") = 0, py::arg("signed") = false)
      .def("lookup_delete_str", &PyDict::lookup_delete, py::arg("key"), py::arg("key_len") = 0,
           py::arg("signed") = false)
      .def("lookup_ref_str", &PyDict::lookup_ref, py::arg("key"), py::arg("key_len") = 0, py::arg("signed") = false)
      .def("lookup_delete_ref_str", &PyDict::lookup_delete_ref, py::arg("key"), py::arg("key_len") = 0,
           py::arg("signed") = false)
      .def("to_boc", &PyDict::to_boc)
      .def("map", &PyDict::map)
      .def("__repr__", &PyDict::toString);

  m.def("parse_string_to_cell", parse_string_to_cell, py::arg("cell_boc"));
  m.def("globalSetVerbosity", globalSetVerbosity, py::arg("verbosity"));
  m.def("load_as_cell_slice", load_as_cell_slice, py::arg("cell"), py::arg("allow_special"));
  m.def("codegen_python_tlb", codeget_python_tlb, py::arg("tlb_text"));
  m.def("parse_token_data", parse_token_data, py::arg("cell"));
  m.def("method_name_to_id", method_name_to_id, py::arg("cell"));
  m.def("pack_address", pack_address, py::arg("address"));

  m.def("code_dissemble_str", code_dissemble_str, py::arg("code_boc"), py::arg("base_path"));
  m.def("code_dissemble_cell", code_dissemble_cell, py::arg("code_cell"), py::arg("base_path"));
  m.def("make_tuple", make_tuple, py::arg("items"));
  m.def("deserialize_stack_entry", deserialize_stack_entry, py::arg("cell_slice"));
  m.def("deserialize_stack", deserialize_stack, py::arg("cell_slice"));

  py::class_<PyStackInfo>(m, "PyStackInfo", py::module_local())
      .def_readwrite("stack", &PyStackInfo::stack)
      .def_readwrite("gas_consumed", &PyStackInfo::gas_consumed)
      .def_readwrite("gas_remaining", &PyStackInfo::gas_remaining);

  py::class_<PyTVM>(m, "PyTVM", py::module_local())
      .def(py::init<int, td::optional<PyCell>, td::optional<PyCell>, bool, bool, bool, bool>(),
           py::arg("log_level_") = 0, py::arg("code_") = td::optional<PyCell>(),
           py::arg("data_") = td::optional<PyCell>(), py::arg("allowDebug_") = false, py::arg("sameC3_") = true,
           py::arg("skip_c7_") = false, py::arg("enable_vm_dump") = false)
      .def_property("code", &PyTVM::get_code, &PyTVM::set_code)
      .def_property("data", &PyTVM::set_data, &PyTVM::get_data)
      .def("set_stack", &PyTVM::set_stack)
      .def("set_libs", &PyTVM::set_libs)
      .def("get_ops", &PyTVM::get_ops)
      .def("set_state_init", &PyTVM::set_state_init)
      .def("clear_stack", &PyTVM::clear_stack)
      .def("set_gasLimit", &PyTVM::set_gasLimit, py::arg("gas_limit") = "0", py::arg("gas_max") = "-1")
      .def("run_vm", &PyTVM::run_vm)
      .def("get_stacks", &PyTVM::get_stacks)
      .def("set_c7", &PyTVM::set_c7, py::arg("stack"))
      .def_property("exit_code", &PyTVM::get_exit_code, &PyTVM::dummy_set)
      .def_property("vm_steps", &PyTVM::get_vm_steps, &PyTVM::dummy_set)
      .def_property("gas_used", &PyTVM::get_gas_used, &PyTVM::dummy_set)
      .def_property("gas_credit", &PyTVM::get_gas_credit, &PyTVM::dummy_set)
      .def_property("success", &PyTVM::get_success, &PyTVM::dummy_set)
      .def_property("vm_final_state_hash", &PyTVM::get_vm_final_state_hash, &PyTVM::dummy_set)
      .def_property("vm_init_state_hash", &PyTVM::get_vm_init_state_hash, &PyTVM::dummy_set)
      .def_property("new_data", &PyTVM::get_new_data, &PyTVM::dummy_set)
      .def_property("actions", &PyTVM::get_actions, &PyTVM::dummy_set);

  py::class_<PyContinuation>(m, "PyContinuation", py::module_local())
      .def(py::init<PyCellSlice>(), py::arg("cell_slice"))
      .def("serialize", &PyContinuation::serialize)
      .def("type", &PyContinuation::type);

  py::class_<PyFift>(m, "PyFift", py::module_local())
      .def(py::init<std::string, bool>(), py::arg("base_path"), py::arg("silent"))
      .def("run", &PyFift::run, py::arg("code_text"))
      .def("add_lib", &PyFift::add_lib, py::arg("lib"))
      .def("get_stack", &PyFift::get_stack)
      .def("clear_libs", &PyFift::clear_libs);

  py::class_<PyStack>(m, "PyStack", py::module_local())
      .def(py::init<>())
      .def("at", &PyStack::at)
      .def("pop", &PyStack::pop)
      .def("push", &PyStack::push)
      .def("is_empty", &PyStack::is_empty)
      .def("depth", &PyStack::depth)
      .def("serialize", &PyStack::serialize, py::arg("mode"));

  py::class_<PyStackEntry>(m, "PyStackEntry", py::module_local())
      .def(py::init<td::optional<PyCell>, td::optional<PyCellSlice>, td::optional<PyCellSlice>,
                    td::optional<PyContinuation>, std::string>(),
           py::arg("cell") = td::optional<PyCell>(), py::arg("cell_slice") = td::optional<PyCellSlice>(),
           py::arg("cell_builder") = td::optional<PyCellSlice>(),
           py::arg("continuation") = td::optional<PyContinuation>(), py::arg("big_int") = "")
      .def("as_int", &PyStackEntry::as_int)
      .def("as_string", &PyStackEntry::as_string)
      .def("as_cell_slice", &PyStackEntry::as_cell_slice)
      .def("as_cell_builder", &PyStackEntry::as_cell_builder)
      .def("as_cont", &PyStackEntry::as_cont)
      .def("as_cell", &PyStackEntry::as_cell)
      .def("as_tuple", &PyStackEntry::as_tuple)
      .def("serialize", &PyStackEntry::serialize, py::arg("mode"))
      .def("type", &PyStackEntry::type);

  py::class_<PyEmulator>(m, "PyEmulator", py::module_local())
      .def(py::init<PyCell>(), py::arg("global_config_boc"))
      .def("set_rand_seed", &PyEmulator::set_rand_seed, py::arg("rand_seed_hex"))
      .def("set_ignore_chksig", &PyEmulator::set_ignore_chksig, py::arg("ignore_chksig"))
      .def("set_libs", &PyEmulator::set_libs, py::arg("shardchain_libs_boc"))
      .def("set_debug_enabled", &PyEmulator::set_debug_enabled, py::arg("debug_enabled"))
      .def("emulate_transaction", &PyEmulator::emulate_transaction, py::arg("shard_account_cell"),
           py::arg("message_cell"), py::arg("unixtime") = "0", py::arg("lt") = "0", py::arg("vm_ver") = 1)
      .def("emulate_tick_tock_transaction", &PyEmulator::emulate_tick_tock_transaction, py::arg("shard_account_boc"),
           py::arg("is_tock"), py::arg("unixtime") = "0", py::arg("lt") = "0", py::arg("vm_ver") = 1)
      .def_property("vm_log", &PyEmulator::get_vm_log, &PyEmulator::dummy_set)
      .def_property("elapsed_time", &PyEmulator::get_elapsed_time, &PyEmulator::dummy_set)
      .def_property("transaction_cell", &PyEmulator::get_transaction_cell, &PyEmulator::dummy_set)
      .def_property("account_cell", &PyEmulator::get_account_cell, &PyEmulator::dummy_set)
      .def_property("actions_cell", &PyEmulator::get_actions_cell, &PyEmulator::dummy_set);

  py::class_<PyAugmentationCheckData>(m, "PyAugmentationCheckData", py::module_local())
      .def(py::init<py::function&, py::function&, py::function&, py::function&>(), py::arg("py_eval_leaf"),
           py::arg("py_skip_extra"), py::arg("py_eval_fork"), py::arg("py_eval_empty"));

  py::class_<PySmcAddress>(m, "PySmcAddress", py::module_local())
      .def(py::init<>())
      .def_property("wc", &PySmcAddress::wc, &PySmcAddress::set_workchain)
      .def_property("bounceable", &PySmcAddress::bounceable, &PySmcAddress::set_bounceable)
      .def_property("testnet", &PySmcAddress::testnet, &PySmcAddress::set_testnet)
      .def("rserialize", &PySmcAddress::rserialize, py::arg("base64_url"))
      .def("append_to_builder", &PySmcAddress::append_to_builder, py::arg("builder"))
      .def("pack", &PySmcAddress::pack)
      .def("address", &PySmcAddress::address);
  m.def("address_from_string", address_from_string, py::arg("address"));
  m.def("address_from_cell_slice", address_from_cell_slice, py::arg("cell_slice"));
  m.def("text_test", test);
}
