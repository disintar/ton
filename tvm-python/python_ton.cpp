#include "third-party/pybind11/include/pybind11/pybind11.h"
#include "third-party/pybind11/include/pybind11/stl.h"
#include "tvm-python/PyCellSlice.h"
#include "tvm-python/PyCell.h"
#include "tvm-python/PyCellBuilder.h"
#include "tvm-python/PyDict.h"
#include "tvm-python/PyEmulator.h"
#include "crypto/tl/tlbc-data.h"

namespace py = pybind11;
using namespace pybind11::literals;  // to bring in the `_a` literal

void globalSetVerbosity(int vb) {
  int v = VERBOSITY_NAME(FATAL) + vb;
  SET_VERBOSITY_LEVEL(v);
}

std::string codeget_python_tlb(std::string tlb_code) {
  py::print(tlb_code);
  auto tlb = tlbc::codegen_python_tlb(tlb_code);
  py::print(tlb);
  return tlb;
}

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

  py::class_<PyCellSlice>(m, "PyCellSlice")
      .def(py::init<>())
      .def("load_uint", &PyCellSlice::load_uint, py::arg("bit_len"))
      .def("preload_uint", &PyCellSlice::preload_uint, py::arg("bit_len"))
      .def("dump", &PyCellSlice::dump)
      .def("fetch_ref", &PyCellSlice::fetch_ref)
      .def("prefetch_ref", &PyCellSlice::prefetch_ref, py::arg("offset"))
      .def("load_int", &PyCellSlice::load_int, py::arg("bit_len"))
      .def("preload_int", &PyCellSlice::preload_int, py::arg("bit_len"))
      .def("load_addr", &PyCellSlice::load_addr)
      .def("empty_ext", &PyCellSlice::empty_ext)
      .def("fetch_uint_less", &PyCellSlice::fetch_uint_less)
      .def("fetch_uint_leq", &PyCellSlice::fetch_uint_leq)
      .def("to_boc", &PyCellSlice::to_boc)
      .def("is_special", &PyCellSlice::is_special)
      .def("special_type", &PyCellSlice::special_type)
      .def("get_hash", &PyCellSlice::get_hash)
      .def("load_snake_string", &PyCellSlice::load_snake_string)
      .def("load_tlb", &PyCellSlice::load_tlb, py::arg("tlb_type"))
      .def("bselect", &PyCellSlice::bselect, py::arg("bits"), py::arg("mask"))
      .def("bselect_ext", &PyCellSlice::bselect_ext, py::arg("bits"), py::arg("mask"))
      .def("bit_at", &PyCellSlice::bit_at, py::arg("position"))
      .def("load_subslice", &PyCellSlice::load_subslice)
      .def("preload_subslice", &PyCellSlice::preload_subslice)
      //      .def("begins_with", &PyCellSlice::begins_with, py::arg("n"))
      //      .def("begins_with_bits", &PyCellSlice::begins_with_bits, py::arg("bits"), py::arg("n"))
      //      .def("begins_with_skip_bits", &PyCellSlice::begins_with_skip_bits, py::arg("bits"), py::arg("value"))
      //      .def("begins_with_skip", &PyCellSlice::begins_with_skip, py::arg("value"))
      .def("to_bitstring", &PyCellSlice::to_bitstring)
      .def("advance", &PyCellSlice::advance, py::arg("n"))
      .def("advance_ext", &PyCellSlice::advance_ext, py::arg("bits_refs"))
      .def("advance_bits_refs", &PyCellSlice::advance_bits_refs, py::arg("bits"), py::arg("refs"))
      .def("advance_refs", &PyCellSlice::advance_refs, py::arg("refs"))
      .def("advance_ext", &PyCellSlice::advance_ext, py::arg("n"))
      .def("skip_bits", &PyCellSlice::skip_bits, py::arg("bits"), py::arg("last"))
      .def("skip_refs", &PyCellSlice::skip_refs, py::arg("n"), py::arg("last"))
      .def("load_string", &PyCellSlice::load_string, py::arg("text_size") = 0, py::arg("convert_to_utf8") = true)
      .def("dump_as_tlb", &PyCellSlice::dump_as_tlb, py::arg("tlb_type"))
      .def("load_var_integer_str", &PyCellSlice::load_var_integer_str, py::arg("bit_len"), py::arg("sgnd"))
      .def("__repr__", &PyCellSlice::toString)
      .def_property("bits", &PyCellSlice::bits, &PyCellSlice::dummy_set)
      .def_property("refs", &PyCellSlice::refs, &PyCellSlice::dummy_set);

  py::class_<PyCell>(m, "PyCell")
      .def(py::init<>())
      .def("get_hash", &PyCell::get_hash)
      .def("dump", &PyCell::dump)
      .def("dump_as_tlb", &PyCell::dump_as_tlb, py::arg("tlb_type"))
      .def("to_boc", &PyCell::to_boc)
      .def("__repr__", &PyCell::toString)
      .def("copy", &PyCell::copy)
      .def("is_null", &PyCell::is_null);

  py::class_<PyCellBuilder>(m, "PyCellBuilder")
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
      .def("get_cell", &PyCellBuilder::get_cell)
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

  py::class_<PyDict>(m, "PyDict")
      .def(py::init<int, bool, std::optional<PyCellSlice>>(), py::arg("bit_len"), py::arg("signed") = false,
           py::arg("cs_root") = py::none())
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
      .def("__repr__", &PyDict::toString);

  m.def("parseStringToCell", parseStringToCell, py::arg("cell_boc"));
  m.def("globalSetVerbosity", globalSetVerbosity, py::arg("verbosity"));
  m.def("load_as_cell_slice", load_as_cell_slice, py::arg("cell"), py::arg("allow_special"));
  m.def("codegen_python_tlb", codeget_python_tlb, py::arg("tlb_text"));

  py::class_<PyEmulator>(m, "PyEmulator")
      .def(py::init<PyCell, int>(), py::arg("global_config_boc"), py::arg("vm_log_verbosity") = 0)
      .def("set_unixtime", &PyEmulator::set_unixtime, py::arg("unixtime"))
      .def("set_lt", &PyEmulator::set_lt, py::arg("lt"))
      .def("set_rand_seed", &PyEmulator::set_rand_seed, py::arg("rand_seed_hex"))
      .def("set_ignore_chksig", &PyEmulator::set_ignore_chksig, py::arg("ignore_chksig"))
      .def("set_config", &PyEmulator::set_config, py::arg("global_config_boc"))
      .def("set_libs", &PyEmulator::set_libs, py::arg("shardchain_libs_boc"))
      .def("set_debug_enabled", &PyEmulator::set_debug_enabled, py::arg("debug_enabled"))
      .def("emulate_transaction", &PyEmulator::emulate_transaction, py::arg("shard_account_cell"),
           py::arg("message_cell"), py::arg("unixtime") = "0", py::arg("lt") = "0", py::arg("vm_ver") = 1)
      .def("emulate_tick_tock_transaction", &PyEmulator::emulate_tick_tock_transaction, py::arg("shard_account_boc"),
           py::arg("is_tock"), py::arg("unixtime") = "0", py::arg("lt") = "0", py::arg("vm_ver") = 1)
      .def_property("vm_log", &PyEmulator::get_vm_log, &PyEmulator::dummy_set)
      .def_property("vm_exit_code", &PyEmulator::get_vm_exit_code, &PyEmulator::dummy_set)
      .def_property("elapsed_time", &PyEmulator::get_elapsed_time, &PyEmulator::dummy_set)
      .def_property("transaction_cell", &PyEmulator::get_transaction_cell, &PyEmulator::dummy_set)
      .def_property("account_cell", &PyEmulator::get_account_cell, &PyEmulator::dummy_set)
      .def_property("actions_cell", &PyEmulator::get_actions_cell, &PyEmulator::dummy_set);
}
