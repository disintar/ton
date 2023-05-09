#include "third-party/pybind11/include/pybind11/pybind11.h"
#include <string>
#include <cassert>
#include <codecvt>
#include <iostream>
#include <locale>
#include <sstream>
#include <string>

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
#include "ton/ton-types.h"
#include "crypto/vm/cellslice.h"
#include "td/actor/actor.h"
#include "tonlib/tonlib/TonlibClientWrapper.h"
#include <queue>
#include "keyring/keyring.h"

namespace py = pybind11;
using namespace pybind11::literals;  // to bring in the `_a` literal

struct PyCellSlice {
  vm::CellSlice my_cell_slice;

  // constructor
  explicit PyCellSlice(const vm::Ref<vm::Cell>& cell_slice) {
    my_cell_slice = vm::load_cell_slice(cell_slice);
  }

  explicit PyCellSlice() = default;

  std::string load_uint(unsigned n) {
    const auto tmp = my_cell_slice.fetch_int256(n, false);
    return tmp->to_dec_string();
  }

  std::string preload_uint(unsigned n) {
    const auto tmp = my_cell_slice.fetch_int256(n, false);
    return tmp->to_dec_string();
  }

  std::string load_int(unsigned n) {
    const auto tmp = my_cell_slice.fetch_int256(n, true);
    return tmp->to_dec_string();
  }

  std::string preload_int(unsigned n) {
    const auto tmp = my_cell_slice.prefetch_int256(n, true);
    return tmp->to_dec_string();
  }

  std::string toString() const {
    std::stringstream os;
    my_cell_slice.dump(os);

    auto t = os.str();
    t.pop_back();

    return "<CellSlice " + t + ">";
  }

  std::string load_addr() {
    ton::StdSmcAddress addr;
    ton::WorkchainId workchain;
    if (!block::tlb::t_MsgAddressInt.extract_std_address(my_cell_slice, workchain, addr)) {
      throw std::invalid_argument("Parse address error: not valid address");
    }
    auto friendlyAddr = block::StdAddress(workchain, addr);

    return friendlyAddr.rserialize(true);
  }

  PyCellSlice fetch_ref() {
    const auto r = my_cell_slice.fetch_ref();
    if (r.is_null()) {
      throw vm::CellBuilder::CellWriteError();
    }
    return PyCellSlice(r);
  }

  PyCellSlice prefetch_ref(int offset = 0) {
    const auto r = my_cell_slice.prefetch_ref(offset);
    if (r.is_null()) {
      throw vm::CellBuilder::CellWriteError();
    }

    return PyCellSlice(r);
  }

  std::string dump() const {
    std::stringstream os;
    my_cell_slice.print_rec(os);

    return os.str();
  }

  std::string load_var_integer_str(unsigned int varu, bool sgnd) {
    td::RefInt256 x;

    const unsigned int len_bits = 32 - td::count_leading_zeroes32(varu - 1);
    int len;
    if (!(my_cell_slice.fetch_uint_to(len_bits, len) && my_cell_slice.fetch_int256_to(len * 8, x, sgnd))) {
      throw vm::VmError{vm::Excno::cell_und, "cannot deserialize a variable-length integer"};
    }

    return x->to_dec_string();
  }

  std::string to_boc() const {
    vm::CellBuilder cb;
    cb.append_cellslice(my_cell_slice.clone());

    return td::base64_encode(std_boc_serialize(cb.finalize(), 31).move_as_ok());
  }
};

struct PyCell {
  vm::Ref<vm::Cell> my_cell;

  // constructor
  explicit PyCell(const vm::Ref<vm::Cell>& cell) {
    my_cell = cell;
  }

  explicit PyCell() = default;
  ~PyCell() = default;

  PyCellSlice begin_parse() const {
    return PyCellSlice(my_cell);
  }

  std::string get_hash() const {
    return my_cell->get_hash().to_hex();
  }

  std::string toString() const {
    std::stringstream os;
    vm::load_cell_slice(my_cell).dump(os);

    auto t = os.str();
    t.pop_back();

    return "<Cell " + t + ">";
  }

  std::string dump() const {
    std::stringstream os;
    vm::load_cell_slice(my_cell).print_rec(os);

    return os.str();
  }

  std::string to_boc() const {
    return td::base64_encode(std_boc_serialize(my_cell, 31).move_as_ok());
  }
};

struct PyCellBuilder {
  vm::CellBuilder my_builder;
  ~PyCellBuilder() = default;

  // constructor
  explicit PyCellBuilder() {
    my_builder = vm::CellBuilder();
  }

  PyCellBuilder* store_uint_str(const std::string& str, unsigned int bits) {
    td::BigInt256 x;
    x.enforce(x.parse_dec(str));

    my_builder.store_int256(x, bits, false);

    return this;
  }

