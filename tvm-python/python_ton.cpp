#include "third-party/pybind11/include/pybind11/pybind11.h"
#include "third-party/pybind11/include/pybind11/stl.h"
#include "tvm-python/PyCellSlice.h"
#include "tvm-python/PyCell.h"
#include "tvm-python/PyCellBuilder.h"
#include "tvm-python/PyDict.h"
#include "tvm-python/PyEmulator.h"

namespace py = pybind11;
using namespace pybind11::literals;  // to bring in the `_a` literal

void globalSetVerbosity(int vb){
  int v = VERBOSITY_NAME(FATAL) + vb;
  SET_VERBOSITY_LEVEL(v);
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
      PyErr_SetString(PyExc_RuntimeError, "Cell write overflow");
    } catch (vm::CellBuilder::CellCreateError) {
      PyErr_SetString(PyExc_RuntimeError, "Cell create overflow");
    } catch (vm::CellSlice::CellReadError) {
      PyErr_SetString(PyExc_RuntimeError, "Cell underflow");
    } catch (const vm::VmError& e) {
      exc(e.get_msg());
    } catch (const vm::VmFatal& e) {
      exc("VMFatal error");
    } catch (std::exception& e) {
      PyErr_SetString(PyExc_RuntimeError, e.what());
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
      .def("to_boc", &PyCellSlice::to_boc)
      .def("get_hash", &PyCellSlice::get_hash)
      .def("skip_bits", &PyCellSlice::skip_bits, py::arg("bits"), py::arg("last"))
      .def("skip_refs", &PyCellSlice::skip_refs, py::arg("n"), py::arg("last"))
      .def("dump_as_tlb", &PyCellSlice::dump_as_tlb, py::arg("tlb_type"))
      .def("load_var_integer_str", &PyCellSlice::load_var_integer_str, py::arg("bit_len"), py::arg("sgnd"))
      .def("__repr__", &PyCellSlice::toString)
      .def_property("bits", &PyCellSlice::bits, &PyCellSlice::dummy_set)
      .def_property("refs", &PyCellSlice::refs, &PyCellSlice::dummy_set);

  py::class_<PyCell>(m, "PyCell")
      .def(py::init<>())
      .def("get_hash", &PyCell::get_hash)
      .def("begin_parse", &PyCell::begin_parse)
      .def("dump", &PyCell::dump)
      .def("dump_as_tlb", &PyCell::dump_as_tlb, py::arg("tlb_type"))
      .def("to_boc", &PyCell::to_boc)
      .def("__repr__", &PyCell::toString)
      .def("is_null", &PyCell::is_null);

  py::class_<PyCellBuilder>(m, "PyCellBuilder")
      .def(py::init<>())
      .def("store_uint_str", &PyCellBuilder::store_uint_str, py::arg("str") = "", py::arg("bits") = "",
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
      .def("store_ref", &PyCellBuilder::store_ref)
      .def("dump", &PyCellBuilder::dump)
      .def("get_hash", &PyCellBuilder::get_hash)
      .def("dump_as_tlb", &PyCellBuilder::dump_as_tlb, py::arg("tlb_type"))
      .def("to_boc", &PyCellBuilder::to_boc)
      .def("__repr__", &PyCellBuilder::toString);

  py::class_<PyDict>(m, "PyDict")
      .def(py::init<int, bool>(), py::arg("bit_len"), py::arg("signed") = false)
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
