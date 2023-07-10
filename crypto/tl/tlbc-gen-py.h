#pragma once
#include "tlbc-gen-cpp.h"
#include "tlbc-data.h"

namespace tlbc {

extern std::set<std::string> forbidden_py_idents, local_forbidden_py_idents;

enum py_val_type {
  py_unknown,
  py_void = 1,
  py_slice = 2,
  py_cell = 3,
  py_typeref = 4,
  py_typeptr = 5,
  py_bits = 6,
  py_bitstring = 7,
  py_integer = 8,
  py_bool = 10,
  py_enum = 11,
  py_int32 = 12,
  py_uint32 = 13,
  py_int64 = 14,
  py_uint64 = 15,
  py_subrecord = 16
};

struct PyValType {
  py_val_type vt;
  int size;
  PyValType(py_val_type _vt = py_unknown, int _size = -1) : vt(_vt), size(_size) {
  }
  py_val_type get() const {
    return vt;
  }
  void show(std::ostream& os, bool pass_value = false) const;
  bool needs_move() const;
};

extern std::ostream& operator<<(std::ostream& os, PyValType cvt);

class PyTypeCode {
  Type& type;
  bool ok;
  CppIdentSet local_cpp_ids;
  bool builtin;
  bool inline_get_tag;
  bool inline_skip;
  bool inline_validate_skip;
  bool simple_get_size;
  bool simple_cons_tags;
  bool incremental_cons_tags;

 public:
  struct ConsRecord;

  struct ConsField {
    const Field& field;
    const ConsRecord* subrec;
    std::string name;
    py_val_type pytype;
    int size;
    int orig_idx;
    bool implicit;
    ConsField(const Field& _field, std::string _name, py_val_type _pytype, int _size, int _idx,
              const ConsRecord* _subrec = nullptr, bool _implicit = false)
        : field(_field)
        , subrec(_subrec)
        , name(_name)
        , pytype(_pytype)
        , size(_size)
        , orig_idx(_idx)
        , implicit(_implicit) {
      assert(pytype != py_subrecord || subrec);
    }
    PyValType get_cvt() const {
      return {pytype, size};
    }
    void print_type(std::ostream& os, bool pass_value = false) const;
  };

  struct ConsRecord {
    const PyTypeCode& cpp_type;
    const Constructor& constr;
    int cons_idx;
    bool is_trivial;
    bool is_small;
    bool triv_conflict;
    bool has_trivial_name;
    bool inline_record;
    bool declared;
    py_val_type equiv_cpp_type;
    std::vector<py_val_type> equiv_cpp_types;
    std::string py_name;
    std::vector<ConsField> py_fields;
    ConsRecord(const PyTypeCode& _cpp_type, const Constructor& _constr, int idx, bool _triv = false)
        : cpp_type(_cpp_type), constr(_constr), cons_idx(idx), is_trivial(_triv), declared(false) {
    }
    bool recover_idents(CppIdentSet& idents) const;
    void declare_record(std::ostream& os, std::string nl, int options);
    bool declare_record_unpack(std::ostream& os, std::string nl, int options);
    bool declare_record_pack(std::ostream& os, std::string nl, int options);
    void print_full_name(std::ostream& os) const;
  };
  std::vector<ConsRecord> records;

  int params;
  int tot_params;
  int ret_params;
  int cons_num;
  int common_cons_len;
  std::vector<std::string> cons_enum_name;
  std::vector<int> cons_enum_value;
  std::vector<int> cons_tag_map;
  std::vector<bool> cons_tag_exact;
  std::vector<int> cons_idx_by_enum;
  std::string py_type_var_name;
  std::string py_type_class_name;
  void generate(std::ostream& os, int options = 0);
  bool is_codegened(){
    return type.already_codegened;
  }

 private:
  std::vector<Action> actions;

  void clear_context();
  void init_cons_context(const Constructor& constr);