  PyCellBuilder* store_int_str(const std::string& str, unsigned int bits) {
    td::BigInt256 x;
    x.enforce(x.parse_dec(str));

    my_builder.store_int256(x, bits);

    return this;
  }

  PyCellBuilder* store_bitstring(const std::string& s) {
    unsigned char buff[128];
    const auto tmp = td::Slice{s};
    int bits = (int)td::bitstring::parse_bitstring_binary_literal(buff, sizeof(buff), tmp.begin(), tmp.end());
    auto cs = td::Ref<vm::CellSlice>{true, vm::CellBuilder().store_bits(td::ConstBitPtr{buff}, bits).finalize()};
    my_builder = my_builder.append_cellslice(cs);

    return this;
  }

  PyCellBuilder* store_slice(const PyCellSlice& cs) {
    my_builder.append_cellslice(cs.my_cell_slice);
    return this;
  }

  PyCellBuilder* store_grams_str(const std::string& str) {
    td::BigInt256 x;
    x.enforce(x.parse_dec(str));

    int k = x.bit_size(false);
    const auto success = k <= 15 * 8 && my_builder.store_long_bool((k + 7) >> 3, 4) &&
                         my_builder.store_int256_bool(x, (k + 7) & -8, false);

    if (!success) {
      throw vm::CellBuilder::CellWriteError();
    }

    return this;
  }

  PyCellBuilder* store_var_integer(const std::string& str, unsigned int varu, bool sgnd) {
    td::BigInt256 x;
    x.enforce(x.parse_dec(str));

    const unsigned int len_bits = 32 - td::count_leading_zeroes32(varu - 1);
    unsigned len = (((unsigned)x.bit_size(sgnd) + 7) >> 3);

    if (len >= (1u << len_bits)) {
      throw vm::VmError{vm::Excno::range_chk};
    }

    if (!(my_builder.store_long_bool(len, len_bits) && my_builder.store_int256_bool(x, len * 8, sgnd))) {
      throw vm::VmError{vm::Excno::cell_ov, "cannot serialize a variable-length integer"};
    }

    return this;
  }

  PyCellBuilder* store_ref(const PyCell& c) {
    my_builder.store_ref(c.my_cell);
    return this;
  }

  PyCellBuilder* store_zeroes(unsigned int bits) {
    my_builder.store_zeroes(bits);
    return this;
  }

  PyCellBuilder* store_ones(unsigned int bits) {
    my_builder.store_ones(bits);
    return this;
  }

  PyCellBuilder* store_builder(const PyCellBuilder& cb) {
    if (!my_builder.can_extend_by(cb.my_builder.size(), cb.my_builder.size_refs())) {
      throw vm::CellBuilder::CellWriteError();
    }

    my_builder.append_builder(std::move(cb.my_builder));
    return this;
  }

  PyCellBuilder* store_address(const std::string& addr) {
    static block::StdAddress res = block::StdAddress::parse(addr).move_as_ok();

    block::tlb::t_MsgAddressInt.store_std_address(my_builder, res);
    return this;
  }

  PyCell get_cell() {
    return PyCell(my_builder.finalize());
  }

  std::string toString() const {
    std::stringstream os;
    vm::CellSlice tmp(my_builder.finalize_copy());
    tmp.dump(os);
    auto t = os.str();
    t.pop_back();

    return "<CellBuilder " + t + ">";
  }

  std::string dump() const {
    std::stringstream os;
    vm::CellSlice tmp(my_builder.finalize_copy());
    tmp.print_rec(os);

    return os.str();
  }

  std::string to_boc() const {
    return td::base64_encode(std_boc_serialize(my_builder.finalize_copy(), 31).move_as_ok());
  }
};

auto get_mode(const std::string& s) {
  if (s == "add") {
    return vm::Dictionary::SetMode::Add;
  } else if (s == "replace") {
    return vm::Dictionary::SetMode::Replace;
  } else {
    return vm::Dictionary::SetMode::Set;
  }
}

struct PyDict {
  std::unique_ptr<vm::Dictionary> my_dict;
  unsigned int key_len;
  bool sgnd;

  explicit PyDict(int key_len_, bool sgnd_ = false) {
    vm::Dictionary my_dict_t{key_len_};
    key_len = key_len_;
    sgnd = sgnd_;
    my_dict = std::make_unique<vm::Dictionary>(my_dict_t);
  }

  PyCell get_pycell() const {
    const auto d = *my_dict;
    td::Ref<vm::Cell> root = d.get_root_cell();
    return PyCell(root);
  }

