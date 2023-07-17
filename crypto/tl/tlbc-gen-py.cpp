#include "tlbc-gen-py.h"
#include "td/utils/bits.h"
#include "td/utils/filesystem.h"
#include "crypto/parser/symtable.h"

namespace tlbc {

/*
 * 
 *   Python CODE GENERATION
 * 
 */

std::set<std::string> forbidden_py_idents, local_forbidden_py_idents;
std::vector<std::unique_ptr<PyTypeCode>> py_type;
std::vector<std::string> const_type_expr_py_idents;
std::vector<bool> const_pytype_expr_simple;

void assign_const_type_py_idents() {
  const_type_expr_py_idents.resize(const_type_expr_num + 1, "");
  const_pytype_expr_simple.resize(const_type_expr_num + 1, false);
  for (int i = 1; i <= const_type_expr_num; i++) {
    const TypeExpr* expr = const_type_expr[i];
    if (!expr->is_nat) {
      if (expr->tp == TypeExpr::te_Ref && expr->args[0]->tp == TypeExpr::te_Apply &&
          (expr->args[0]->type_applied == Any_type || expr->args[0]->type_applied == Cell_type)) {
        const_type_expr_py_idents[i] = "t_RefCell";
        const_pytype_expr_simple[i] = true;
        continue;
      }
      if (expr->tp == TypeExpr::te_Apply) {
        const Type* typ = expr->type_applied;
        int idx = typ->type_idx;
        if (typ == Any_type || typ == Cell_type || typ == Nat_type) {
          const_type_expr_py_idents[i] = (typ == Nat_type ? "t_Nat" : "t_Anything");
          const_pytype_expr_simple[i] = true;
          continue;
        }
        if (idx >= builtin_types_num && idx < types_num && !py_type[idx]->params) {
          const_type_expr_py_idents[i] = py_type[idx]->py_type_var_name;
          const_pytype_expr_simple[i] = true;
          continue;
        }
      }
      std::ostringstream ss;
      ss << "t";
      expr->const_type_name(ss);
      const_type_expr_py_idents[i] = global_cpp_ids.new_ident(ss.str());
    }
  }
}

void init_forbidden_py_idents() {
  std::set<std::string>& f = forbidden_py_idents;
  f.insert("False");
  f.insert("def");
  f.insert("if");
  f.insert("raise");
  f.insert("None");
  f.insert("del");
  f.insert("import");
  f.insert("return");
  f.insert("True");
  f.insert("elif");
  f.insert("in");
  f.insert("try");
  f.insert("and");
  f.insert("else");
  f.insert("is");
  f.insert("while");
  f.insert("as");
  f.insert("except");
  f.insert("lambda");
  f.insert("with");
  f.insert("assert");
  f.insert("finally");
  f.insert("nonlocal");
  f.insert("yield");
  f.insert("break");
  f.insert("for");
  f.insert("not");
  f.insert("class");
  f.insert("from");
  f.insert("or");
  f.insert("continue");
  f.insert("global");
  f.insert("pass");
  f.insert("Cell");
  f.insert("CellSlice");
  f.insert("CellBuilder");
  f.insert("PyDict");
  f.insert("PyEmulator");

  std::set<std::string>& l = local_forbidden_py_idents;
  l.insert("123");  // todo: fix
}

void PyAction::show(std::ostream& os) const {
  if (fixed_size >= 0) {
    if (!fixed_size) {
      os << "True";
    } else if (fixed_size < 0x10000) {
      os << "cs.advance(" << fixed_size << ")";
    } else if (!(fixed_size & 0xffff)) {
      os << "cs.advance_refs(" << (fixed_size >> 16) << ")";
    } else {
      os << "cs.advance_ext(0x" << std::hex << fixed_size << std::dec << ")";
    }
  } else {
    os << action;
  }
}

bool PyAction::may_combine(const PyAction& next) const {
  return !fixed_size || !next.fixed_size || (fixed_size >= 0 && next.fixed_size >= 0);
}

bool PyAction::operator+=(const PyAction& next) {
  if (!next.fixed_size) {
    return true;
  }
  if (!fixed_size) {
    fixed_size = next.fixed_size;
    action = next.action;
    return true;
  }
  if (fixed_size >= 0 && next.fixed_size >= 0) {
    fixed_size += next.fixed_size;
    return true;
  }
  return false;
}

void operator+=(std::vector<PyAction>& av, const PyAction& next) {
  if (av.empty() || !(av.back() += next)) {
    if (next.is_constraint && !av.empty() && av.back().fixed_size >= 0) {
      PyAction last = av.back();
      av.pop_back();
      av.push_back(next);
      av.push_back(last);
    } else {
      av.push_back(next);
    }
  }
}

void prepare_generate_py(int options = 0) {
  std::vector<std::pair<int, int>> pairs;
  pairs.reserve(types_num - builtin_types_num);
  for (int i = builtin_types_num; i < types_num; i++) {
    pairs.emplace_back(types.at(i).last_declared, i);
  }
  std::sort(pairs.begin(), pairs.end());
  type_gen_order.reserve(pairs.size());
  for (auto z : pairs) {
    type_gen_order.push_back(z.second);
  }
  py_type.resize(types_num);
  for (int i : type_gen_order) {
    Type& type = types[i];
    py_type[i] = std::make_unique<PyTypeCode>(type);
    PyTypeCode& cc = *py_type[i];
    if (!py_type[i] || !cc.is_ok()) {
      throw src::Fatal{std::string{"cannot generate py code for type `"} + type.get_name() + "`"};
    }
  }
  //  split_namespace_id();
  assign_const_type_py_idents();
}

bool PyTypeCode::init() {
  builtin = type.is_builtin;
  cons_num = type.constr_num;
  params = ret_params = tot_params = 0;
  for (int z : type.args) {
    if ((z & Type::_IsNeg)) {
      ++ret_params;
    } else {
      ++params;
    }
    ++tot_params;
  }

  assign_class_name();
  assign_cons_names();
  assign_class_field_names();
  assign_cons_values();
  assign_record_cons_names();
  simple_get_size = type.has_fixed_size;
  inline_skip = simple_get_size;
  inline_validate_skip = (inline_skip && type.any_bits && !(type.size.min_size() & 0xff));
  inline_get_tag = (type.is_pfx_determ && type.useful_depth <= 6);
  simple_cons_tags = compute_simple_cons_tags();
  common_cons_len = type.cons_common_len();
  incremental_cons_tags = check_incremental_cons_tags();
  return true;
}

bool PyTypeCode::check_incremental_cons_tags() const {
  if (!cons_num || common_cons_len < 0) {
    return false;
  }
  int l = common_cons_len;
  if (!l || l > 32) {
    return true;
  }
  for (int i = 0; i < cons_num; i++) {
    unsigned long long tag = (type.constructors.at(i)->tag >> (64 - l));
    if (tag != (unsigned)cons_enum_value.at(i)) {
      return false;
    }
  }
  return true;
}

bool PyTypeCode::compute_simple_cons_tags() {
  if (!type.is_pfx_determ || type.useful_depth > 8) {
    return false;
  }
  int d = type.useful_depth;
  int n = (1 << d);
  cons_tag_map.resize(n, 0);
  //  std::cerr << "compute_simple_cons_tags() for `" << type.get_name() << "` (d=" << d << ")\n";
  for (int i = 0; i < cons_num; i++) {
    int t = cons_enum_value.at(i) + 1;
    for (unsigned long long z : type.constructors[i]->begins_with.pfx) {
      int l = std::min(63 - td::count_trailing_zeroes_non_zero64(z), d);
      assert(l <= d);
      int a = d ? (int)((z & (z - 1)) >> (64 - d)) : 0;
      int b = (1 << (d - l));
      while (b-- > 0) {
        assert(!cons_tag_map.at(a) || cons_tag_map[a] == t);
        cons_tag_map[a++] = t;
      }
    }
  }
  int c = 0;
  for (int v : cons_tag_map) {
    if (v && v != c && v != ++c) {
      return false;
    }
  }
  return true;
}

py_val_type detect_py_type(const TypeExpr* expr) {
  if (expr->tp == TypeExpr::te_Ref) {
    return py_cell;
  }
  if (expr->is_nat) {
    return py_int32;
  }
  MinMaxSize sz = expr->compute_size();
  int l = sz.fixed_bit_size();
  if (expr->is_nat_subtype) {
    return l == 1 ? py_bool : py_int32;
  }
  if (expr->tp == TypeExpr::te_CondType) {
    py_val_type subtype = detect_py_type(expr->args.at(1));
    if (subtype == py_slice || subtype == py_cell || subtype == py_integer || subtype == py_bitstring ||
        subtype == py_enum) {
      return subtype;
    }
    if ((subtype == py_int32 || subtype == py_int64) && expr->args[1]->is_integer() > 0) {
      return subtype;
    }
    return py_slice;
  }
  int x = expr->is_integer();
  if (sz.max_size() & 0xff) {
    return py_slice;
  }
  if (!x) {
    const Type* ta = expr->type_applied;
    if (expr->tp == TypeExpr::te_Apply && ta && ta->is_simple_enum) {
      return py_enum;
    }
    if (expr->tp == TypeExpr::te_Apply && ta && ta->type_idx < builtin_types_num &&
        (ta == Bits_type || ta->get_name().at(0) == 'b')) {
      return (l >= 0 && l <= 256) ? py_bits : py_bitstring;
    }
    if (expr->tp == TypeExpr::te_Tuple && expr->args[1]->tp == TypeExpr::te_Apply &&
        expr->args[1]->type_applied->is_bool) {
      return (l >= 0 && l <= 256) ? py_bits : py_bitstring;
    }
    return py_slice;
  }
  l = (sz.max_size() >> 8);
  if (x > 0 && l == 1) {
    return py_bool;
  }
  if (l < 32) {
    return py_int32;
  }
  if (l == 32) {
    return (x < 0 ? py_int32 : py_uint32);
  }
  if (l < 64) {
    return py_int64;
  }
  if (l == 64) {
    return (x < 0 ? py_int64 : py_uint64);
  }
  return py_integer;
}

py_val_type detect_field_py_type(const Field& field) {
  return field.subrec ? py_subrecord : detect_py_type(field.type);
}

void PyTypeCode::assign_record_cons_names() {
  for (int i = 0; i < cons_num; i++) {
    const Constructor& ctor = *type.constructors.at(i);
    records.emplace_back(*this, ctor, i);
    ConsRecord& record = records.back();
    record.has_trivial_name = (cons_num <= 1 || !ctor.constr_name);
    record.declared = false;
    record.py_name = local_cpp_ids.new_ident(cons_num <= 1 ? "Record" : std::string{"Record_"} + cons_enum_name[i]);
    CppIdentSet rec_py_ids;
    rec_py_ids.insert("type_class");
    rec_py_ids.insert(record.py_name);
    // maybe : add field identifiers from type class context (?)
    for (int j = 0; j < ctor.fields_num; j++) {
      const Field& field = ctor.fields.at(j);
      if (field.constraint) {
      } else if (!field.implicit) {
        MinMaxSize sz = field.type->compute_size();
        if (!sz.max_size()) {
          continue;
        }
        std::string field_name;
        const ConsRecord* subrec = nullptr;
        if (field.name) {
          field_name = rec_py_ids.new_ident(field.get_name());
        } else if (field.subrec) {
          field_name = rec_py_ids.new_ident("r", 1);
          subrec = &py_type.at(field.type->args.at(0)->type_applied->type_idx)->records.at(0);
        } else if (field.type->tp == TypeExpr::te_Ref) {
          field_name = rec_py_ids.new_ident("ref", 1);
        }
        record.py_fields.emplace_back(field, field_name, detect_field_py_type(field), sz.fixed_bit_size(), j, subrec);
      } else if (field.used && (add_type_members || field.type->is_nat_subtype)) {
        std::string field_name = rec_py_ids.new_ident(field.get_name());
        record.py_fields.emplace_back(field, field_name, field.type->is_nat_subtype ? py_int32 : py_typeptr, -1, j,
                                      nullptr, true);
      }
    }
    auto q = std_field_names.cbegin();
    for (auto& fi : record.py_fields) {
      if (fi.name.empty()) {
        bool is_ok = false;
        while (q < std_field_names.cend()) {
          if (!rec_py_ids.defined(*q)) {
            fi.name = rec_py_ids.new_ident(*q++);
            is_ok = true;
            break;
          }
        }
        if (!is_ok) {
          fi.name = rec_py_ids.new_ident("f", 1);
        }
      }
    }
    record.is_trivial = (record.py_fields.size() <= 1);
    record.is_small = (record.py_fields.size() <= 3);
    record.inline_record = (record.py_fields.size() <= 2);
    py_val_type t = py_unknown;
    if (record.is_trivial) {
      t = (record.py_fields.size() == 1) ? record.py_fields.at(0).pytype : py_void;
    }
    std::vector<py_val_type> tv;
    for (const auto& f : record.py_fields) {
      if (f.pytype == py_subrecord) {
        record.is_trivial = record.is_small = false;
      } else if (!f.implicit) {
        tv.push_back(f.pytype);
      }
    }
    record.equiv_cpp_type = t;
    record.equiv_cpp_types = tv;
    record.triv_conflict = false;
    for (int j = 0; j < i; j++) {
      if (records[j].equiv_cpp_types == tv) {
        record.triv_conflict = records[j].triv_conflict = true;
        break;
      }
    }
  }
}

void PyTypeCode::assign_cons_values() {
  std::vector<std::pair<unsigned long long, int>> a;
  a.reserve(cons_num);
  for (int i = 0; i < cons_num; i++) {
    a.emplace_back(type.constructors[i]->begins_with.min(), i);
  }
  std::sort(a.begin(), a.end());
  cons_enum_value.resize(cons_num);
  cons_idx_by_enum.resize(cons_num);
  int i = 0;
  for (auto z : a) {
    cons_enum_value[z.second] = i;
    cons_idx_by_enum[i++] = z.second;
  }
}

void PyTypeCode::assign_class_field_names() {
  char cn = 'm', ct = 'X';
  for (int z : type.args) {
    bool f = z & Type::_IsNat;
    bool neg = (z & Type::_IsNeg);
    type_param_is_nat.push_back(f);
    type_param_is_neg.push_back(neg);
    std::string id;
    if (f) {
      id = local_cpp_ids.new_ident(std::string{cn}, 0, "_");
      if (cn != 't') {
        ++cn;
      }
      if (neg) {
        skip_extra_args += ", ";
        skip_extra_args_pass += ", ";
      }
    } else {
      id = local_cpp_ids.new_ident(std::string{ct}, 0, "_");
      if (ct != 'Z') {
        ++ct;
      } else {
        ct = 'T';
      }
      assert(!neg);
    }
    type_param_name.push_back(id);
    if (neg) {
      skip_extra_args += id + ": int";
      skip_extra_args_pass += id;
    };
  }
}

void PyTypeCode::assign_cons_names() {
  cons_enum_name.resize(cons_num);
  for (int i = 0; i < cons_num; i++) {
    sym_idx_t cons = type.constructors.at(i)->constr_name;
    if (cons) {
      cons_enum_name[i] = local_cpp_ids.new_ident(sym::symbols.get_name(cons));
    } else if (type.const_param_idx >= 0) {
      int pv = type.constructors[i]->get_const_param(type.const_param_idx);
      cons_enum_name[i] = local_cpp_ids.new_ident(pv ? "cons" : "cons0", pv);
    } else {
      cons_enum_name[i] = local_cpp_ids.new_ident("cons", i + 1);
    }
  }
}

void PyTypeCode::assign_class_name() {
  std::string type_name = type.get_name();
  sym_idx_t name = type.type_name;
  if (!name && type.parent_type_idx >= 0) {
    int i = type.parent_type_idx;
    while (true) {
      name = types.at(i).type_name;
      if (name || types.at(i).parent_type_idx < 0) {
        break;
      }
      i = types.at(i).parent_type_idx;
    }
    if (name) {
      type_name = sym::symbols.get_name(name) + "_aux";
    }
  }
  py_type_class_name = global_cpp_ids.new_ident(type_name);
  if (!params) {
    py_type_var_name = global_cpp_ids.new_ident(std::string{"t_"} + py_type_class_name);
  }
}

bool generate_py_prepared;

void generate_pytype_constant(std::ostream& os, int i, TypeExpr* expr, std::string py_name) {
  std::string cls_name = "TLB";
  int fake_arg = -1;
  cls_name = compute_type_expr_class_name(expr, fake_arg);
  os << "TLBComplex.constants[\"" << py_name << "\"] = " << cls_name;
  int c = 0;
  if (fake_arg >= 0) {
    os << '(' << fake_arg;
    c++;
  }
  for (const TypeExpr* arg : expr->args) {
    if (!arg->negated) {
      assert(arg->is_constexpr);
      os << (c++ ? ", " : "(");
      if (arg->is_nat) {
        os << arg->value;
      } else {
        os << "TLBComplex.constants[\"" << const_type_expr_py_idents.at(arg->is_constexpr) << "\"]";
      }
    }
  }
  if (c) {
    os << ')';
  }
  os << "\n";
}

void generate_pytype_constants(std::ostream& os) {
  os << "\n# "
     << "definitions of constants\n";

  for (int i : type_gen_order) {
    PyTypeCode& cc = *py_type[i];
    cc.generate_constant(os);
  }
  os << "\n";

  for (int i = 1; i <= const_type_expr_num; i++) {
    TypeExpr* expr = const_type_expr[i];
    if (!expr->is_nat && !const_pytype_expr_simple[i]) {
      generate_pytype_constant(os, i, expr, const_type_expr_py_idents[i]);
    }
  }
}

void generate_py_output_to(std::ostream& os, int options = 0) {
  tlbc::init_forbidden_py_idents();

  if (!generate_py_prepared) {
    global_cpp_ids.clear();
    cpp_type.clear();
    type_gen_order.clear();

    prepare_generate_py(options);
    generate_py_prepared = true;
  }

  os << "\nfrom enum import Enum\n";
  os << "import bitstring\n";
  os << "from tonpy.types import TLB, TLBComplex, Cell, CellSlice, CellBuilder, RefT, NatWidth, RecordBase\n";
  os << "from typing import Optional, Union\n"
     << "tlb_classes = []\n";

  for (int i : type_gen_order) {
    PyTypeCode& cc = *py_type[i];
    if (!cc.is_codegened()) {
      cc.generate(os, options);
    }
  }
  generate_pytype_constants(os);
  //  generate_register_function(os);

  generate_py_prepared = false;
}

void PyTypeCode::generate(std::ostream& os, int options) {
  std::string type_name = type.get_name();
  if (!type.type_name && type.is_auto) {
    type_name = py_type_class_name;
  }

  os << "\n\n# class for " << (type.is_auto ? "auxiliary " : "") << "type `" << type_name << "`";

  generate_class(os, options);
  type.already_codegened = true;
}

void PyTypeCode::generate_cons_enum(std::ostream& os) {
  os << "    class Tag(Enum):\n";
  for (int i = 0; i < cons_num; i++) {
    int k = cons_idx_by_enum.at(i);
    os << "        " << cons_enum_name.at(k) << " = " << i << "\n";
    assert(cons_enum_value.at(k) == i);
  }
  os << "\n";
}

void PyTypeCode::generate_cons_len_array(std::ostream& os, std::string nl, int options) {
  os << nl << "cons_len = [";
  for (int i = 0; i < cons_num; i++) {
    int k = cons_idx_by_enum.at(i);
    const Constructor& constr = *type.constructors.at(k);
    if (i > 0) {
      os << ", ";
    }
    os << constr.tag_bits;
  }
  os << "]\n";
}

void PyTypeCode::generate_cons_tag_array(std::ostream& os, std::string nl, int options) {
  int m = -1;
  for (int i = 0; i < cons_num; i++) {
    int k = cons_idx_by_enum.at(i);
    const Constructor& constr = *type.constructors.at(k);
    if (constr.tag_bits > m) {
      m = constr.tag_bits;
    }
  }

  os << nl << "cons_tag = [";

  for (int i = 0; i < cons_num; i++) {
    int k = cons_idx_by_enum.at(i);
    const Constructor& constr = *type.constructors.at(k);
    if (i > 0) {
      os << ", ";
    }
    auto tmp = HexConstWriter{constr.tag_bits ? (constr.tag >> (64 - constr.tag_bits)) : 0};
    tmp.write(os, false);
  }
  os << "]\n";
}

void PyTypeCode::generate_cons_tag_info(std::ostream& os, std::string nl, int options) {
  if (cons_num) {
    if (common_cons_len == -1) {
      generate_cons_len_array(os, nl, options);
    } else {
      os << nl << "cons_len_exact = " << common_cons_len << "\n";
    }
    //    if (common_cons_len != 0 && !incremental_cons_tags) {
    generate_cons_tag_array(os, nl, options);
    //    }
    os << "\n";
  }
}

void PyTypeCode::generate_type_fields(std::ostream& os, int options) {
  int st = -1;
  for (int i = 0; i < tot_params; i++) {
    if (type_param_is_neg[i]) {
      continue;
    }
    int nst = type_param_is_nat[i];
    os << "    " << type_param_name[i] << ": " << (nst ? "int = None\n" : "TLB = None\n");
  }
}

void PyTypeCode::generate_type_constructor(std::ostream& os, int options) {
  os << "\n    def __init__(self";

  if (tot_params > 0) {
    os << ", ";
  }

  for (int i = 0, j = 0; i < tot_params; i++) {
    if (type_param_is_neg[i]) {
      continue;
    }
    if (j++ > 0) {
      os << ", ";
    }
    os << type_param_name[i] << ": " << (type_param_is_nat[i] ? "int" : "TLB");
  }
  os << "):\n";
  os << "        super().__init__()\n";

  for (int i = 0, j = 0; i < tot_params; i++) {
    if (type_param_is_neg[i]) {
      continue;
    }
    os << "        self." << type_param_name[i] << " = " << type_param_name[i] << "\n";
  }
  if (tot_params > 0) {
    os << "\n";
  }

  generate_tag_to_class(os, options);
  os << "\n";
}

void PyTypeCode::ConsRecord::print_full_name(std::ostream& os) const {
  os << cpp_type.py_type_class_name << "." << py_name;
}

void show_pyvaltype(std::ostream& os, py_val_type x, int size = -1, bool pass_value = false) {
  switch (x) {
    case py_void:
      os << "void";  // WTF this is needed for?
      break;
    case py_slice:
      os << "CellSlice";
      break;
    case py_cell:
      os << "Cell";
      break;
    case py_typeptr:
      os << "TLB";
      break;
    case py_typeref:
      os << "TLB";
      break;

    case py_bits:
    case py_bitstring:
      os << "bitstring.BitArray";
      break;
    case py_bool:
      os << "bool";
      break;
    case py_enum:
      os << " Enum";
      break;
    case py_int32:
    case py_uint32:
    case py_int64:
    case py_uint64:
    case py_integer:
      os << "int";
      break;
    case py_subrecord:
      if (pass_value) {
        os << "const ";
      }
      os << "<unknown-py-type>::Record";
      if (pass_value) {
        os << "&";
      }
      break;
    default:
      os << "<unknown-py-scalar-type>";
  }
}

void PyValType::show(std::ostream& os, bool pass_value) const {
  show_pyvaltype(os, vt, size, pass_value);
}

void PyTypeCode::ConsField::print_type(std::ostream& os, bool pass_value) const {
  if (pytype != py_subrecord) {
    get_cvt().show(os, pass_value);
  } else {
    assert(subrec);
    subrec->print_full_name(os);
  }
}

bool PyValType::needs_move() const {
  return (vt == py_cell || vt == py_slice || vt == py_bitstring || vt == py_integer);
}

bool PyTypeCode::ConsRecord::recover_idents(CppIdentSet& idents) const {
  bool is_ok = idents.insert(py_name) && idents.insert("type_class");
  for (const auto& f : py_fields) {
    is_ok &= idents.insert(f.name);
  }
  return is_ok;
}

std::ostream& operator<<(std::ostream& os, PyValType cvt) {
  cvt.show(os);
  return os;
}

void PyTypeCode::ConsRecord::declare_record(std::ostream& os, std::string nl, int options) {
  if (declared) {
    return;
  }

  os << "\n" << nl << "class " << py_name << "(RecordBase):\n";
  os << nl << "    def get_tag_enum(self):\n";
  os << nl << "        return " << cpp_type.py_type_class_name << ".Tag." << cpp_type.cons_enum_name.at(cons_idx)
     << "\n\n";

  os << nl << "    def get_tag(self):\n";
  os << nl << "        return " << cpp_type.py_type_class_name << "."
     << "cons_tag[self.get_tag_enum().value]"
     << "\n\n";

  os << nl << "    def get_tag_len(self):\n";
  os << nl << "        return " << cpp_type.py_type_class_name << "."
     << "cons_len_exact[self.get_tag_enum().value]"
     << " if isinstance(" << cpp_type.py_type_class_name << ".cons_len_exact, list) else "
     << cpp_type.py_type_class_name << ".cons_len_exact"
     << "\n\n";

  os << nl << "    def get_type_class(self):\n"
     << "            return " << cpp_type.py_type_class_name << "\n\n";

  CppIdentSet rec_cpp_ids;
  recover_idents(rec_cpp_ids);
  std::size_t n = py_fields.size();
  for (const ConsField& fi : py_fields) {
    os << nl << "    "
       << "# ";

    if (fi.field.name) {
      os << fi.field.get_name() << " : ";
    }

    fi.field.type->show(os, &constr);

    os << "\n";

    os << nl << "    " << fi.name << ": ";
    fi.print_type(os);
    os << " = None";

    os << std::endl;
  }

  if (n) {
    std::vector<std::string> ctor_args;
    os << "\n" << nl << "    def __init__(self, ";
    int i = 0, j = 0;
    for (const ConsField& fi : py_fields) {
      if (!fi.implicit) {
        if (i++) {
          os << ", ";
        }

        std::string arg = fi.name;
        ctor_args.push_back(arg);

        os << arg << ": ";
        fi.print_type(os, true);
        os << " = None";
      }
    }
    os << "):\n";
    os << nl << "        super().__init__()\n" << nl << "        self.field_names = []\n";

    i = 0;
    for (const ConsField& fi : py_fields) {
      if (i++) {
        os << "\n";
      }
      os << nl << "        self." << fi.name << " = ";
      if (fi.implicit) {
        os << (fi.pytype == py_int32 ? "-1" : "None");
      } else {
        os << ctor_args.at(j++);
      }
      os << nl << "\n            self.field_names.append(\"" << fi.name << "\")";  // todo: move to field_names directly
    }

    os << "\n";
  }
  declared = true;
}

bool PyTypeCode::ConsRecord::declare_record_unpack(std::ostream& os, std::string nl, int options) {
  bool is_ok = false;
  bool cell = options & 16;
  std::string slice_arg = cell ? "cell_ref: Cell" : "cs: CellSlice";
  std::string fun_name = (options & 1) ? "validate_unpack" : "unpack";
  if (cell) {
    fun_name = std::string{"cell_"} + fun_name;
  }
  std::string class_name;
  class_name = cpp_type.py_type_class_name;

  if (!(options & 8)) {
    os << nl << "def " << fun_name << "(self, " << slice_arg << "";
    is_ok = true;
  } else if (is_small) {
    os << nl << "def " << fun_name << "_" << cpp_type.cons_enum_name.at(cons_idx) << "(self, " << slice_arg;
    is_ok = true;
  }
  if (is_ok) {
    if (options & 2) {
      os << cpp_type.skip_extra_args;
    }
    os << ", rec_unpack: bool = False) -> bool:\n";
  }
  return is_ok;
}

void PyTypeCode::init_cons_context(const Constructor& constr) {
  clear_context();
  field_vars.resize(constr.fields.size());
  field_var_set.resize(constr.fields.size(), false);
  param_var_set.resize(params + ret_params, false);
  param_constraint_used.resize(params + ret_params, false);
}

void PyTypeCode::bind_record_fields(const PyTypeCode::ConsRecord& rec, int options) {
  bool direct = options & 8;
  bool read_only = options & 32;
  for (const ConsField& fi : rec.py_fields) {
    int i = fi.orig_idx;
    assert(field_vars.at(i).empty() && !field_var_set.at(i));
    if (!read_only || !rec.constr.fields.at(i).implicit) {
      field_vars[i] = fi.name;
      field_var_set[i] = read_only;
    }
  }
}

void PyTypeCode::identify_cons_params(const Constructor& constr, int options) {
  int j = 0;
  for (const TypeExpr* pexpr : constr.params) {
    if (pexpr->tp == TypeExpr::te_Param) {
      if (!type_param_is_neg.at(j)) {
        int i = pexpr->value;
        if (field_var_set.at(i)) {
          // field i and parameter j must be equal
          actions += PyAction{type_param_name.at(j) + " == " + field_vars.at(i)};
          param_constraint_used[j] = true;
        } else if (field_vars.at(i).empty()) {
          // identify field i with parameter j
          field_vars[i] = type_param_name.at(j);
          field_var_set[i] = true;
          param_constraint_used[j] = true;
        }
      } else if (!(options & 2)) {
        tmp_vars.push_back(type_param_name.at(j));
      }
    }
    j++;
  }
}

void PyTypeCode::identify_cons_neg_params(const Constructor& constr, int options) {
  int j = 0;
  for (const TypeExpr* pexpr : constr.params) {
    if (pexpr->tp == TypeExpr::te_Param && type_param_is_neg.at(j)) {
      int i = pexpr->value;
      if (!field_var_set.at(i) && field_vars.at(i).empty()) {
        // identify field i with parameter j
        field_vars[i] = type_param_name.at(j);
        param_constraint_used[j] = true;
      }
    }
    j++;
  }
}

void PyTypeCode::add_cons_tag_check(const Constructor& constr, int cidx, int options) {
  if (constr.tag_bits) {
    if ((options & 1) && ((options & 8) || cons_num == 1 || !cons_tag_exact.at(cidx))) {
      std::ostringstream ss;
      int l = constr.tag_bits;
      unsigned long long tag = (constr.tag >> (64 - l));
      if (l < 64) {
        ss << "assert cs.load_uint(" << l << ") == ";
        auto w = HexConstWriter{tag};
        w.write(ss, false);
      } else {
        //        ss << "cs.begins_with_skip_bits(" << l << ", ";
        //        auto w = HexConstWriter{tag};
        //        w.write(ss, false);
        //        ss << ")";
        throw std::logic_error("Unreachable");
      }
      actions.emplace_back(std::move(ss));
    } else {
      actions.emplace_back(constr.tag_bits);
    }
  }
}

void PyTypeCode::output_cpp_expr(std::ostream& os, const TypeExpr* expr, int prio, bool allow_type_neg) const {
  if (expr->negated) {
    if (!allow_type_neg || expr->tp != TypeExpr::te_Apply) {
      throw src::Fatal{static_cast<std::ostringstream&&>(std::ostringstream{} << "cannot convert negated expression `"
                                                                              << expr << "` into C++ code")
                           .str()};
    }
  }
  int pos_args = 0;
  for (const TypeExpr* arg : expr->args) {
    pos_args += !arg->negated;
  }
  switch (expr->tp) {
    case TypeExpr::te_Param: {
      int i = expr->value;
      assert(field_var_set.at(i));
      std::string fv = field_vars.at(i);
      assert(!fv.empty());
      os << fv;
      return;
    }
    case TypeExpr::te_Apply:
      if (!pos_args && expr->type_applied->type_idx >= builtin_types_num) {
        int type_idx = expr->type_applied->type_idx;
        const PyTypeCode& cc = *py_type.at(type_idx);
        assert(!cc.py_type_var_name.empty());
        os << cc.py_type_var_name;
        return;
      }
      // fall through
    case TypeExpr::te_Ref:
    case TypeExpr::te_CondType:
    case TypeExpr::te_Tuple:
      if (expr->is_constexpr > 0) {
        auto n = expr->is_constexpr;
        os << const_type_expr_py_idents.at(n);
        return;
      } else {
        int fake_arg = -1;
        os << compute_type_expr_class_name(expr, fake_arg);
        os << "{";
        int c = 0;
        if (fake_arg >= 0) {
          os << fake_arg;
          c = 1;
        }
        for (const TypeExpr* arg : expr->args) {
          if (!arg->negated) {
            os << (c++ ? ", " : "");
            output_cpp_expr(os, arg);
          }
        }
        os << '}';
        return;
      }
    case TypeExpr::te_Add:
      if (prio > 10) {
        os << "(";
      }
      output_cpp_expr(os, expr->args[0], 10);
      os << " + ";
      output_cpp_expr(os, expr->args[1], 10);
      if (prio > 10) {
        os << ")";
      }
      return;
    case TypeExpr::te_MulConst:
      if (prio > 20) {
        os << "(";
      }
      os << expr->value;
      os << " * ";
      output_cpp_expr(os, expr->args[0], 20);
      if (prio > 20) {
        os << ")";
      }
      return;
    case TypeExpr::te_GetBit:
      if (prio > 0) {
        os << "(";
      }
      output_cpp_expr(os, expr->args[0], 5);
      os << " & ";
      if (expr->args[1]->tp == TypeExpr::te_IntConst && (unsigned)expr->args[1]->value <= 31) {
        int v = expr->args[1]->value;
        if (v > 1024) {
          os << "0x" << std::hex << (1 << v) << std::dec;
        } else {
          os << (1 << v);
        }
      } else {
        os << "(1 << ";
        output_cpp_expr(os, expr->args[1], 5);
        os << ")";
      }
      if (prio > 0) {
        os << ")";
      }
      return;
    case TypeExpr::te_IntConst:
      os << expr->value;
      return;
  }
  os << "<unknown-expression>";
}

std::string PyTypeCode::new_tmp_var() {
  char buffer[16];
  while (true) {
    sprintf(buffer, "t%d", ++tmp_ints);
    if (tmp_cpp_ids.is_good_ident(buffer) && local_cpp_ids.is_good_ident(buffer)) {
      break;
    }
  }
  std::string s{buffer};
  s = tmp_cpp_ids.new_ident(s);
  tmp_vars.push_back(s);
  return s;
}

std::string PyTypeCode::new_tmp_var(std::string hint) {
  if (hint.empty() || hint == "_") {
    return new_tmp_var();
  }
  int count = 0;
  while (true) {
    std::string s = local_cpp_ids.compute_cpp_ident(hint, count++);
    if (tmp_cpp_ids.is_good_ident(s) && local_cpp_ids.is_good_ident(s)) {
      s = tmp_cpp_ids.new_ident(s);
      tmp_vars.push_back(s);
      return s;
    }
  }
}

void PyTypeCode::add_compute_actions(const TypeExpr* expr, int i, std::string bind_to) {
  assert(expr->negated && expr->is_nat);
  switch (expr->tp) {
    case TypeExpr::te_MulConst: {
      assert(expr->args.size() == 1 && expr->value > 0);
      const TypeExpr* x = expr->args[0];
      assert(x->negated);
      std::string tmp;
      if (x->tp != TypeExpr::te_Param || (x->value != i && i >= 0)) {
        tmp = new_tmp_var();
      } else {
        i = x->value;
        tmp = field_vars.at(i);
        assert(!tmp.empty());
        assert(!field_var_set[i]);
        field_var_set[i] = true;
        x = nullptr;
      }
      std::ostringstream ss;
      ss << "mul_r1(" << tmp << ", " << expr->value << ", " << bind_to << ")";
      actions += PyAction{std::move(ss), true};
      if (x) {
        add_compute_actions(x, i, tmp);
      }
      return;
    }
    case TypeExpr::te_Add: {
      assert(expr->args.size() == 2);
      const TypeExpr *x = expr->args[0], *y = expr->args[1];
      assert(x->negated ^ y->negated);
      if (!x->negated) {
        std::swap(x, y);
      }
      std::string tmp;
      if (x->tp != TypeExpr::te_Param || (x->value != i && i >= 0)) {
        tmp = new_tmp_var();
      } else {
        i = x->value;
        tmp = field_vars.at(i);
        assert(!tmp.empty());
        assert(!field_var_set[i]);
        field_var_set[i] = true;
        x = nullptr;
      }
      std::ostringstream ss;
      ss << "add_r1(" << tmp << ", ";
      output_cpp_expr(ss, y);
      ss << ", " << bind_to << ")";
      actions += PyAction{std::move(ss), true};
      if (x) {
        add_compute_actions(x, i, tmp);
      }
      return;
    }
    case TypeExpr::te_Param:
      assert(expr->value == i || i < 0);
      i = expr->value;
      assert(!field_vars.at(i).empty());
      if (!field_var_set.at(i)) {
        actions += PyAction{std::string{"("} + field_vars.at(i) + " = " + bind_to + ") >= 0"};
        field_var_set[i] = true;
      } else {
        actions += PyAction{field_vars.at(i) + " == " + bind_to};
      }
      return;
  }
  throw src::Fatal{static_cast<std::ostringstream&&>(std::ostringstream{} << "cannot use expression `" << expr << "` = "
                                                                          << bind_to << " to set field variable "
                                                                          << (i >= 0 ? field_vars.at(i) : "<unknown>"))
                       .str()};
}

bool PyTypeCode::add_constraint_check(const Constructor& constr, const Field& field, int options) {
  const TypeExpr* expr = field.type;
  if (expr->tp == TypeExpr::te_Apply &&
      (expr->type_applied == Eq_type || expr->type_applied == Less_type || expr->type_applied == Leq_type)) {
    assert(expr->args.size() == 2);
    const TypeExpr *x = expr->args[0], *y = expr->args[1];
    if (x->negated || y->negated) {
      assert(expr->type_applied == Eq_type);
      assert(x->negated ^ y->negated);
      if (!x->negated) {
        std::swap(x, y);
      }
      std::ostringstream ss;
      output_cpp_expr(ss, y);
      add_compute_actions(x, -1, ss.str());
    } else {
      std::ostringstream ss;
      ss << "assert ";
      if (x->tp == TypeExpr::te_Param) {
        ss << "self.";
      }

      output_cpp_expr(ss, x);
      ss << (expr->type_applied == Eq_type ? " == " : (expr->type_applied == Less_type ? " < " : " <= "));

      if (y->tp == TypeExpr::te_Param) {
        ss << "self.";
      }

      output_cpp_expr(ss, y);
      actions += PyAction{std::move(ss), true};
    }
    return true;
  } else {
    // ...
    ++incomplete;
    actions += PyAction{"check_constraint_incomplete"};
    return false;
  }
}

std::string PyTypeCode::add_fetch_nat_field(const Constructor& constr, const Field& field, int options) {
  const TypeExpr* expr = field.type;
  int i = field.field_idx;
  std::string id = field_vars.at(i);
  if (id.empty()) {
    field_vars[i] = id = new_tmp_var(field.get_name());
  }
  const Type* ta = expr->type_applied;
  assert(expr->tp == TypeExpr::te_Apply &&
         (ta == Nat_type || ta == NatWidth_type || ta == NatLeq_type || ta == NatLess_type));
  std::ostringstream ss;
  ss << "self." << id << " = ";
  if (ta == Nat_type) {
    ss << "cs.load_uint(32)\n";
  } else if (ta == NatWidth_type && expr->args.at(0)->tp == TypeExpr::te_IntConst && expr->args[0]->value == 1) {
    ss << "cs.load_bool()\n";
  } else {
    if (ta == NatWidth_type) {
      ss << "cs.load_uint(";
    } else if (ta == NatLeq_type) {
      ss << "cs.load_uint_leq(";
    } else if (ta == NatLess_type) {
      ss << "cs.load_uint_less(";
    }
    output_cpp_expr(ss, expr->args[0]);
    ss << ")";
  }
  actions += PyAction{std::move(ss)};
  field_var_set[i] = true;
  return id;
}

void PyTypeCode::output_fetch_subrecord(std::ostream& os, std::string field_name, const ConsRecord* subrec) {
  assert(subrec);
  os << subrec->cpp_type.py_type_var_name << ".cell_unpack(cs.fetch_ref(), " << field_name << ")";
}

void PyTypeCode::output_cpp_sizeof_expr(std::ostream& os, const TypeExpr* expr, int prio) const {
  if (expr->negated) {
    throw src::Fatal{static_cast<std::ostringstream&&>(std::ostringstream{}
                                                       << "cannot compute size of negated type expression `" << expr
                                                       << "` in C++ code")
                         .str()};
  }
  if (expr->is_nat) {
    throw src::Fatal{static_cast<std::ostringstream&&>(std::ostringstream{}
                                                       << "cannot compute size of non-type expression `" << expr
                                                       << "` in C++ code")
                         .str()};
  }
  MinMaxSize sz = expr->compute_size();
  if (sz.is_fixed()) {
    os << SizeWriter{(int)sz.convert_min_size()};
    return;
  }
  switch (expr->tp) {
    case TypeExpr::te_CondType:
      if (prio > 5) {
        os << '(';
      }
      output_cpp_expr(os, expr->args[0], 5);
      os << " ? ";
      output_cpp_sizeof_expr(os, expr->args[1], 6);
      os << " : 0";
      if (prio > 5) {
        os << ')';
      }
      return;
    case TypeExpr::te_Tuple:
      if (expr->args[0]->tp == TypeExpr::te_IntConst && expr->args[0]->value == 1) {
        output_cpp_sizeof_expr(os, expr->args[1], prio);
        return;
      }
      sz = expr->args[1]->compute_size();
      if (sz.is_fixed() && sz.convert_min_size() == 1) {
        output_cpp_expr(os, expr->args[0], prio);
        return;
      }
      if (prio > 20) {
        os << '(';
      }
      output_cpp_expr(os, expr->args[0], 20);
      os << " * ";
      output_cpp_sizeof_expr(os, expr->args[1], 20);
      if (prio > 20) {
        os << ')';
      }
      return;
    case TypeExpr::te_Apply:
      if (expr->type_applied == Int_type || expr->type_applied == UInt_type || expr->type_applied == NatWidth_type ||
          expr->type_applied == Bits_type) {
        output_cpp_expr(os, expr->args[0], prio);
        return;
      }
      // no break
  }
  os << "<unknown-expression>";
}

void PyTypeCode::output_fetch_field(std::ostream& os, std::string field_var, const TypeExpr* expr, py_val_type cvt) {
  int i = expr->is_integer();
  MinMaxSize sz = expr->compute_size();
  int l = (sz.is_fixed() ? sz.convert_min_size() : -1);
  switch (cvt) {
    case py_slice:
      os << "self." << field_var << " = cs.load_subslice" << (sz.max_size() & 0xff ? "_ext(" : "(");
      output_cpp_sizeof_expr(os, expr, 0);
      os << ")";
      return;
    case py_bitstring:
    case py_bits:
      assert(!(sz.max_size() & 0xff));
      os << "self." << field_var << " = cs.load_bitstring(";
      output_cpp_sizeof_expr(os, expr, 0);
      os << ")";
      return;
    case py_cell:
      assert(l == 0x10000);
      os << "self." << field_var << " = cs.load_ref()";
      return;
    case py_bool:
      assert(i > 0 && l == 1);
      os << "self." << field_var << " = "
         << "cs.load_bool()";
      return;
    case py_int32:
    case py_uint32:
    case py_int64:
    case py_integer:
    case py_uint64:
      assert(i && l <= 64);
      os << "self." << field_var << "cs.load_" << (i > 0 ? "u" : "") << "int(";
      output_cpp_sizeof_expr(os, expr, 0);
      os << ")";
      return;
    default:
      break;
  }
  throw src::Fatal{"cannot fetch a field of unknown scalar type"};
}

bool PyTypeCode::can_compute(const TypeExpr* expr) const {
  if (expr->negated) {
    return false;
  }
  if (expr->tp == TypeExpr::te_Param) {
    return field_var_set.at(expr->value);
  }
  for (const TypeExpr* arg : expr->args) {
    if (!can_compute(arg)) {
      return false;
    }
  }
  return true;
}

bool PyTypeCode::can_use_to_compute(const TypeExpr* expr, int i) const {
  if (!expr->negated || !expr->is_nat) {
    return false;
  }
  if (expr->tp == TypeExpr::te_Param) {
    return expr->value == i;
  }
  for (const TypeExpr* arg : expr->args) {
    if (!(arg->negated ? can_use_to_compute(arg, i) : can_compute(arg))) {
      return false;
    }
  }
  return true;
}

void PyTypeCode::compute_implicit_field(const Constructor& constr, const Field& field, int options) {
  int i = field.field_idx;
  if (field_vars.at(i).empty()) {
    assert(!field_var_set.at(i));
    assert(field.type->is_nat_subtype);
    std::string id = new_tmp_var(field.get_name());
    field_vars[i] = id;
  }
  int j = -1;
  for (const TypeExpr* pexpr : constr.params) {
    ++j;
    if (!param_constraint_used.at(j) && !type_param_is_neg.at(j)) {
      // std::cerr << "can_use_to_compute(" << pexpr << ", " << i << ") = " << can_use_to_compute(pexpr, i) << std::endl;
      if (!field_var_set.at(i) && pexpr->tp == TypeExpr::te_Param && pexpr->value == i) {
        std::ostringstream ss;
        if (field.type->is_nat_subtype) {
          ss << "(" << field_vars[i] << " = " << type_param_name.at(j) << ") >= 0";
        } else {
          ss << "(" << field_vars[i] << " = &" << type_param_name.at(j) << ")";
        }
        actions += PyAction{std::move(ss)};
        field_vars[i] = type_param_name[j];
        field_var_set[i] = true;
        param_constraint_used[j] = true;
      } else if (can_compute(pexpr)) {
        std::ostringstream ss;
        ss << type_param_name.at(j) << " == ";
        output_cpp_expr(ss, pexpr);
        actions += PyAction{std::move(ss), true};
        param_constraint_used[j] = true;
      } else if (!field_var_set.at(i) && can_use_to_compute(pexpr, i)) {
        add_compute_actions(pexpr, i, type_param_name.at(j));
        param_constraint_used[j] = true;
      }
    }
  }
}

bool PyTypeCode::is_self(const TypeExpr* expr, const Constructor& constr) const {
  if (expr->tp != TypeExpr::te_Apply || expr->type_applied != &type || (int)expr->args.size() != tot_params) {
    return false;
  }
  assert(constr.params.size() == expr->args.size());
  for (int i = 0; i < tot_params; i++) {
    assert(type_param_is_neg[i] == expr->args[i]->negated);
    assert(type_param_is_neg[i] == constr.param_negated[i]);
    if (!type_param_is_neg[i] && !expr->args[i]->equal(*constr.params[i])) {
      return false;
    }
  }
  return true;
}

void PyTypeCode::output_negative_type_arguments(std::ostream& os, const TypeExpr* expr) {
  assert(expr->tp == TypeExpr::te_Apply);
  for (const TypeExpr* arg : expr->args) {
    if (arg->negated) {
      int j = arg->value;
      if (arg->tp == TypeExpr::te_Param && !field_var_set.at(j)) {
        assert(!field_vars.at(j).empty());
        os << ", " << field_vars.at(j);
        field_var_set[j] = true;
      } else {
        std::string tmp = new_tmp_var();
        os << ", " << tmp;
        postponed_equate.emplace_back(tmp, arg);
      }
    }
  }
}

void PyTypeCode::add_postponed_equate_actions() {
  for (const auto& p : postponed_equate) {
    add_compute_actions(p.second, -1, p.first);
  }
  postponed_equate.clear();
}

bool PyTypeCode::can_compute_sizeof(const TypeExpr* expr) const {
  if (expr->negated || expr->is_nat) {
    return false;
  }
  MinMaxSize sz = expr->compute_size();
  if (sz.is_fixed()) {
    return !(sz.min_size() & 0xff);
  }
  if (expr->tp == TypeExpr::te_Apply && (expr->type_applied == Int_type || expr->type_applied == UInt_type ||
                                         expr->type_applied == NatWidth_type || expr->type_applied == Bits_type)) {
    return true;
  }
  if (expr->tp != TypeExpr::te_CondType && expr->tp != TypeExpr::te_Tuple) {
    return false;
  }
  return can_compute_sizeof(expr->args[1]);
}

void PyTypeCode::generate_unpack_field(const PyTypeCode::ConsField& fi, const Constructor& constr, const Field& field,
                                       int options) {
  int i = field.field_idx;
  const TypeExpr* expr = field.type;
  MinMaxSize sz = expr->compute_size();
  bool any_bits = expr->compute_any_bits();
  bool validating = (options & 1);
  py_val_type cvt = fi.pytype;
  // std::cerr << "field `" << field.get_name() << "` size is " << sz << "; fixed=" << sz.is_fixed() << "; any=" << any_bits << std::endl;
  if (field.used || expr->is_nat_subtype) {
    assert(expr->is_nat_subtype && "cannot use fields of non-`#` type");
    assert(cvt == py_int32 || cvt == py_bool);
    add_fetch_nat_field(constr, field, options);
    return;
  }
  if (sz.is_fixed() && cvt != py_enum && (!validating || (!(sz.min_size() & 0xff) && any_bits))) {
    // field has fixed size, and either its bits can have arbitrary values (and it has no references)
    // ... or we are not validating
    // simply skip the necessary amount of bits
    // NB: if the field is a reference, and we are not validating, we arrive here
    if (cvt == py_cell) {
      assert(sz.min_size() == 1);
    }
    std::ostringstream ss;
    if ((cvt == py_subrecord || cvt == py_cell || cvt == py_slice) && field.subrec) {
      output_fetch_subrecord(ss, field_vars.at(i), fi.subrec);
    } else {
      if (cvt == py_cell || cvt == py_slice) {  // Load as var first
        std::ostringstream ss2;
        output_fetch_field(ss2, field_vars.at(i), expr, cvt);
        if (!is_self(expr, constr)) {
          ss << "\n                if rec_unpack:\n"
             << "                    self." << field_vars.at(i) << " = TLBComplex.constants[\"";
          output_cpp_expr(ss, expr, 100);
          ss << "\"].fetch(self." << field_vars.at(i) << ", True)\n";
          ss << "                    assert self." << field_vars.at(i) << " is not None\n";
        }

        actions += PyAction{std::move(ss2)};
      } else {
        output_fetch_field(ss, field_vars.at(i), expr, cvt);
      }
    }
    actions += PyAction{std::move(ss)};
    field_var_set[i] = true;
    return;
  }
  if (expr->negated) {
    // the field type has some "negative" parameters, which will be computed while checking this field
    // must invoke the correct validate_skip or skip method for the type in question
    std::ostringstream ss;
    assert(cvt == py_slice);
    if (!is_self(expr, constr)) {
      output_cpp_expr(ss, expr, 100, true);
      ss << '.';
    }
    ss << (validating ? "validate_fetch_to(ops, cs, weak, " : "fetch_to(cs, ") << field_vars.at(i);
    output_negative_type_arguments(ss, expr);
    ss << ")";
    actions += PyAction{std::move(ss)};
    add_postponed_equate_actions();
    field_var_set[i] = true;
    return;
  }
  // at this point, if the field type is a reference, we must be validating
  if (expr->tp == TypeExpr::te_Ref && expr->args[0]->tp == TypeExpr::te_Apply &&
      (expr->args[0]->type_applied == Cell_type || expr->args[0]->type_applied == Any_type)) {
    // field type is a reference to a cell with arbitrary contents
    assert(cvt == py_cell);
    actions += PyAction{"cs.fetch_ref_to(" + field_vars.at(i) + ")"};
    field_var_set[i] = true;
    return;
  }
  // remaining case: general positive type expression
  std::ostringstream ss;
  std::string tail;
  while (expr->tp == TypeExpr::te_CondType) {
    // optimization for (chains of) conditional types ( x?type )
    assert(expr->args.size() == 2);
    ss << "(!";
    output_cpp_expr(ss, expr->args[0], 30);
    ss << " || ";
    expr = expr->args[1];
    tail = std::string{")"} + tail;
  }
  if ((!validating || any_bits) && can_compute_sizeof(expr) && cvt != py_enum) {
    // field size can be computed at run-time, and either the contents is arbitrary, or we are not validating
    output_fetch_field(ss, field_vars.at(i), expr, cvt);
    field_var_set[i] = true;
    ss << tail;
    actions += PyAction{std::move(ss)};
    return;
  }
  if (expr->tp != TypeExpr::te_Ref) {
    // field type is not a reference, generate a type expression and invoke skip/validate_skip method
    assert(cvt == py_slice || cvt == py_enum);
    ss << "self." << field_vars.at(i) << " = ";

    if (!is_self(expr, constr)) {
      ss << "TLBComplex.constants[\"";
      output_cpp_expr(ss, expr, 100);
      ss << "\"].";
    } else {
      ss << "self.";
    }

    ss << (validating ? "validate_" : "") << "fetch" << (cvt == py_enum ? "_enum" : "")
       << (validating ? "to(ops, cs, weak, " : "(cs) ") << tail;
    field_var_set[i] = true;
    actions += PyAction{std::move(ss)};
    return;
  }
  // the (remaining) field type is a reference
  if (!validating || (expr->args[0]->tp == TypeExpr::te_Apply &&
                      (expr->args[0]->type_applied == Cell_type || expr->args[0]->type_applied == Any_type))) {
    // the subcase when the field type is either a reference to a cell with arbitrary contents
    // or it is a reference, and we are not validating, so we simply skip the reference
    assert(cvt == py_cell);
    ss << "cs.fetch_ref_to(" << field_vars.at(i) << ")" << tail;
    field_var_set[i] = true;
    actions += PyAction{std::move(ss)};
    return;
  }
  // general reference type, invoke validate_skip_ref()
  // (notice that we are necessarily validating at this point)
  expr = expr->args[0];
  assert(cvt == py_cell);
  ss << "(cs.fetch_ref_to(" << field_vars.at(i) << ") && ";
  if (!is_self(expr, constr)) {
    output_cpp_expr(ss, expr, 100);
    ss << '.';
  }
  ss << "validate_ref(ops, " << field_vars.at(i) << "))" << tail;
  actions += PyAction{ss.str()};
}

void PyTypeCode::generate_unpack_method(std::ostream& os, PyTypeCode::ConsRecord& rec, int options) {
  std::ostringstream tmp;
  if (!rec.declare_record_unpack(tmp, "", options)) {
    return;
  }
  tmp.clear();
  os << "\n";
  bool res = rec.declare_record_unpack(os, "        ", options | 3072);
  os << "            try:\n";
  DCHECK(res);
  if (options & 16) {
    // cell unpack version
    os << "                if cell_ref.is_null():\n                    return False"
       << "\n                cs = cell_ref.begin_parse()"
       << "\n                return self.unpack";

    if (!(options & 8)) {
      os << "(cs";
    } else {
      os << "_" << cons_enum_name.at(rec.cons_idx) << "(cs";
    }
    if (options & 2) {
      os << skip_extra_args_pass;
    }
    os << ") and cs.empty_ext()\n\n";
    os << "            except (RuntimeError, AssertionError):\n                return False\n            return True\n";
    return;
  }
  init_cons_context(rec.constr);
  bind_record_fields(rec, options);
  identify_cons_params(rec.constr, options);
  identify_cons_neg_params(rec.constr, options);
  add_cons_tag_check(rec.constr, rec.cons_idx, 9 /* (options & 1) | 8 */);
  auto it = rec.py_fields.cbegin(), end = rec.py_fields.cend();
  for (const Field& field : rec.constr.fields) {
    if (field.constraint) {
      add_constraint_check(rec.constr, field, options);
      continue;
    }
    if (!field.implicit) {
      assert(it < end && it->orig_idx == field.field_idx);
      generate_unpack_field(*it++, rec.constr, field, options);
    } else {
      if (it < end && it->orig_idx == field.field_idx) {
        ++it;
      }
      compute_implicit_field(rec.constr, field, options);
    }
  }
  assert(it == end);
  add_remaining_param_constraints_check(rec.constr, options);
  output_actions(os, "                ", options | 4);
  clear_context();
  os << "            except (RuntimeError, AssertionError):\n                return False\n            return True\n";
}

void PyTypeCode::output_actions(std::ostream& os, std::string nl, int options) {
  if (tmp_vars.size() || needs_tmp_cell) {
    if (tmp_vars.size()) {
      os << nl << "int";
      int c = 0;
      for (auto t : tmp_vars) {
        if (c++) {
          os << ",";
        }
        os << " " << t;
      }
      os << ";";
    }
    if (needs_tmp_cell) {
      os << nl << "Ref<vm::Cell> tmp_cell;";
    }
  }
  if (!actions.size()) {
    os << nl << "return True";
  } else {
    for (std::size_t i = 0; i < actions.size(); i++) {
      os << (i ? "\n" + nl : nl);
      actions[i].show(os);
    }
  }
  if (incomplete) {
    os << nl << "# ???";
  }

  os << nl << "\n";
}

void PyTypeCode::add_remaining_param_constraints_check(const Constructor& constr, int options) {
  int j = 0;
  for (const TypeExpr* pexpr : constr.params) {
    if (!param_constraint_used.at(j)) {
      std::ostringstream ss;
      if (!type_param_is_neg.at(j)) {
        ss << type_param_name.at(j) << " == ";
        output_cpp_expr(ss, pexpr);
        actions += PyAction{std::move(ss)};
      } else if (options & 2) {
        ss << "(" << type_param_name.at(j) << " = ";
        output_cpp_expr(ss, pexpr);
        ss << ") >= 0";
        actions += PyAction{std::move(ss), true};
      }
    }
    ++j;
  }
}

void PyTypeCode::add_cons_tag_store(const Constructor& constr, int cidx) {
  if (constr.tag_bits) {
    std::ostringstream ss;
    int l = constr.tag_bits;
    unsigned long long tag = (constr.tag >> (64 - l));
    ss << "cb.store_long_bool(" << HexConstWriter{tag} << ", " << l << ")";
    actions.emplace_back(std::move(ss));
  }
}

void PyTypeCode::add_store_nat_field(const Constructor& constr, const Field& field, int options) {
  const TypeExpr* expr = field.type;
  int i = field.field_idx;
  std::string id = field_vars.at(i);
  assert(!id.empty());
  const Type* ta = expr->type_applied;
  assert(expr->tp == TypeExpr::te_Apply &&
         (ta == Nat_type || ta == NatWidth_type || ta == NatLeq_type || ta == NatLess_type));
  std::ostringstream ss;
  ss << "cb.";
  if (ta == Nat_type) {
    ss << "store_ulong_rchk_bool(" << id << ", 32)";
  } else if (ta == NatWidth_type) {
    if (expr->args.at(0)->tp == TypeExpr::te_IntConst && expr->args[0]->value == 1) {
      ss << "store_ulong_rchk_bool(" << id << ", 1)";
    } else {
      ss << "store_ulong_rchk_bool(" << id << ", ";
      output_cpp_expr(ss, expr->args[0]);
      ss << ")";
    }
  } else if (ta == NatLeq_type) {
    ss << "store_uint_leq(";
    output_cpp_expr(ss, expr->args[0]);
    ss << ", " << id << ")";
  } else if (ta == NatLess_type) {
    ss << "store_uint_less(";
    output_cpp_expr(ss, expr->args[0]);
    ss << ", " << id << ")";
  } else {
    ss << "<store-unknown-nat-subtype>(" << id << ")";
  }
  actions += PyAction{std::move(ss)};
  field_var_set[i] = true;
}

void PyTypeCode::add_store_subrecord(std::string field_name, const ConsRecord* subrec) {
  assert(subrec);
  needs_tmp_cell = true;
  std::ostringstream ss;
  ss << subrec->cpp_type.py_type_var_name << ".cell_pack(tmp_cell, " << field_name << ")";
  actions += PyAction{std::move(ss)};
  actions += PyAction{"cb.store_ref_bool(std::move(tmp_cell))"};
}

void PyTypeCode::output_store_field(std::ostream& os, std::string field_var, const TypeExpr* expr, py_val_type cvt) {
  int i = expr->is_integer();
  MinMaxSize sz = expr->compute_size();
  int l = (sz.is_fixed() ? sz.convert_min_size() : -1);
  switch (cvt) {
    case py_slice:
      os << "cb.append_cellslice_chk(" << field_var << ", ";
      output_cpp_sizeof_expr(os, expr, 0);
      os << ")";
      return;
    case py_bitstring:
      assert(!(sz.max_size() & 0xff));
      os << "cb.append_bitstring_chk(" << field_var << ", ";
      output_cpp_sizeof_expr(os, expr, 0);
      os << ")";
      return;
    case py_bits:
      assert(l >= 0 && l < 0x10000);
      os << "cb.store_bits_bool(" << field_var << ".cbits(), " << l << ")";
      return;
    case py_cell:
      assert(l == 0x10000);
      os << "cb.store_ref_bool(" << field_var << ")";
      return;
    case py_bool:
      assert(i > 0 && l == 1);
      // os << "cb.store_bool(" << field_var << ")";
      // return;
      // fall through
    case py_int32:
    case py_uint32:
    case py_int64:
    case py_uint64:
      assert(i && l <= 64);
      os << "cb.store_" << (i > 0 ? "u" : "") << "long_rchk_bool(" << field_var << ", ";
      output_cpp_sizeof_expr(os, expr, 0);
      os << ")";
      return;
    case py_integer:
      assert(i);
      os << "cb.store_int256_bool(" << field_var << ", ";
      output_cpp_sizeof_expr(os, expr, 0);
      os << (i > 0 ? ", false)" : ")");
      return;
    default:
      break;
  }
  throw src::Fatal{"cannot store a field of unknown scalar type"};
}

void PyTypeCode::generate_pack_field(const PyTypeCode::ConsField& fi, const Constructor& constr, const Field& field,
                                     int options) {
  int i = field.field_idx;
  const TypeExpr* expr = field.type;
  MinMaxSize sz = expr->compute_size();
  bool any_bits = expr->compute_any_bits();
  bool validating = (options & 1);
  py_val_type cvt = fi.pytype;
  // std::cerr << "field `" << field.get_name() << "` size is " << sz << "; fixed=" << sz.is_fixed() << "; any=" << any_bits << std::endl;
  if (field.used || expr->is_nat_subtype) {
    assert(expr->is_nat_subtype && "cannot use fields of non-`#` type");
    assert(cvt == py_int32 || cvt == py_bool);
    add_store_nat_field(constr, field, options);
    return;
  }
  if (sz.is_fixed() && cvt != py_enum && (!validating || (!(sz.min_size() & 0xff) && any_bits))) {
    // field has fixed size, and either its bits can have arbitrary values (and it has no references)
    // ... or we are not validating
    // simply skip the necessary amount of bits
    // NB: if the field is a reference, and we are not validating, we arrive here
    if (cvt == py_cell) {
      assert(sz.min_size() == 1);
    }
    if (cvt == py_subrecord && field.subrec) {
      add_store_subrecord(field_vars.at(i), fi.subrec);
    } else {
      std::ostringstream ss;
      output_store_field(ss, field_vars.at(i), expr, cvt);
      actions += PyAction{std::move(ss)};
    }
    field_var_set[i] = true;
    return;
  }
  if (expr->negated) {
    // the field type has some "negative" parameters, which will be computed while checking this field
    // must invoke the correct validate_skip or skip method for the type in question
    std::ostringstream ss;
    assert(cvt == py_slice);
    ss << "tlb::" << (validating ? "validate_" : "") << "store_from(cb, ";
    if (!is_self(expr, constr)) {
      output_cpp_expr(ss, expr, 5, true);
    } else {
      ss << "*this";
    }
    ss << ", " << field_vars.at(i);
    output_negative_type_arguments(ss, expr);
    ss << ")";
    actions += PyAction{std::move(ss)};
    add_postponed_equate_actions();
    field_var_set[i] = true;
    return;
  }
  // at this point, if the field type is a reference, we must be validating
  if (expr->tp == TypeExpr::te_Ref && expr->args[0]->tp == TypeExpr::te_Apply &&
      (expr->args[0]->type_applied == Cell_type || expr->args[0]->type_applied == Any_type)) {
    // field type is a reference to a cell with arbitrary contents
    assert(cvt == py_cell);
    actions += PyAction{"cb.store_ref_bool(" + field_vars.at(i) + ")"};
    field_var_set[i] = true;
    return;
  }
  // remaining case: general positive type expression
  std::ostringstream ss;
  std::string tail;
  while (expr->tp == TypeExpr::te_CondType) {
    // optimization for (chains of) conditional types ( x?type )
    assert(expr->args.size() == 2);
    ss << "(!";
    output_cpp_expr(ss, expr->args[0], 30);
    ss << " || ";
    expr = expr->args[1];
    tail = std::string{")"} + tail;
  }
  if ((!validating || any_bits) && can_compute_sizeof(expr) && cvt != py_enum) {
    // field size can be computed at run-time, and either the contents is arbitrary, or we are not validating
    output_store_field(ss, field_vars.at(i), expr, cvt);
    field_var_set[i] = true;
    ss << tail;
    actions += PyAction{std::move(ss)};
    return;
  }
  if (expr->tp != TypeExpr::te_Ref) {
    // field type is not a reference, generate a type expression and invoke skip/validate_skip method
    assert(cvt == py_slice || cvt == py_enum);
    if (!is_self(expr, constr)) {
      output_cpp_expr(ss, expr, 100);
      ss << '.';
    }
    ss << (validating ? "validate_" : "") << "store_" << (cvt == py_enum ? "enum_" : "") << "from(cb, "
       << field_vars.at(i) << ")" << tail;
    field_var_set[i] = true;
    actions += PyAction{std::move(ss)};
    return;
  }
  // the (remaining) field type is a reference
  if (!validating || (expr->args[0]->tp == TypeExpr::te_Apply &&
                      (expr->args[0]->type_applied == Cell_type || expr->args[0]->type_applied == Any_type))) {
    // the subcase when the field type is either a reference to a cell with arbitrary contents
    // or it is a reference, and we are not validating, so we simply skip the reference
    assert(cvt == py_cell);
    ss << "cb.store_ref_bool(" << field_vars.at(i) << ")" << tail;
    field_var_set[i] = true;
    actions += PyAction{std::move(ss)};
    return;
  }
  // general reference type, invoke validate_skip_ref()
  // (notice that we are necessarily validating at this point)
  expr = expr->args[0];
  assert(cvt == py_cell);
  ss << "(cb.store_ref_bool(" << field_vars.at(i) << ") && ";
  if (!is_self(expr, constr)) {
    output_cpp_expr(ss, expr, 100);
    ss << '.';
  }
  ss << "validate_ref(ops, " << field_vars.at(i) << "))" << tail;
  actions += PyAction{ss.str()};
}

bool PyTypeCode::ConsRecord::declare_record_pack(std::ostream& os, std::string nl, int options) {
  bool is_ok = false;
  bool cell = options & 16;
  std::string builder_arg = cell ? "Ref<vm::Cell>& cell_ref" : "vm::CellBuilder& cb";
  std::string fun_name = (options & 1) ? "validate_pack" : "pack";
  if (cell) {
    fun_name = std::string{"cell_"} + fun_name;
  }
  std::string class_name;
  if (options & 2048) {
    class_name = cpp_type.py_type_class_name + "::";
  }
  if (!(options & 8)) {
    os << nl << "bool " << class_name << fun_name << "(" << builder_arg << ", const " << class_name << py_name
       << "& data";
    is_ok = true;
  } else if (is_small) {
    os << nl << "bool " << class_name << fun_name << "_" << cpp_type.cons_enum_name.at(cons_idx) << "(" << builder_arg;
    for (const auto& f : py_fields) {
      // skip SOME implicit fields ???
      if (!f.implicit) {
        os << ", " << f.get_cvt() << " " << f.name;
      }
    }
    is_ok = true;
  }
  if (is_ok) {
    if (options & 2) {
      os << cpp_type.skip_extra_args;
    }
    os << ") const" << (options & 1024 ? " {" : ";\n");
  }
  return is_ok;
}

void PyTypeCode::generate_pack_method(std::ostream& os, PyTypeCode::ConsRecord& rec, int options) {
  std::ostringstream tmp;
  if (!rec.declare_record_pack(tmp, "", options)) {
    return;
  }
  tmp.clear();
  os << "\n";
  bool res = rec.declare_record_pack(os, "", options | 3072);
  DCHECK(res);
  if (options & 16) {
    // cell pack version
    os << "\n  vm::CellBuilder cb;"
       << "\n  return " << (options & 1 ? "validate_" : "") << "pack";
    if (!(options & 8)) {
      os << "(cb, data";
    } else {
      os << "_" << cons_enum_name.at(rec.cons_idx) << "(cb";
      for (const auto& f : rec.py_fields) {
        // skip SOME implicit fields ???
        if (f.implicit) {
        } else if (f.get_cvt().needs_move()) {
          os << ", std::move(" << f.name << ")";
        } else {
          os << ", " << f.name;
        }
      }
    }
    if (options & 2) {
      os << skip_extra_args_pass;
    }
    os << ") && std::move(cb).finalize_to(cell_ref);\n}\n";
    return;
  }
  init_cons_context(rec.constr);
  bind_record_fields(rec, options | 32);
  identify_cons_params(rec.constr, options);
  identify_cons_neg_params(rec.constr, options);
  add_cons_tag_store(rec.constr, rec.cons_idx);
  auto it = rec.py_fields.cbegin(), end = rec.py_fields.cend();
  for (const Field& field : rec.constr.fields) {
    if (field.constraint) {
      add_constraint_check(rec.constr, field, options);
      continue;
    }
    if (!field.implicit) {
      assert(it < end && it->orig_idx == field.field_idx);
      generate_pack_field(*it++, rec.constr, field, options);
    } else {
      if (it < end && it->orig_idx == field.field_idx) {
        ++it;
      }
      compute_implicit_field(rec.constr, field, options);
    }
  }
  assert(it == end);
  add_remaining_param_constraints_check(rec.constr, options);
  output_actions(os, "\n  ", options | 4);
  clear_context();
  os << "\n}\n";
}

void PyTypeCode::clear_context() {
  actions.clear();
  incomplete = 0;
  tmp_ints = 0;
  needs_tmp_cell = false;
  tmp_vars.clear();
  field_vars.clear();
  field_var_set.clear();
  param_var_set.clear();
  param_constraint_used.clear();
  tmp_cpp_ids.clear();
  tmp_cpp_ids.new_ident("cs");
  tmp_cpp_ids.new_ident("cb");
  tmp_cpp_ids.new_ident("cell_ref");
  tmp_cpp_ids.new_ident("t");
}

void PyTypeCode::generate_skip_field(const Constructor& constr, const Field& field, int options) {
  const TypeExpr* expr = field.type;
  MinMaxSize sz = expr->compute_size();
  bool any_bits = expr->compute_any_bits();
  bool validating = (options & 1);
  // std::cerr << "field `" << field.get_name() << "` size is " << sz << "; fixed=" << sz.is_fixed() << "; any=" << any_bits << std::endl;
  if (field.used || (validating && expr->is_nat_subtype && !any_bits)) {
    // an explicit field of type # or ## which is used later or its value is not arbitrary
    // (must load the value into an integer variable and check)
    assert(expr->is_nat_subtype && "cannot use fields of non-`#` type");
    add_fetch_nat_field(constr, field, options);
    return;
  }
  if (sz.is_fixed() && (!validating || (!(sz.min_size() & 0xff) && any_bits))) {
    // field has fixed size, and either its bits can have arbitrary values (and it has no references)
    // ... or we are not validating
    // simply skip the necessary amount of bits
    // NB: if the field is a reference, and we are not validating, we arrive here
    actions += PyAction{(int)sz.convert_min_size()};
    return;
  }
  if (expr->negated) {
    // the field type has some "negative" parameters, which will be computed while checking this field
    // must invoke the correct validate_skip or skip method for the type in question
    std::ostringstream ss;
    if (!is_self(expr, constr)) {
      output_cpp_expr(ss, expr, 100, true);
      ss << '.';
    }
    ss << (validating ? "validate_skip(ops, cs, weak" : "skip(cs");
    output_negative_type_arguments(ss, expr);
    ss << ")";
    actions += PyAction{std::move(ss)};
    add_postponed_equate_actions();
    return;
  }
  // at this point, if the field type is a reference, we must be validating
  if (expr->tp == TypeExpr::te_Ref && expr->args[0]->tp == TypeExpr::te_Apply &&
      (expr->args[0]->type_applied == Cell_type || expr->args[0]->type_applied == Any_type)) {
    // field type is a reference to a cell with arbitrary contents
    actions += PyAction{0x10000};
    return;
  }
  // remaining case: general positive type expression
  std::ostringstream ss;
  std::string tail;
  while (expr->tp == TypeExpr::te_CondType) {
    // optimization for (chains of) conditional types ( x?type )
    assert(expr->args.size() == 2);
    ss << "(!";
    output_cpp_expr(ss, expr->args[0], 30);
    ss << " || ";
    expr = expr->args[1];
    tail = std::string{")"} + tail;
  }
  if ((!validating || any_bits) && can_compute_sizeof(expr)) {
    // field size can be computed at run-time, and either the contents is arbitrary, or we are not validating
    ss << "cs.advance(";
    output_cpp_sizeof_expr(ss, expr, 0);
    ss << ")" << tail;
    actions += PyAction{std::move(ss)};
    return;
  }
  if (expr->tp != TypeExpr::te_Ref) {
    // field type is not a reference, generate a type expression and invoke skip/validate_skip method
    if (!is_self(expr, constr)) {
      output_cpp_expr(ss, expr, 100);
      ss << '.';
    }
    ss << (validating ? "validate_skip(ops, cs, weak)" : "skip(cs)") << tail;
    actions += PyAction{std::move(ss)};
    return;
  }
  // the (remaining) field type is a reference
  if (!validating || (expr->args[0]->tp == TypeExpr::te_Apply &&
                      (expr->args[0]->type_applied == Cell_type || expr->args[0]->type_applied == Any_type))) {
    // the subcase when the field type is either a reference to a cell with arbitrary contents
    // or it is a reference, and we are not validating, so we simply skip the reference
    ss << "cs.advance_refs(1)" << tail;
    actions += PyAction{std::move(ss)};
    return;
  }
  // general reference type, invoke validate_skip_ref()
  // (notice that we are necessarily validating at this point)
  expr = expr->args[0];
  if (!is_self(expr, constr)) {
    output_cpp_expr(ss, expr, 100);
    ss << '.';
  }
  ss << "validate_skip_ref(ops, cs, weak)" << tail;
  actions += PyAction{ss.str()};
}

void PyTypeCode::generate_skip_cons_method(std::ostream& os, std::string nl, int cidx, int options) {
  // TODO HERE
  const Constructor& constr = *(type.constructors.at(cidx));
  init_cons_context(constr);
  identify_cons_params(constr, options);
  identify_cons_neg_params(constr, options);
  add_cons_tag_check(constr, cidx, options);
  for (const Field& field : constr.fields) {
    if (!field.implicit) {
      generate_skip_field(constr, field, options);
    } else if (!field.constraint) {
      compute_implicit_field(constr, field, options);
    } else {
      add_constraint_check(constr, field, options);
    }
  }
  add_remaining_param_constraints_check(constr, options);
  output_actions(os, nl, options);
  clear_context();
}

void PyTypeCode::generate_skip_method(std::ostream& os, int options) {
  int sz = type.size.min_size();
  sz = ((sz & 0xff) << 16) | (sz >> 8);
  auto writed = SizeWriter{sz};
  bool validate = options & 1;
  bool ret_ext = ret_params;

  if (validate && inline_validate_skip) {
    os << "    def validate_skip(self, ops: int, cs: CellSlice, weak: bool = False):\n";
    if (sz) {
      os << "        return cs.advance(" << writed << ")\n\n";
    } else {
      os << "        return True\n\n";
    }
    return;
  } else if (inline_skip) {
    os << "    def skip(self, cs: CellSlice):\n";
    if (sz) {
      os << "        return cs.advance" << (sz < 0x10000 ? "(" : "_ext(") << writed << ")\n\n";
    } else {
      os << "        return True\n\n";
    }
    return;
  }

  os << "    def "
     << (validate ? "validate_skip(self, ops: int, cs: CellSlice, weak: bool = False" : "skip(self, cs: CellSlice");
  if (ret_ext) {
    os << skip_extra_args;
  }
  os << "):\n";
  if (cons_num > 1) {
    os << "    tag = self.get_tag(cs)\n";
    for (int i = 0; i < cons_num; i++) {
      os << "    if tag == " << py_type_class_name << ".Tag." << cons_enum_name[i] << ":";
      generate_skip_cons_method(os, "\n    ", i, options & ~4);
      os << "\n";
    }
    os << "\n    return False\n";
  } else if (cons_num == 1) {
    generate_skip_cons_method(os, "\n  ", 0, options | 4);
    os << "\n";
  } else {
    os << "\n    return False\n";
  }
  os << "\n";
}

unsigned long long PyTypeCode::compute_selector_mask() const {
  unsigned long long z = 0, w = 1;
  int c = 0;
  for (int v : cons_tag_map) {
    if (v > c) {
      c = v;
      z |= w;
    }
    w <<= 1;
  }
  return z;
}

void PyTypeCode::generate_tag_pfx_selector(std::ostream& os, std::string nl, const BinTrie& trie, int d,
                                           int min_size) const {
  LOG(WARNING) << py_type_class_name;
  assert(d >= 0 && d <= 6);
  int n = (1 << d);
  unsigned long long A[64];
  int c[65];
  unsigned long long mask = trie.build_submap(d, A);
  int l = 1;
  c[0] = -1;
  for (int i = 0; i < n; i++) {
    assert(!(A[i] & (A[i] - 1)));
    if ((mask >> l) & 1) {
      c[l++] = A[i] ? td::count_trailing_zeroes_non_zero64(A[i]) : -1;
    }
  }
  bool simple = (l > n / 2);
  if (simple) {
    l = n + 1;
    for (int i = 0; i < n; i++) {
      c[i + 1] = A[i] ? td::count_trailing_zeroes_non_zero64(A[i]) : -1;
    }
  }
  os << nl << "ctab = [";
  for (int i = 0; i < l; i++) {
    if (i > 0) {
      os << ", ";
    }
    if (c[i] < 0) {
      os << "None";
    } else {
      os << cons_enum_name.at(c[i]);
    }
  }
  os << "]\n" << nl << "return ctab[1 + ";
  if (simple) {
    os << "(long long)cs.prefetch_ulong(" << d << ")];";
  } else {
    os << "(long long)cs.bselect" << (d >= min_size ? "(" : "_ext(") << d << ", " << HexConstWriter{mask} << ")];";
  }
}

bool PyTypeCode::generate_get_tag_pfx_distinguisher(std::ostream& os, std::string nl,
                                                    const std::vector<int>& constr_list, bool in_block) const {
  if (constr_list.empty()) {
    os << nl << "  return -1;";
    return false;
  }
  if (constr_list.size() == 1) {
    os << nl << "  return " << cons_enum_name.at(constr_list[0]) << ";";
    return false;
  }
  std::unique_ptr<BinTrie> trie;
  for (int i : constr_list) {
    trie = BinTrie::insert_paths(std::move(trie), type.constructors.at(i)->begins_with, 1ULL << i);
  }
  if (!trie) {
    os << nl << "  return -1;";
    return false;
  }
  int d = trie->compute_useful_depth();
  bool is_pfx_determ = !trie->find_conflict_path();
  assert(is_pfx_determ);
  if (!in_block) {
    os << " {";
  }
  generate_tag_pfx_selector(os, nl, *trie, d, (int)(type.size.min_size() >> 8));
  return !in_block;
}

std::string PyTypeCode::get_nat_param_name(int idx) const {
  for (int i = 0; i < tot_params; i++) {
    if (!type_param_is_neg.at(i) && type_param_is_nat.at(i) && !idx--) {
      return type_param_name.at(i);
    }
  }
  return "???";
}

void PyTypeCode::generate_get_tag_param(std::ostream& os, std::string nl, unsigned long long tag,
                                        unsigned long long tag_params) const {
  if (!tag) {
    os << nl << "return None # ???";
    return;
  }
  if (!(tag & (tag - 1))) {
    os << nl << "return " << py_type_class_name << ".Tag." << cons_enum_name.at(td::count_trailing_zeroes64(tag));
    return;
  }
  int cnt = td::count_bits64(tag);
  DCHECK(cnt >= 2);
  int mdim = 0, mmdim = 0;
  for (int c = 0; c < 64; c++) {
    if ((tag >> c) & 1) {
      int dim = type.constructors.at(c)->admissible_params.dim;
      if (dim > mdim) {
        mmdim = mdim;
        mdim = dim;
      } else if (dim > mmdim) {
        mmdim = dim;
      }
    }
  }
  assert(mmdim > 0);
  for (int p1 = 0; p1 < mmdim; p1++) {
    char A[4];
    std::memset(A, 0, sizeof(A));
    int c;
    for (c = 0; c < 64; c++) {
      if ((tag >> c) & 1) {
        if (!type.constructors[c]->admissible_params.extract1(A, (char)(c + 1), p1)) {
          break;
        }
      }
    }
    if (c == 64) {
      std::string param_name = get_nat_param_name(p1);
      generate_get_tag_param1(os, nl, A, &param_name);
      return;
    }
  }
  for (int p2 = 0; p2 < mmdim; p2++) {
    for (int p1 = 0; p1 < p2; p1++) {
      char A[4][4];
      std::memset(A, 0, sizeof(A));
      int c;
      for (c = 0; c < 64; c++) {
        if ((tag >> c) & 1) {
          if (!type.constructors[c]->admissible_params.extract2(A, (char)(c + 1), p1, p2)) {
            break;
          }
        }
      }
      if (c == 64) {
        std::string param_names[2];
        param_names[0] = get_nat_param_name(p1);
        param_names[1] = get_nat_param_name(p2);
        generate_get_tag_param2(os, nl, A, param_names);
        return;
      }
    }
  }
  for (int p3 = 0; p3 < mmdim; p3++) {
    for (int p2 = 0; p2 < p3; p2++) {
      for (int p1 = 0; p1 < p2; p1++) {
        char A[4][4][4];
        std::memset(A, 0, sizeof(A));
        int c;
        for (c = 0; c < 64; c++) {
          if ((tag >> c) & 1) {
            if (!type.constructors[c]->admissible_params.extract3(A, (char)(c + 1), p1, p2, p3)) {
              break;
            }
          }
        }
        if (c == 64) {
          std::string param_names[3];
          param_names[0] = get_nat_param_name(p1);
          param_names[1] = get_nat_param_name(p2);
          param_names[2] = get_nat_param_name(p3);
          generate_get_tag_param3(os, nl, A, param_names);
          return;
        }
      }
    }
  }
  os << nl << "# ??? cannot distinguish constructors for this type using up to three parameters\n";
  throw src::Fatal{std::string{"cannot generate `"} + py_type_class_name + "::get_tag()` method for type `" +
                   type.get_name() + "`"};
}

bool PyTypeCode::match_param_pattern(std::ostream& os, std::string nl, const char A[4], int mask, std::string pattern,
                                     std::string param_name) const {
  int v = 0, w = 0;
  for (int i = 0; i < 4; i++) {
    if (A[i]) {
      if ((mask >> i) & 1) {
        v = (v && v != A[i] ? -1 : A[i]);
      } else {
        w = (w && w != A[i] ? -1 : A[i]);
      }
    }
  }
  if (v <= 0 || w <= 0) {
    return false;
  }
  os << nl << "return ";
  os << py_type_class_name << ".Tag." << cons_enum_name.at(v - 1) << " if ";

  for (char c : pattern) {
    if (c != '#') {
      os << c;
    } else {
      os << "self." << param_name;
    }
  }
  os << " else " << py_type_class_name << ".Tag." << cons_enum_name.at(w - 1) << "\n\n";
  return true;
}

void PyTypeCode::generate_get_tag_param1(std::ostream& os, std::string nl, const char A[4],
                                         const std::string param_names[1]) const {
  os << nl << "# distinguish by parameter `" << param_names[0] << "` using";
  for (int i = 0; i < 4; i++) {
    os << ' ' << (int)A[i];
  }
  os << "\n";
  if (match_param_pattern(os, nl, A, 14, "#", param_names[0]) ||
      match_param_pattern(os, nl, A, 2, "# == 1", param_names[0]) ||
      match_param_pattern(os, nl, A, 3, "# <= 1", param_names[0]) ||
      match_param_pattern(os, nl, A, 10, "(# & 1)", param_names[0]) ||
      match_param_pattern(os, nl, A, 4, "# && !(# & 1)", param_names[0]) ||
      match_param_pattern(os, nl, A, 8, "# > 1 && (# & 1)", param_names[0])) {
    return;
  }
  os << nl << "# static inline size_t nat_abs(int x) { return (x > 1) * 2 + (x & 1);\n";
  os << nl << "ctab = [";
  for (int i = 0; i < 4; i++) {
    if (i > 0) {
      os << ", ";
    }
    os << (A[i] ? py_type_class_name + ".Tag." + cons_enum_name.at(A[i] - 1) : "None");
  }
  os << " ]\n" << nl << "return ctab[nat_abs(self." << param_names[0] << ")]\n";
}

void PyTypeCode::generate_get_tag_param2(std::ostream& os, std::string nl, const char A[4][4],
                                         const std::string param_names[2]) const {
  // TODO: double check
  //
  // a$_ = A 0 0;
  // b$_ = A 0 1;
  // e$_ {n:#} {m:#} a:(A n m) = A (n + 1) (m + 1);
  os << nl << "# distinguish by parameters `" << param_names[0] << "`, `" << param_names[1] << "` using";
  for (int i = 0; i < 4; i++) {
    for (int j = 0; j < 4; j++) {
      os << ' ' << (int)A[i][j];
    }
  }
  os << "\n";
  os << nl << "# static inline size_t nat_abs(int x) { return (x > 1) * 2 + (x & 1); }\n";
  os << nl << "ctab = [";
  for (int i = 0; i < 16; i++) {
    if (i > 0) {
      os << ", ";
    }
    int v = A[i >> 2][i & 3];
    os << (v ? py_type_class_name + ".Tag." + cons_enum_name.at(v - 1) : "None");
  }
  os << "]\n" << nl << "return ctab[nat_abs(self." << param_names[0] << ") * nat_abs(self." << param_names[1] << ")]\n";
}

void PyTypeCode::generate_get_tag_param3(std::ostream& os, std::string nl, const char A[4][4][4],
                                         const std::string param_names[3]) const {
  // Todo: double check
  // a$_ = A 0 0 0;
  // b$_ = A 0 1 0;
  // c$_ {n:#} {m:#} a:(A 0 n m) = A 0 (n + 1) (m + 1);
  // e$_ {n:#} {m:#} {e:#} a:(A n m e) = A (n + 1) (m + 1) (e + 1);

  os << nl << "# distinguish by parameters `" << param_names[0] << "`, `" << param_names[1] << "`, `" << param_names[2]
     << "` using";
  for (int i = 0; i < 4; i++) {
    for (int j = 0; j < 4; j++) {
      for (int k = 0; k < 4; k++) {
        os << ' ' << (int)A[i][j][k];
      }
    }
  }
  os << "\n";
  os << nl << "# static inline size_t nat_abs(int x) { return (x > 1) * 2 + (x & 1); }\n";
  os << nl << "ctab = [ ";
  for (int i = 0; i < 64; i++) {
    if (i > 0) {
      os << ", ";
    }
    int v = A[i >> 4][(i >> 2) & 3][i & 3];
    os << (v ? py_type_class_name + ".Tag." + cons_enum_name.at(v - 1) : "None");
  }
  os << " ]\n"
     << nl << "return ctab[nat_abs(self." << param_names[0] << ") * nat_abs(self." << param_names[1]
     << ") * nat_abs(self." << param_names[2] << ")]\n";
}

void PyTypeCode::generate_get_tag_subcase(std::ostream& os, std::string nl, const BinTrie* trie, int depth) const {
  if (!trie || !trie->down_tag) {
    os << nl << "return None # ???";
    return;
  }
  if (trie->is_unique()) {
    os << nl << "return " << py_type_class_name << ".Tag." << cons_enum_name.at(trie->unique_value()) << "\n";
    return;
  }
  if (!trie->useful_depth) {
    generate_get_tag_param(os, nl, trie->down_tag);
    return;
  }
  assert(trie->left || trie->right);
  if (!trie->right) {
    generate_get_tag_subcase(os, nl, trie->left.get(), depth + 1);
    return;
  }
  if (!trie->left) {
    generate_get_tag_subcase(os, nl, trie->right.get(), depth + 1);
    return;
  }
  if (trie->left->is_unique() && trie->right->is_unique()) {
    int a = trie->right->unique_value(), b = trie->left->unique_value();

    os << nl << "return ";
    os << (a >= 0 ? py_type_class_name + ".Tag." + cons_enum_name.at(a) : "None") << " if ";
    os << "cs.bit_at(" << depth << ") else ";
    os << (b >= 0 ? py_type_class_name + ".Tag." + cons_enum_name.at(b) : "None") << "\n";
    return;
  }
  os << nl << "if cs.bit_at(" << depth << "):\n";
  generate_get_tag_subcase(os, nl + "    ", trie->right.get(), depth + 1);
  os << nl << "else:\n";
  generate_get_tag_subcase(os, nl + "    ", trie->left.get(), depth + 1);
  os << "\n";
}

void PyTypeCode::generate_get_tag_body(std::ostream& os, std::string nl) {
  int d = type.useful_depth;
  cons_tag_exact.resize(cons_num, false);
  if (type.is_pfx_determ) {
    if (!cons_num) {
      os << nl << "return None";  // When?
      return;
    }
    if (!d) {
      assert(simple_cons_tags && cons_num == 1);
      cons_tag_exact[0] = !(type.constructors.at(0)->tag_bits);
      os << nl << "return " << py_type_class_name << ".Tag(0)";
      return;
    }
    int min_size = (int)(type.size.min_size() >> 8);
    bool always_has = (d <= min_size);
    if (d <= 6 && simple_cons_tags) {
      unsigned long long sm = compute_selector_mask();
      if (always_has && sm + 1 == (2ULL << ((1 << d) - 1))) {
        for (int i = 0; i < cons_num; i++) {
          cons_tag_exact[i] = (type.constructors.at(i)->tag_bits <= d);
        }
        os << nl << "return " << py_type_class_name << ".Tag(int(cs.preload_uint(" << d << ")))";
        return;
      }
      for (int i = 0; i < cons_num; i++) {
        unsigned long long tag = type.constructors.at(i)->tag;
        int l = 63 - td::count_trailing_zeroes_non_zero64(tag);
        if (l <= d) {
          int a = (int)((tag & (tag - 1)) >> (64 - d));
          int b = a + (1 << (d - l));
          cons_tag_exact[i] = ((sm >> a) & 1) && (b == (1 << d) || ((sm >> b) & 1));
        }
      }
      auto w = HexConstWriter{sm};
      os << nl << "return " << py_type_class_name << ".Tag(int(cs.bselect" << (always_has ? "(" : "_ext(") << d << ", ";
      w.write(os, false);
      os << ")))";

      return;
    }
    if (d <= 6) {
      generate_tag_pfx_selector(os, nl, *(type.cs_trie), d, min_size);  // TODO: WHEN?
      return;
    }
  }
  if (type.is_const_param_determ || type.is_const_param_pfx_determ) {
    // a$_ = A 0;
    // b$_ = A 1;

    int p = type.const_param_idx;
    assert(p >= 0);
    std::vector<int> param_values = type.get_all_param_values(p);
    assert(param_values.size() > 1 && param_values.at(0) >= 0);
    os << nl << "tag = self." << type_param_name.at(p) << "\n\n";
    for (int pv : param_values) {
      assert(pv >= 0);
      os << nl << "if tag == " << pv << ":\n";
      std::vector<int> constr_list = type.get_constr_by_param_value(p, pv);
      assert(!constr_list.empty());
      if (constr_list.size() == 1) {
        os << nl << "    return " << py_type_class_name << ".Tag." << cons_enum_name.at(constr_list[0]) << "\n\n";
        continue;
      }
      bool opbr = generate_get_tag_pfx_distinguisher(os, nl + "  ", constr_list, false);
      if (opbr) {
        os << nl << "\n";
      }
    }
    os << nl << "return None";
    return;
  }
  if (d) {
    int d1 = std::min(6, d);
    int n = (1 << d1);
    bool always_has = (d1 <= (int)(type.size.min_size() >> 8));
    unsigned long long A[64], B[64];
    unsigned long long mask = type.cs_trie->build_submap(d1, A);
    int l = td::count_bits64(mask);
    bool simple = (l > n / 2 || n <= 8);
    if (!simple) {
      int j = 0;
      for (int i = 0; i < n; i++) {
        if ((mask >> i) & 1) {
          //std::cerr << i << ',' << std::hex << A[i] << std::dec << std::endl;
          B[j] = (2 * i + 1ULL) << (63 - d1);
          A[j++] = A[i];
        }
      }
      assert(j == l);
    } else {
      for (int i = 0; i < n; i++) {
        B[i] = (2 * i + 1ULL) << (63 - d1);
      }
      l = n;
    }

    if (simple) {
      os << nl << "tag = int(cs.preload_uint(" << d1 << "))\n\n";
    } else {
      os << nl << "tag = int(cs.bselect" << (always_has ? "(" : "_ext(") << d1 << ", ";
      auto w = HexConstWriter{mask};
      w.write(os, false);
      os << "))\n";
    }

    for (int i = 0; i < l; i++) {
      if (A[i] != 0) {
        if ((long long)A[i] > 0) {
          int j;
          for (j = 0; j < i; j++) {
            if (A[j] == A[i]) {
              break;
            }
          }
          if (j < i) {
            continue;
          }
        }
        os << nl << "if tag == " << i << ":\n";
        if ((long long)A[i] > 0) {
          int j;
          for (j = i + 1; j < l; j++) {
            if (A[j] == A[i]) {
              os << "  if tag == " << j << ":\n";
            }
          }
          if (!(A[i] & (A[i] - 1))) {
            os << nl << "    return " << py_type_class_name << ".Tag."
               << cons_enum_name.at(td::count_trailing_zeroes_non_zero64(A[i])) << "\n\n";
          }
        } else {
          generate_get_tag_subcase(os, nl + "    ", type.cs_trie->lookup_node_const(B[i]), d1);
        }
      }
    }
    os << nl << "return None\n";
  } else {
    generate_get_tag_subcase(os, nl, type.cs_trie.get(), 0);
  }
}

void PyTypeCode::generate_ext_fetch_to(std::ostream& os, int options) {
  std::string validate = (options & 1) ? "validate_" : "";
  os << "\nbool " << py_type_class_name << "::" << validate << "fetch_to(vm::CellSlice& cs, Ref<vm::CellSlice>& res"
     << skip_extra_args << ") const {\n"
     << "  res = Ref<vm::CellSlice>{true, cs};\n"
     << "  return " << validate << "skip(cs" << skip_extra_args_pass << ") && res.unique_write().cut_tail(cs);\n"
     << "}\n";
}

void PyTypeCode::generate_fetch_enum_method(std::ostream& os, int options) {
  int minl = type.size.convert_min_size(), maxl = type.size.convert_max_size();
  bool exact = type.cons_all_exact();
  std::string ctag = incremental_cons_tags ? "expected_tag" : "self.cons_tag[expected_tag]";
  os << "\n    def fetch_enum(self, cs: CellSlice) -> int:\n";
  if (!cons_num) {
    os << "        return -1\n";  // When?
  } else if (!maxl) {
    os << "        return 0\n";
  } else if (cons_num == 1) {
    const Constructor& constr = *type.constructors.at(0);
    HexConstWriter w{constr.tag >> (64 - constr.tag_bits)};
    std::stringstream x;
    w.write(x, false);

    os << "        value = cs.load_uint(" << minl << ")\n"
       << "        assert value == " << x.str() << ", 'Not valid tag fetched'\n"
       << "        return value\n";
  } else if (minl == maxl) {
    if (exact) {
      os << "        value = cs.load_uint(" << minl << ");\n"
         << "        assert value in self.cons_tag, f'Unexpected value {value} for tag, expected one of: "
            "{self.cons_tag}'\n"
         << "        return value\n";
    } else {
      os << "        expected_tag = self.get_tag(cs).value\n"
         << "        value = cs.load_uint(" << minl << ")\n"
         << "        assert value == " << ctag << ", f'Not valid tag fetched, got {value}, expected {" << ctag << "}'\n"
         << "        return value\n";
    }
  } else if (exact) {
    os << "        expected_tag = get_tag(cs).value;\n"
       << "        cs.advance(self.cons_len[expected_tag])"
       << "        return expected_tag\n";
  } else {
    os << "        expected_tag = self.get_tag(cs).value\n"
       << "        value = cs.load_uint(self.cons_len[expected_tag])\n"
       << "        assert value == self.cons_tag[expected_tag], f'Not valid tag fetched, got {value}, expected "
          "{self.cons_tag[expected_tag]}'\n"
       << "        return value\n";
  }
  os << "\n";
}

void PyTypeCode::generate_store_enum_method(std::ostream& os, int options) {
  int minl = type.size.convert_min_size(), maxl = type.size.convert_max_size();
  bool exact = type.cons_all_exact();
  std::string ctag = incremental_cons_tags ? "value" : "self.cons_tag[value]";
  os << "\n    def store_enum_from(self, cb: CellBuilder, value: int = None) -> bool:\n";
  if (!cons_num) {
    os << "        return False\n";
  } else if (!maxl) {
    os << "        return True\n";
  } else if (cons_num == 1) {
    const Constructor& constr = *type.constructors.at(0);
    HexConstWriter w{constr.tag >> (64 - constr.tag_bits)};
    std::ostringstream s;
    w.write(s, false);

    os << "        cb.store_uint(" << s.str() << ", " << minl << ")\n"
       << "        return True\n";
  } else if (minl == maxl) {
    if (exact) {
      os << "        assert value is not None, 'You must provide enum to store'\n"
         << "        cb.store_uint(value, " << minl << ")\n"
         << "        return True\n";
    } else if (incremental_cons_tags && cons_num > (1 << (minl - 1))) {
      os << "        assert value is not None, 'You must provide enum position'\n"
         << "        cb.store_uint_less(" << cons_num << ", value)\n"
         << "        return True";
    } else {
      os << "        assert value is not None and value < " << cons_num
         << ", f'Value {value} must be < then {cont_num}'\n"
         << "        cb.store_uint(" << ctag << ", " << minl << ")\n"
         << "        return True\n";
    }
  } else {
    os << "        assert value is not None and value < " << cons_num
       << ", f'Value {value} must be < then {cont_num}'\n"
       << "        cb.store_uint(" << ctag << ", self.cons_len[value])\n"
       << "        return True\n";
  }
  os << "\n";
}

void PyTypeCode::generate_tag_to_class(std::ostream& os, int options) {
  os << "\n        self.tag_to_class = {";

  for (int i = 0; i < cons_num; i++) {
    auto rec = records.at(i);
    auto tag = cons_enum_name.at(i);

    os << py_type_class_name << ".Tag." << tag << ": " << py_type_class_name << "." << rec.py_name;

    if (i < cons_num - 1) {
      os << ", ";
    }
  }
  os << "}\n";
}

// This is actually not header in Python, but just base class with static methods
void PyTypeCode::generate_class(std::ostream& os, int options) {
  //  dump_all_types()

  os << "\nclass " << py_type_class_name << "(TLBComplex):\n";
  generate_cons_enum(os);
  generate_cons_tag_info(os, "    ", 1);

  if (params) {
    generate_type_fields(os, options);
  }

  generate_type_constructor(os, options);

  os << "    def get_tag(self, cs: CellSlice) -> Optional[\"" << py_type_class_name << ".Tag\"]:\n";
  generate_get_tag_body(os, "        ");
  os << "\n\n";

  //  generate_skip_method(os, options);      // skip
  //  generate_skip_method(os, options + 1);  // validate_skip

  //  if (ret_params) {
  //    generate_skip_method(os, options + 3); // TODO
  //    generate_ext_fetch_to(os, options); // TODO
  //  }
  if (type.is_simple_enum) {
    generate_fetch_enum_method(os, options);
    generate_store_enum_method(os, options);
  }

  for (int i = 0; i < cons_num; i++) {
    auto rec = records.at(i);
    rec.declare_record(os, "    ", options);
    generate_unpack_method(os, rec, 2);
    //    generate_unpack_method(os, rec, 10); TODO: is this needed?
    generate_unpack_method(os, rec, 18);
    //    generate_unpack_method(os, rec, 26);  TODO: is this needed?

    //      generate_pack_method(os, rec, 2);
    //      generate_pack_method(os, rec, 10);
    //      generate_pack_method(os, rec, 18);
    //      generate_pack_method(os, rec, 26);
  }
  os << "\n";

  if (type.is_special) {
    os << "    def always_special(self):\n";
    os << "        return True\n\n";
  } else {
    os << "    def always_special(self):\n";
    os << "        return False\n\n";
  }
  //  int sz = type.size.min_size();
  //  sz = ((sz & 0xff) << 16) | (sz >> 8);
  //  if (simple_get_size) {
  //    os << "    @staticmethod\n";
  //    os << "    def get_size(cs: CellSlice):\n";
  //    os << "        return " << SizeWriter{sz} << "\n\n";
  //  }
  //

  os << "\ntlb_classes.append(\"" << py_type_class_name << "\")\n";
}

void PyTypeCode::generate_constant(std::ostream& os) {
  if (!py_type_var_name.empty()) {
    os << "TLBComplex.constants[\"" << py_type_var_name << "\"] = " << py_type_class_name << "()\n";
  }
}

void generate_py_output_to(const std::string& filename, int options = 0) {
  std::stringstream ss;
  generate_py_output_to(ss, options);
  auto new_content = ss.str();
  auto r_old_content = td::read_file_str(filename);
  if (r_old_content.is_ok() && r_old_content.ok() == new_content) {
    return;
  }
  std::ofstream os{filename};
  if (!os) {
    throw src::Fatal{std::string{"cannot create output file `"} + filename + "`"};
  }
  os << new_content;
}

void generate_py_output(const std::string& filename = "", int options = 0) {
  if (filename.empty()) {
    generate_py_output_to(std::cout, options);
  } else {
    generate_py_output_to(filename + ".py", options);
  }
}

void generate_py_output(std::stringstream& ss, int options) {
  generate_py_output_to(ss, options);
}

}  // namespace tlbc