  int incomplete;
  int tmp_ints;
  bool needs_tmp_cell;
  std::vector<std::string> tmp_vars;
  std::vector<std::string> field_vars;
  std::vector<bool> field_var_set;
  std::vector<bool> param_var_set;
  std::vector<std::pair<std::string, const TypeExpr*>> postponed_equate;
  std::vector<bool> param_constraint_used;
  std::vector<std::string> type_param_name;
  std::vector<bool> type_param_is_nat;
  std::vector<bool> type_param_is_neg;
  std::string skip_extra_args;
  std::string skip_extra_args_pass;
  CppIdentSet tmp_cpp_ids;
  void assign_class_name();
  std::string new_tmp_var(std::string hint);
  std::string new_tmp_var();
  void assign_cons_names();
  void assign_class_field_names();
  void assign_cons_values();
  void assign_record_cons_names();
  bool compute_simple_cons_tags();
  bool check_incremental_cons_tags() const;
  void add_postponed_equate_actions();
  bool can_compute_sizeof(const TypeExpr* expr) const;
  void generate_cons_enum(std::ostream& os);
  void generate_cons_len_array(std::ostream& os, std::string nl, int options = 0);
  void output_negative_type_arguments(std::ostream& os, const TypeExpr* expr);
  void generate_cons_tag_array(std::ostream& os, std::string nl, int options = 0);
  void generate_cons_tag_info(std::ostream& os, std::string nl, int options = 0);
  void generate_type_constructor(std::ostream& os, int options);
  void generate_type_fields(std::ostream& os, int options);
  void output_actions(std::ostream& os, std::string nl, int options);
  void add_cons_tag_store(const Constructor& constr, int cidx);
  void output_cpp_expr(std::ostream& os, const TypeExpr* expr, int prio = 0, bool allow_type_neg = false) const;
  std::string add_fetch_nat_field(const Constructor& constr, const Field& field, int options);
  void output_fetch_subrecord(std::ostream& os, std::string field_name, const ConsRecord* subrec);
  bool is_self(const TypeExpr* expr, const Constructor& constr) const;
  bool can_compute(const TypeExpr* expr) const;
  void add_store_subrecord(std::string field_name, const ConsRecord* subrec);
  void output_store_field(std::ostream& os, std::string field_name, const TypeExpr* expr, py_val_type cvt);
  void generate_ext_fetch_to(std::ostream& os, int options);
  unsigned long long compute_selector_mask() const;
  void generate_skip_method(std::ostream& os, int options = 0);
  void generate_fetch_enum_method(std::ostream& os, int options);
  void generate_skip_cons_method(std::ostream& os, std::string nl, int cidx, int options);
  void generate_skip_field(const Constructor& constr, const Field& field, int options);
  void generate_get_tag_body(std::ostream& os, std::string nl);
  bool can_use_to_compute(const TypeExpr* expr, int i) const;
  void output_cpp_sizeof_expr(std::ostream& os, const TypeExpr* expr, int prio) const;
  void output_fetch_field(std::ostream& os, std::string field_name, const TypeExpr* expr, py_val_type cvt);
  void add_remaining_param_constraints_check(const Constructor& constr, int options);
  void generate_store_enum_method(std::ostream& os, int options);
  std::string get_nat_param_name(int idx) const;
  void generate_pack_method(std::ostream& os, ConsRecord& rec, int options);
  void generate_unpack_method(std::ostream& os, PyTypeCode::ConsRecord& rec, int options);
  void bind_record_fields(const ConsRecord& rec, int options);
  void identify_cons_params(const Constructor& constr, int options);
  void identify_cons_neg_params(const Constructor& constr, int options);
  void add_compute_actions(const TypeExpr* expr, int i, std::string bind_to);
  void add_store_nat_field(const Constructor& constr, const Field& field, int options);
  void add_cons_tag_check(const Constructor& constr, int cidx, int options);
  void generate_unpack_field(const ConsField& fi, const Constructor& constr, const Field& field, int options);
  void compute_implicit_field(const Constructor& constr, const Field& field, int options);
  bool add_constraint_check(const Constructor& constr, const Field& field, int options);
  void generate_pack_field(const ConsField& fi, const Constructor& constr, const Field& field, int options);
  void generate_tag_pfx_selector(std::ostream& os, std::string nl, const BinTrie& trie, int d, int min_size) const;
  void generate_get_tag_subcase(std::ostream& os, std::string nl, const BinTrie* trie, int depth) const;
  bool generate_get_tag_pfx_distinguisher(std::ostream& os, std::string nl, const std::vector<int>& constr_list,
                                          bool in_block) const;
  void generate_get_tag_param(std::ostream& os, std::string nl, unsigned long long tag,
                              unsigned long long params = std::numeric_limits<td::uint64>::max()) const;
  void generate_get_tag_param1(std::ostream& os, std::string nl, const char A[4],
                               const std::string param_names[1]) const;
  void generate_get_tag_param2(std::ostream& os, std::string nl, const char A[4][4],
                               const std::string param_names[2]) const;
  void generate_get_tag_param3(std::ostream& os, std::string nl, const char A[4][4][4],
                               const std::string param_names[3]) const;
  bool match_param_pattern(std::ostream& os, std::string nl, const char A[4], int mask, std::string pattern,
                           std::string param_name) const;
  bool init();
  void generate_header(std::ostream& os, int options = 0);

 public:
  PyTypeCode(Type& _type) : type(_type), local_cpp_ids(&local_forbidden_py_idents) {
    ok = init();
  }
  bool is_ok() const {
    return ok;
  }
};

void generate_py_output(std::stringstream& ss, int options);

}  // namespace tlbc