  //  bool set(td::ConstBitPtr key, int key_len, Ref<CellSlice> value, SetMode mode = SetMode::Set);
  PyDict* set(const std::string& key, PyCellSlice& value, const std::string& mode, int key_len_ = 0, int sgnd_ = -1) {
    if (sgnd_ == -1) {
      sgnd_ = sgnd;
    }

    if (key_len_ == 0) {
      key_len_ = key_len;
    }

    td::BigInt256 x;
    x.enforce(x.parse_dec(key));
    unsigned char buffer[key_len_];
    x.export_bits(td::BitPtr{buffer}, key_len_, sgnd_);

    const auto mdict = my_dict.get();

    mdict->set(td::BitPtr{buffer}, key_len_, td::make_ref<vm::CellSlice>(value.my_cell_slice.clone()), get_mode(mode));
    return this;
  }

  //  bool Dictionary::set_ref(td::ConstBitPtr key, int key_len, Ref<Cell> val_ref, SetMode mode)
  PyDict* set_ref(const std::string& key, PyCell& value, const std::string& mode, int key_len_ = 0, int sgnd_ = -1) {
    if (sgnd_ == -1) {
      sgnd_ = sgnd;
    }

    if (key_len_ == 0) {
      key_len_ = key_len;
    }

    td::BigInt256 x;
    x.enforce(x.parse_dec(key));
    unsigned char buffer[key_len_];
    x.export_bits(td::BitPtr{buffer}, key_len_, sgnd_);

    const auto mdict = my_dict.get();

    mdict->set_ref(td::BitPtr{buffer}, key_len_, value.my_cell, get_mode(mode));
    return this;
  }

  // bool Dictionary::set_builder(td::ConstBitPtr key, int key_len, Ref<CellBuilder> val_b, SetMode mode) {
  PyDict* set_builder(const std::string& key, PyCellBuilder& value, const std::string& mode, int key_len_ = 0,
                      int sgnd_ = -1) {
    if (sgnd_ == -1) {
      sgnd_ = sgnd;
    }

    if (key_len_ == 0) {
      key_len_ = key_len;
    }

    td::BigInt256 x;
    x.enforce(x.parse_dec(key));
    unsigned char buffer[key_len_];
    x.export_bits(td::BitPtr{buffer}, key_len_, sgnd_);

    const auto mdict = my_dict.get();

    mdict->set_builder(td::BitPtr{buffer}, key_len_, value.my_builder, get_mode(mode));
    return this;
  }

  PyCellSlice lookup(const std::string& key, int key_len_ = 0, int sgnd_ = -1) const {
    if (sgnd_ == -1) {
      sgnd_ = sgnd;
    }

    if (key_len_ == 0) {
      key_len_ = key_len;
    }

    td::BigInt256 x;
    x.enforce(x.parse_dec(key));
    unsigned char buffer[key_len_];
    x.export_bits(td::BitPtr{buffer}, key_len_, sgnd_);
    const auto mdict = my_dict.get();

    auto cs = mdict->lookup(td::BitPtr{buffer}, key_len_);
    vm::CellBuilder cb;
    cb.append_cellslice(cs);

    return PyCellSlice(cb.finalize());
  }

  PyCellSlice lookup_delete(const std::string& key, int key_len_ = 0, int sgnd_ = -1) const {
    if (sgnd_ == -1) {
      sgnd_ = sgnd;
    }

    if (key_len_ == 0) {
      key_len_ = key_len;
    }

    td::BigInt256 x;
    x.enforce(x.parse_dec(key));
    unsigned char buffer[key_len_];
    x.export_bits(td::BitPtr{buffer}, key_len_, sgnd_);
    const auto mdict = my_dict.get();

    auto cs = mdict->lookup_delete(td::BitPtr{buffer}, key_len_);
    vm::CellBuilder cb;
    cb.append_cellslice(cs);

    return PyCellSlice(cb.finalize());
  }

  std::tuple<std::string, PyCellSlice> get_minmax_key(bool fetch_max = false, bool inver_first = false,
                                                      int key_len_ = 0, int sgnd_ = -1) const {
    if (sgnd_ == -1) {
      sgnd_ = sgnd;
    }

    if (key_len_ == 0) {
      key_len_ = key_len;
    }
    unsigned char buffer[key_len_];
    const auto mdict = my_dict.get();
    const auto cs = mdict->get_minmax_key(td::BitPtr{buffer}, key_len_, fetch_max, inver_first);

    vm::CellBuilder cb;
    cb.append_cellslice(cs);

    td::BigInt256 x;
    x.import_bits(td::ConstBitPtr{buffer}, key_len_, sgnd_);

    return std::make_tuple(x.to_dec_string(), PyCellSlice(cb.finalize()));
  }

  std::tuple<std::string, PyCell> get_minmax_key_ref(bool fetch_max = false, bool inver_first = false, int key_len_ = 0,
                                                     int sgnd_ = -1) const {
    if (sgnd_ == -1) {
      sgnd_ = sgnd;
    }

    if (key_len_ == 0) {
      key_len_ = key_len;
    }
    unsigned char buffer[key_len_];
    const auto mdict = my_dict.get();
    const auto cell = mdict->get_minmax_key_ref(td::BitPtr{buffer}, key_len_, fetch_max, inver_first);

    if (cell.is_null()) {
      throw vm::CellBuilder::CellCreateError();
    }

    td::BigInt256 x;
    x.import_bits(td::ConstBitPtr{buffer}, key_len_, sgnd_);

    return std::make_tuple(x.to_dec_string(), PyCell(cell));
  }

  std::tuple<std::string, PyCellSlice> lookup_nearest_key(const std::string& key, bool fetch_next = true,
                                                          bool allow_eq = false, bool inver_first = false,
                                                          int key_len_ = 0, int sgnd_ = -1) const {
    if (sgnd_ == -1) {
      sgnd_ = sgnd;
    }

    if (key_len_ == 0) {
      key_len_ = key_len;
    }

    td::BigInt256 x;
    x.enforce(x.parse_dec(key));

    unsigned char buffer[key_len_];
    x.export_bits(td::BitPtr{buffer}, key_len_, sgnd_);

    const auto mdict = my_dict.get();
    const auto cs = mdict->lookup_nearest_key(td::BitPtr{buffer}, key_len_, fetch_next, allow_eq, inver_first);

    td::BigInt256 x2;
    x2.import_bits(td::ConstBitPtr{buffer}, key_len_, sgnd_);

    vm::CellBuilder cb;
    cb.append_cellslice(cs);

    return std::make_tuple(x2.to_dec_string(), PyCellSlice(cb.finalize()));
  }

  bool is_empty() {
    return my_dict->is_empty();
  }

  PyCell lookup_ref(const std::string& key, int key_len_ = 0, int sgnd_ = -1) const {
    if (sgnd_ == -1) {
      sgnd_ = sgnd;
    }

    if (key_len_ == 0) {
      key_len_ = key_len;
    }

    td::BigInt256 x;
    x.enforce(x.parse_dec(key));
    unsigned char buffer[key_len_];
    x.export_bits(td::BitPtr{buffer}, key_len_, sgnd_);
    const auto mdict = my_dict.get();

    auto cr = mdict->lookup_ref(td::BitPtr{buffer}, key_len_);

    if (cr.is_null()) {
      throw vm::CellBuilder::CellCreateError();
    }

    return PyCell(cr);
  }

  PyCell lookup_delete_ref(const std::string& key, int key_len_ = 0, int sgnd_ = -1) const {
    if (sgnd_ == -1) {
      sgnd_ = sgnd;
    }

    if (key_len_ == 0) {
      key_len_ = key_len;
    }

    td::BigInt256 x;
    x.enforce(x.parse_dec(key));
    unsigned char buffer[key_len_];
    x.export_bits(td::BitPtr{buffer}, key_len_, sgnd_);
    const auto mdict = my_dict.get();

    auto cr = mdict->lookup_delete_ref(td::BitPtr{buffer}, key_len_);

    if (cr.is_null()) {
      throw vm::CellBuilder::CellCreateError();
    }

    return PyCell(cr);
  }

  std::string to_boc() const {
    const auto d = *my_dict;
    td::Ref<vm::Cell> root = d.get_root_cell();

    return td::base64_encode(std_boc_serialize(root, 31).move_as_ok());
  }

  std::string toString() const {
    std::stringstream os;
    const auto d = *my_dict;
    td::Ref<vm::Cell> root = d.get_root_cell();
    vm::load_cell_slice(root).dump(os);

    auto t = os.str();
    t.pop_back();

    return "<VmDict " + t + ">";
  }

  std::string dump() const {
    std::stringstream os;
    const auto d = *my_dict;
    td::Ref<vm::Cell> root = d.get_root_cell();
    vm::load_cell_slice(root).print_rec(os);

    return os.str();
  }
};

PYBIND11_MODULE(python_ton, m) {
  SET_VERBOSITY_LEVEL(verbosity_INFO);
  PSLICE() << "123";

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
      .def("load_var_integer_str", &PyCellSlice::load_var_integer_str, py::arg("bit_len"), py::arg("sgnd"))
      .def("__repr__", &PyCellSlice::toString);

  py::class_<PyCell>(m, "PyCell")
      .def(py::init<>())
      .def("get_hash", &PyCell::get_hash)
      .def("begin_parse", &PyCell::begin_parse)
      .def("dump", &PyCell::dump)
      .def("to_boc", &PyCell::to_boc)
      .def("__repr__", &PyCell::toString);

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
}
