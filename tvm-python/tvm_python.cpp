#include "pybind11/pybind11.h"
#include <string>

#include "vm/vm.h"
#include "vm/vmstate.h"
#include "td/utils/base64.h"
#include <utility>
#include "vm/boc.h"
#include "vm/cellslice.h"
#include "vm/cp0.h"
#include "pybind11/stl.h"
#include "block/block.h"
#include "block/block-parse.h"
#include "td/utils/crypto.h"
#include "crypto/fift/Fift.h"
#include "crypto/fift/words.h"

namespace py = pybind11;
using namespace pybind11::literals;  // to bring in the `_a` literal

unsigned method_name_to_id(const std::string& method_name) {
  unsigned crc = td::crc16(method_name);
  const unsigned method_id = (crc & 0xffff) | 0x10000;
  return method_id;
}

// this can be slow :(
std::string dump_as_boc(td::Ref<vm::Cell> root_cell) {
  auto s = td::base64_encode(std_boc_serialize(std::move(root_cell), 31).move_as_ok());
  return s;
}

// type converting utils
td::Ref<vm::Cell> parseStringToCell(const std::string& base64string) {
  auto base64decoded = td::base64_decode(td::Slice(base64string));

  if (base64decoded.is_error()) {
    throw std::invalid_argument("Parse code error: invalid base64");
  }

  auto boc_decoded = vm::std_boc_deserialize(base64decoded.move_as_ok());

  if (boc_decoded.is_error()) {
    throw std::invalid_argument("Parse code error: invalid BOC");
  }

  return boc_decoded.move_as_ok();
}
vm::StackEntry cast_python_item_to_stack_entry(const py::handle item) {
  // TODO: maybe add tuple support?

  py::module builtins = py::module::import("builtins");
  py::object py_int = builtins.attr("int");
  py::object py_str = builtins.attr("str");
  py::object py_dict = builtins.attr("dict");
  py::object py_list = builtins.attr("list");

  if (item.get_type().is(py_int)) {
    std::string asStrValue = item.cast<py::str>().cast<std::string>();
    td::BigIntG<257, td::BigIntInfo> tmp;
    tmp.parse_dec(asStrValue);

    vm::StackEntry tmpEntry;
    tmpEntry.set_int(td::make_refint(tmp));

    return tmpEntry;
  } else if (item.get_type().is(py_list)) {
    std::vector<vm::StackEntry> tmp;

    auto iter = py::iter(item);
    while (iter != py::iterator::sentinel()) {
      auto value = *iter;

      tmp.push_back(cast_python_item_to_stack_entry(value));
      ++iter;
    }

    return tmp;
  } else if (item.get_type().is(py_dict)) {
    std::string tvmType;
    std::string tvmValue;
    vm::StackEntry s;

    auto dict = item.cast<py::dict>();

    for (auto dictItem : dict) {
      auto key = dictItem.first.cast<py::str>().cast<std::string>();
      auto value = dictItem.second.cast<py::str>().cast<std::string>();

      if (key == "type") {
        tvmType = value;
      } else if (key == "value") {
        tvmValue = value;
      } else {
        throw std::invalid_argument("Key should be neither type or value");
      }
    };

    if (tvmType == "cell") {
      auto cell = parseStringToCell(tvmValue);
      s = cell;
    } else if (tvmType == "cellSlice") {
      auto cell = parseStringToCell(tvmValue);
      auto slice = vm::load_cell_slice_ref(cell);
      s = slice;
    }

    return s;
  }

  throw std::invalid_argument("Not supported type: " + item.get_type().cast<py::str>().cast<std::string>());
}

py::object cast_stack_item_to_python_object(const vm::StackEntry& item) {
  if (item.is_null() || item.empty()) {
    return py::none();
  } else if (item.is_int()) {
    std::string bigInteger = item.as_int()->to_dec_string();
    auto pyStringInteger = py::str(bigInteger);
    return pyStringInteger.cast<py::int_>();
  } else if (item.is_cell()) {
    py::dict d("type"_a = "cell", "value"_a = py::str(dump_as_boc(item.as_cell())));
    return d;
  }

  // if slice
  auto slice_item = item.as_slice();

  if (slice_item.not_null()) {
    vm::CellBuilder cb;
    cb.append_cellslice(slice_item);
    auto body_cell = cb.finalize();

    py::dict d("type"_a = "cellSlice", "value"_a = py::str(dump_as_boc(body_cell)));
    return d;
  }

  // if tuple
  auto tuple_item = item.as_tuple();

  if (tuple_item.not_null()) {
    auto tuple_size = tuple_item->size();

    std::vector<py::object> pyStack;
    for (unsigned long idx = 0; idx < tuple_size; idx++) {
      pyStack.push_back(cast_stack_item_to_python_object(tuple_item->at(idx)));
    }

    py::list pyStackList = py::cast(pyStack);

    return pyStackList;
  }

  // if continuation
  auto cont_item = item.as_cont();

  if (cont_item.not_null()) {
    vm::CellBuilder cb;
    cont_item->serialize(cb);
    auto body_cell = cb.finalize();

    py::dict d("type"_a = "continuation", "value"_a = py::str(dump_as_boc(body_cell)));
    return d;
  }

  throw std::invalid_argument("Not supported type: " + std::to_string(item.type()));
}

// Vm logger
class PythonLogger : public td::LogInterface {
 public:
  void append(td::CSlice slice) override {
    py::print(slice.str());
  }
};

const int LOG_DEBUG = 2;
const int LOG_INFO = 1;


std::string code_disasseble(const std::string& code) {
  auto codeCell = parseStringToCell(code);

  fift::Fift::Config config;

  config.source_lookup = fift::SourceLookup(std::make_unique<fift::OsFileLoader>());
  config.source_lookup.add_include_path("./");

  fift::init_words_common(config.dictionary);
  fift::init_words_vm(config.dictionary);
  fift::init_words_ton(config.dictionary);

  fift::Fift fift(std::move(config));

  std::stringstream ss;
  std::stringstream output;

  // Fift.fif & Lists.fif & Disasm.fif
  ss << "{ char \" word 1 { swap { abort } if drop } } ::_ abort\"  { { bl word dup \"\" $= abort\"comment extends "
        "after end of file\" \"*/\" $= } until 0 'nop } :: /*  { bl word 1 ' (forget) } :: [forget]  { char \" word "
        "1 ' type } ::_ .\"  { char } word x>B 1 'nop } ::_ B{  { swap ({) over 2+ -roll swap (compile) (}) } : does "
        " { 1 'nop does create } : constant  { 2 'nop does create } : 2constant  { hole constant } : variable  10 "
        "constant ten  { bl word 1 { find 0= abort\"word not found\" } } :: (')  { bl word find not abort\"-?\" 0 "
        "swap } :: [compile]  { bl word 1 {     dup find { \" -?\" $+ abort } ifnot nip execute  } } :: @'  { bl "
        "word 1 { swap 1 'nop does swap 0 (create) }  } :: =:  { bl word 1 { -rot 2 'nop does swap 0 (create) }  } "
        ":: 2=:  { <b swap s, b> } : s>c  { s>c hashB } : shash  { dup 0< ' negate if } : abs  { 2dup > ' swap if } "
        ": minmax  { minmax drop } : min  { minmax nip } : max  \"\" constant <#  ' $reverse : #>  { swap 10 /mod "
        "char 0 + rot swap hold } : #  { { # over 0<= } until } : #s  { 0< { char - hold } if } : sign  { dup 10 < { "
        "48 } { 55 } cond + } : Digit  { dup 10 < { 48 } { 87 } cond + } : digit  { rot swap /mod Digit rot swap "
        "hold } : B#  { rot swap /mod digit rot swap hold } : b#  { 16 B# } : X#  { 16 b# } : x#  { -rot { 2 pick B# "
        "over 0<= } until rot drop } : B#s  { -rot { 2 pick b# over 0<= } until rot drop } : b#s  { 16 B#s } : X#s  "
        "{ 16 b#s } : x#s  variable base  { 10 base ! } : decimal  { 16 base ! } : hex  { 8 base ! } : octal  { 2 "
        "base ! } : binary  { base @ B# } : Base#  { base @ b# } : base#  { base @ B#s } : Base#s  { base @ b#s } : "
        "base#s  { over abs <# rot 1- ' X# swap times X#s rot sign #> nip } : (0X.)  { over abs <# rot 1- ' x# swap "
        "times x#s rot sign #> nip } : (0x.)  { (0X.) type } : 0X._  { 0X._ space } : 0X.  { (0x.) type } : 0x._  { "
        "0x._ space } : 0x.  { bl (-trailing) } : -trailing  { char 0 (-trailing) } : -trailing0  { char \" word 1 ' "
        "$+ } ::_ +\"  { find 0<> dup ' nip if } : (def?)  { bl word 1 ' (def?) } :: def?  { bl word 1 { (def?) not "
        "} } :: undef?  { def? ' skip-to-eof if } : skip-ifdef  { bl word dup (def?) { drop skip-to-eof } { 'nop "
        "swap 0 (create) } cond } : library  { bl word dup (def?) { 2drop skip-to-eof } { swap 1 'nop does swap 0 "
        "(create) } cond } : library-version  { char ) word \"$\" swap $+ 1 { find 0= abort\"undefined parameter\" "
        "execute } } ::_ $(  { sbitrefs rot brembitrefs rot >= -rot <= and } : s-fits?  { swap sbitrefs -rot + rot "
        "brembitrefs -rot <= -rot <= and } : s-fits-with?  { 0 swap ! } : 0!  { tuck @ + swap ! } : +!  { tuck @ "
        "swap - swap ! } : -!  { 1 swap +! } : 1+!  { -1 swap +! } : 1-!  { null swap ! } : null!  { not 2 pick @ "
        "and xor swap ! } : ~!  0 tuple constant nil  { 1 tuple } : single  { 2 tuple } : pair  { 3 tuple } : triple "
        " { 1 untuple } : unsingle  { 2 untuple } : unpair  { 3 untuple } : untriple  { over tuple? { swap count = } "
        "{ 2drop false } cond } : tuple-len?  { 0 tuple-len? } : nil?  { 1 tuple-len? } : single?  { 2 tuple-len? } "
        ": pair?  { 3 tuple-len? } : triple?  { 0 [] } : first  { 1 [] } : second  { 2 [] } : third  ' pair : cons  "
        "' unpair : uncons  { 0 [] } : car  { 1 [] } : cdr  { cdr car } : cadr  { cdr cdr } : cddr  { cdr cdr car } "
        ": caddr  { null ' cons rot times } : list  { -rot pair swap ! } : 2!  { @ unpair } : 2@  { true (atom) drop "
        "} : atom  { bl word atom 1 'nop } ::_ `  { hole dup 1 { @ execute } does create } : recursive  { 0 { 1+ dup "
        "1 ' $() does over (.) \"$\" swap $+ 0 (create) } rot times drop } : :$1..n  { 10 hold } : +cr  { 9 hold } : "
        "+tab  { \"\" swap { 0 word 2dup $cmp } { rot swap $+ +cr swap } while 2drop } : scan-until-word  { 0 word "
        "-trailing scan-until-word 1 'nop } ::_ $<<  { 0x40 runvmx } : runvmcode  { 0x48 runvmx } : gasrunvmcode  { "
        "0xc8 runvmx } : gas2runvmcode  { 0x43 runvmx } : runvmdict  { 0x4b runvmx } : gasrunvmdict  { 0xcb runvmx } "
        ": gas2runvmdict  { 0x45 runvmx } : runvm  { 0x4d runvmx } : gasrunvm  { 0xcd runvmx } : gas2runvm  { 0x55 "
        "runvmx } : runvmctx  { 0x5d runvmx } : gasrunvmctx  { 0xdd runvmx } : gas2runvmctx  { 0x75 runvmx } : "
        "runvmctxact  { 0x7d runvmx } : gasrunvmctxact  { 0xfd runvmx } : gas2runvmctxact  { 0x35 runvmx } : "
        "runvmctxactq  { 0x3d runvmx } : gasrunvmctxactq    { hole dup 1 { @ execute } does create } : recursive  "
        "recursive equal? {    dup tuple? {      over tuple? {        over count over count over = {          0 { "
        "dup 0>= { 2dup [] 3 pick 2 pick [] equal? { 1+ } { drop -1 } cond              } if } rot times          "
        "nip nip 0>=        } { drop 2drop false } cond      } { 2drop false } cond    } { eqv? } cond  } swap !  { "
        "null swap { dup null? not } { uncons swap rot cons swap } while drop } : list-reverse  { { uncons dup null? "
        "{ drop true } { nip false } cond } until } : list-last  recursive list+ {    over null? { nip } { swap "
        "uncons rot list+ cons } cond  } swap !  { { dup null? { drop true true } {    swap dup null? { 2drop false "
        "true } {    uncons swap rot uncons -rot equal? { false } {    2drop false true    } cond } cond } cond } "
        "until  } : list-  { 0 { over null? not } { swap uncons rot 1+ } while nip } : explode-list  { swap "
        "explode-list dup 1+ roll } : explode-list-1  { explode-list tuple } : list>tuple  { null swap rot { -rot "
        "cons swap } swap times } : mklist-1  { \"\" { over null? not } { swap uncons -rot $+ } while nip  } : "
        "concat-string-list  { 0 { over null? not } { swap uncons -rot + } while nip  } : sum-list  { -rot { over "
        "null? not } { swap uncons -rot 3 pick execute } while nip nip  } : foldl  { swap uncons swap rot foldl } : "
        "foldl-ne  recursive foldr {    rot dup null? { 2drop } {      uncons -rot 2swap swap 3 pick foldr rot "
        "execute    } cond  } swap !  recursive foldr-ne {    over cdr null? { drop car } {      swap uncons 2 pick "
        "foldr-ne rot execute    } cond  } swap !  { dup null? { ' list+ foldr-ne } ifnot } : concat-list-lists  { ' "
        "cdr swap times } : list-tail  { list-tail car } : list-ref  { { dup null? { drop true true } {      dup "
        "pair? { cdr false } {      drop false true    } cond } cond } until  } : list?  { 0 { over null? not } { 1+ "
        "swap uncons nip swap } while nip  } : list-length  { swap {    dup null? { nip true } {    tuck car over "
        "execute { drop true } {    swap cdr false    } cond } cond } until  } : list-tail-from  { swap 1 ' eq? does "
        "list-tail-from } : list-member-eq  { swap 1 ' eqv? does list-tail-from } : list-member-eqv  { swap 1 ' "
        "equal? does list-tail-from } : list-member-equal  { list-member-eq null? not } : list-member?  { "
        "list-member-eqv null? not } : list-member-eqv?  { dup null? { drop false } { car true } cond  } : safe-car  "
        "{ dup null? { drop false } { car second true } cond  } : get-first-value  { list-tail-from safe-car } : "
        "assoc-gen  { list-tail-from get-first-value } : assoc-gen-x  { swap 1 { swap first eq? } does assoc-gen } : "
        "assq  { swap 1 { swap first eqv? } does assoc-gen } : assv  { swap 1 { swap first equal? } does assoc-gen } "
        ": assoc  { swap 1 { swap first eq? } does assoc-gen-x } : assq-val  { swap 1 { swap first eqv? } does "
        "assoc-gen-x } : assv-val  { swap 1 { swap first equal? } does assoc-gen-x } : assoc-val  recursive list-map "
        "{    over null? { drop } {    swap uncons -rot over execute -rot list-map cons    } cond  } swap !    "
        "variable ctxdump  variable curctx  { ctxdump @ curctx @ ctxdump 2! curctx 2!    { curctx 2@ over null? not "
        "} { swap uncons rot tuck curctx 2! execute }    while 2drop ctxdump 2@ curctx ! ctxdump !  } : list-foreach "
        " forget ctxdump  forget curctx    variable loopdump  variable curloop  { curloop @ loopdump @ loopdump 2! } "
        ": push-loop-ctx  { loopdump 2@ loopdump ! curloop ! } : pop-loop-ctx  { -rot 2dup > {      push-loop-ctx {  "
        "      triple dup curloop ! first execute curloop @ untriple 1+ 2dup <=      } until pop-loop-ctx    } if "
        "2drop drop  } : for  { -rot 2dup > {      push-loop-ctx {        triple dup curloop ! untriple nip swap "
        "execute curloop @ untriple 1+ 2dup <=      } until pop-loop-ctx    } if 2drop drop  } : for-i  { curloop @ "
        "third } : i  { loopdump @ car third } : j  { loopdump @ cadr third } : k  forget curloop  forget loopdump   "
        " variable ')  'nop box constant ',  { \") without (\" abort } ') !   { ') @ execute } : )  anon constant "
        "dot-marker  { swap    { -rot 2dup eq? not }    { over dot-marker eq? abort\"invalid dotted list\"      swap "
        "rot cons } while 2drop  } : list-tail-until-marker  { null swap list-tail-until-marker } : "
        "list-until-marker  { over dot-marker eq? { nip 2dup eq? abort\"invalid dotted list\" }    { null swap } "
        "cond    list-tail-until-marker  } : list-until-marker-ext  { ') @ ', @ } : ops-get  { ', ! ') ! } : ops-set "
        " { anon dup ops-get 3 { ops-set list-until-marker-ext } does ') ! 'nop ', !  } : (    { 2 { 1+ 2dup pick "
        "eq? } until 3 - nip } : count-to-marker  { count-to-marker tuple nip } : tuple-until-marker  { anon dup "
        "ops-get 3 { ops-set tuple-until-marker } does ') ! 'nop ', ! } : _(    \"()[]'\" 34 hold constant "
        "lisp-delims  { lisp-delims 11 (word) } : lisp-token  { null cons `quote swap cons } : do-quote  { 1 { ', @ "
        "2 { 2 { ', ! execute ', @ execute } does ', ! }        does ', ! } does  } : postpone-prefix  { ', @ 1 { ', "
        "! } does ', ! } : postpone-',  ( `( ' ( pair    `) ' ) pair    `[ ' _( pair    `] ' ) pair    `' ' do-quote "
        "postpone-prefix pair    `. ' dot-marker postpone-prefix pair    `\" { char \" word } pair    `;; { 0 word "
        "drop postpone-', } pair  ) constant lisp-token-dict  variable eol  { eol @ eol 0! anon dup ') @ 'nop 3    { "
        "ops-set list-until-marker-ext true eol ! } does ') ! rot ', !    { lisp-token dup (number) dup { roll drop "
        "} {        drop atom dup lisp-token-dict assq { nip second execute } if      } cond      ', @ execute      "
        "eol @    } until    -rot eol ! execute  } :_ List-generic(  { 'nop 'nop List-generic( } :_ LIST(      "
        "variable 'disasm  { 'disasm @ execute } : disasm   variable @dismode  @dismode 0!  { rot over @ and rot xor "
        "swap ! } : andxor! { -2 0 @dismode andxor! } : stack-disasm { -2 1 @dismode andxor! } : std-disasm  { -3 2 "
        "@dismode andxor! } : show-vm-code  { -3 0 @dismode andxor! } : hide-vm-code  { @dismode @ 1 and 0= } : "
        "stack-disasm?    variable @indent  @indent 0!  { ' space @indent @ 2* times } : .indent  { @indent 1+! } : "
        "+indent  { @indent 1-! } : -indent "
        "  \n { \" \" $pos } : spc-pos { dup \" \" $pos swap \",\" $pos dup 0< { drop } {   over 0< { nip } { min } "
        "cond } cond } : spc-comma-pos \n"
        " { { dup spc-pos 0= } { 1 $| nip } while } : -leading\n"
        "{ -leading -trailing dup spc-pos dup 0< {\n"
        "  drop dup $len { atom single } { drop nil } cond } {\n"
        "    $| swap atom swap -leading 2 { over spc-comma-pos dup 0>= } {\n"
        "      swap 1+ -rot $| 1 $| nip -leading rot\n"
        "    } while drop tuple\n"
        "  } cond\n"
        "} : parse-op \n"
        "{ dup \"s-1\" $= { drop \"s(-1)\" true } {\n"
        "  dup \"s-2\" $= { drop \"s(-2)\" true } {\n"
        "  dup 1 $| swap \"x\" $= { nip \"x{\" swap $+ +\"}\" true } {\n"
        "  2drop false } cond } cond } cond\n"
        "} : adj-op-arg\n"
        "{ over count over <= { drop } { 2dup [] adj-op-arg { swap []= } { drop } cond } cond } : adj-arg[]\n"
        "{ 1 adj-arg[] 2 adj-arg[] 3 adj-arg[]\n"
        "  dup first\n"
        "  dup `XCHG eq? {\n"
        "    drop dup count 2 = { tpop swap \"s0\" , swap , } if } {\n"
        "  dup `LSHIFT eq? {\n"
        "    drop dup count 2 = stack-disasm? and { second `LSHIFT# swap pair } if } {\n"
        "  dup `RSHIFT eq? {\n"
        "    drop dup count 2 = stack-disasm? and { second `RSHIFT# swap pair } if } {\n"
        "  drop\n"
        "  } cond } cond } cond\n"
        "} : adjust-op  \n"
        "\n"
        "variable @cp  @cp 0!\n"
        "variable @curop\n"
        "variable @contX  variable @contY  variable @cdict\n"
        "\n"
        "{ atom>$ type } : .atom\n"
        "{ dup first .atom dup count 1 > { space 0 over count 2- { 1+ 2dup [] type .\", \" } swap times 1+ [] type } "
        "{ drop } cond } : std-show-op\n"
        "{ 0 over count 1- { 1+ 2dup [] type space } swap times drop first .atom } : stk-show-op\n"
        "{ @dismode @ 2 and { .indent .\"// \" @curop @ csr. } if } : .curop? "
        "\n{ .curop? .indent @dismode @ 1 and ' std-show-op ' stk-show-op cond cr\n"
        "} : show-simple-op\n"
        "{ dup 4 u@ 9 = { 8 u@+ swap 15 and 3 << s@ } {\n"
        "  dup 7 u@ 0x47 = { 7 u@+ nip 2 u@+ 7 u@+ -rot 3 << swap sr@ } {\n"
        "  dup 8 u@ 0x8A = { ref@ <s } {\n"
        "  abort\"invalid PUSHCONT\"\n"
        "  } cond } cond } cond\n"
        "} : get-cont-body\n"
        "{ 14 u@+ nip 10 u@+ ref@ dup rot pair swap <s empty? { drop null } if } : get-const-dict\n"
        "{ @contX @ @contY @ @contX ! @contY ! } : scont-swap\n"
        "{ .indent swap type type cr @contY @ @contY null! @contX @ @contX null!\n"
        "  +indent disasm -indent @contY !\n"
        "} : show-cont-bodyx\n"
        "{ \":<{\" show-cont-bodyx .indent .\"}>\" cr } : show-cont-op\n"
        "{ swap scont-swap \":<{\" show-cont-bodyx scont-swap\n"
        "  \"\" show-cont-bodyx .indent .\"}>\" cr } : show-cont2-op\n"
        "\n"
        "{ @contX @ null? { \"CONT\" show-cont-op } ifnot\n"
        "} : flush-contX\n"
        "{ @contY @ null? { scont-swap \"CONT\" show-cont-op scont-swap } ifnot\n"
        "} : flush-contY\n"
        "{ flush-contY flush-contX } : flush-cont\n"
        "{ @contX @ null? not } : have-cont?\n"
        "{ @contY @ null? not } : have-cont2?\n"
        "{ flush-contY @contY ! scont-swap } : save-cont-body\n"
        "\n"
        "{ @cdict ! } : save-const-dict\n"
        "{ @cdict null! } : flush-dict\n"
        "{ @cdict @ null? not } : have-dict?\n"
        "\n"
        "{ flush-cont .indent type .\":<{\" cr\n"
        "  @curop @ ref@ <s +indent disasm -indent .indent .\"}>\" cr\n"
        "} : show-ref-op\n"
        "{ flush-contY .indent rot type .\":<{\" cr\n"
        "  @curop @ ref@ <s @contX @ @contX null! rot ' swap if\n"
        "  +indent disasm -indent .indent swap type cr\n"
        "  +indent disasm -indent .indent .\"}>\" cr\n"
        "} : show-cont-ref-op\n"
        "{ flush-cont .indent swap type .\":<{\" cr\n"
        "  @curop @ ref@+ <s +indent disasm -indent .indent swap type cr\n"
        "  ref@ <s +indent disasm -indent .indent .\"}>\" cr\n"
        "} : show-ref2-op\n"
        "\n"
        "{ flush-cont first atom>$ dup 5 $| drop \"DICTI\" $= swap\n"
        "  .indent type .\" {\" cr +indent @cdict @ @cdict null! unpair\n"
        "  rot {\n"
        "    swap .indent . .\"=> <{\" cr +indent disasm -indent .indent .\"}>\" cr true\n"
        "  } swap ' idictforeach ' dictforeach cond drop\n"
        "  -indent .indent .\"}\" cr\n"
        "} : show-const-dict-op\n"
        "\n"
        "( `PUSHCONT `PUSHREFCONT ) constant @PushContL\n"
        "( `REPEAT `UNTIL `IF `IFNOT `IFJMP `IFNOTJMP ) constant @CmdC1\n"
        "( `IFREF `IFNOTREF `IFJMPREF `IFNOTJMPREF `CALLREF `JMPREF ) constant @CmdR1\n"
        "( `DICTIGETJMP `DICTIGETJMPZ `DICTUGETJMP `DICTUGETJMPZ `DICTIGETEXEC `DICTUGETEXEC ) constant @JmpDictL\n"
        " { dup first `DICTPUSHCONST eq? {\n"
        "    flush-cont @curop @ get-const-dict save-const-dict show-simple-op } {\n"
        "  dup first @JmpDictL list-member? have-dict? and {\n"
        "    flush-cont show-const-dict-op } {\n"
        "  flush-dict\n"
        "  dup first @PushContL list-member? {\n"
        "    drop @curop @ get-cont-body save-cont-body } {\n"
        "  dup first @CmdC1 list-member? have-cont? and {\n"
        "    flush-contY first atom>$ .curop? show-cont-op } {\n"
        "  dup first @CmdR1 list-member? {\n"
        "    flush-cont first atom>$ dup $len 3 - $| drop .curop? show-ref-op } {\n"
        "  dup first `WHILE eq? have-cont2? and {\n"
        "    drop \"WHILE\" \"}>DO<{\" .curop? show-cont2-op } {\n"
        "  dup first `IFELSE eq? have-cont2? and {\n"
        "    drop \"IF\" \"}>ELSE<{\" .curop? show-cont2-op } {\n"
        "  dup first dup `IFREFELSE eq? swap `IFELSEREF eq? or have-cont? and {\n"
        "    first `IFREFELSE eq? \"IF\" \"}>ELSE<{\" rot .curop? show-cont-ref-op } {\n"
        "  dup first `IFREFELSEREF eq? {\n"
        "    drop \"IF\" \"}>ELSE<{\" .curop? show-ref2-op } {\n"
        "    flush-cont show-simple-op\n"
        "  } cond } cond } cond } cond } cond } cond } cond } cond } cond\n"
        "} : show-op\n"
        "{ dup @cp @ (vmoplen) dup 0> { 65536 /mod swap sr@+ swap dup @cp @ (vmopdump) parse-op swap s> true } { "
        "drop false } cond } : fetch-one-op\n"
        "{ { fetch-one-op } { swap @curop ! adjust-op show-op } while } : disasm-slice\n"
        "{ { disasm-slice dup sbitrefs 1- or 0= } { ref@ <s } while flush-dict flush-cont } : disasm-chain\n"
        "{ @curop @ swap disasm-chain dup sbitrefs or { .indent .\"Cannot disassemble: \" csr. } { drop } cond "
        "@curop ! }\n"
        "'disasm ! <s std-disasm disasm ";

  fift::IntCtx ctx{ss, "stdin", "./", 0};
  ctx.stack.push_cell(codeCell);

  ctx.ton_db = &fift.config().ton_db;
  ctx.source_lookup = &fift.config().source_lookup;
  ctx.dictionary = ctx.main_dictionary = ctx.context = &fift.config().dictionary;
  ctx.output_stream = &output;
  ctx.error_stream = fift.config().error_stream;

  try {
    auto res = ctx.run(td::make_ref<fift::InterpretCont>());
    if (res.is_error()) {
      std::cerr << "Disasm error: " << res.move_as_error().to_string();
      throw std::invalid_argument("Error in disassembler");
    } else {
      auto disasm_out = output.str();
      // cheap no-brainer
      std::string_view pattern = " ok\n";
      std::string::size_type n = pattern.length();
      for (std::string::size_type i = disasm_out.find(pattern); i != std::string::npos; i = disasm_out.find(pattern)) {
        disasm_out.erase(i, n);
      }

      return disasm_out;
    }
  } catch (const std::exception& e) {
    std::cerr << "Disasm error: " << e.what();
    throw std::invalid_argument("Error in disassembler");
  }
}

struct PyTVM {
  td::Ref<vm::Cell> code;
  td::Ref<vm::Cell> data;

  vm::GasLimits gas_limits;
  std::vector<td::Ref<vm::Cell>> lib_set;
  vm::Stack stackVm;
  bool allowDebug;
  bool sameC3;
  int log_level;

  long long c7_unixtime = 0;
  long long c7_blocklt = 0;
  long long c7_translt = 0;
  long long c7_randseed = 0;
  long long c7_balanceRemainingGrams = 100000;
  std::string c7_myaddress;
  std::string c7_globalConfig;

  int exit_code_out;
  long long vm_steps_out;
  long long gas_used_out;
  long long gas_credit_out;
  bool success_out;
  std::string vm_final_state_hash_out;
  std::string vm_init_state_hash_out;
  std::string new_data_out;
  std::string actions_out;

  void set_c7(int c7_unixtime_, int c7_blocklt_, int c7_translt_, int c7_randseed_, int c7_balanceRemainingGrams_,
              const std::string& c7_myaddress_, const std::string& c7_globalConfig_) {
    c7_unixtime = c7_unixtime_;
    c7_blocklt = c7_blocklt_;
    c7_translt = c7_translt_;
    c7_randseed = c7_randseed_;
    c7_balanceRemainingGrams = c7_balanceRemainingGrams_;
    c7_myaddress = c7_myaddress_;
    c7_globalConfig = c7_globalConfig_;
  }

  // constructor
  explicit PyTVM(int log_level_ = 0, const std::string& code_ = "", const std::string& data_ = "",
                 const bool allowDebug_ = false, const bool sameC3_ = true) {
    allowDebug = allowDebug_;
    sameC3 = sameC3_;

    this->log_level = log_level_;

    if (!code_.empty()) {
      set_code(code_);
    }

    if (!data_.empty()) {
      set_data(data_);
    }
  }

  // log utils
  void log(const std::string& log_string, int level = LOG_INFO) const {
    if (this->log_level >= level && level == LOG_INFO) {
      py::print("INFO: " + log_string);
    } else if (this->log_level >= level && level == LOG_DEBUG) {
      py::print("DEBUG: " + log_string);
    }
  }

  void log_debug(const std::string& log_string) const {
    log(log_string, LOG_DEBUG);
  }
  void log_info(const std::string& log_string) const {
    log(log_string, LOG_INFO);
  }

  void set_gasLimit(int gas_limit, int gas_max = -1) {
    if (gas_max == -1) {
      gas_limits = vm::GasLimits{gas_limit, gas_max};
    } else {
      gas_limits = vm::GasLimits{gas_limit, gas_max};
    }
  }

  // @prop Data
  void set_data(const std::string& data_) {
    log_debug("Start parse data");
    auto data_parsed = parseStringToCell(data_);
    log_debug("Data parsed success");

    data = data_parsed;

    if (log_level >= LOG_DEBUG) {
      std::stringstream log;
      py::print("Data loaded: " + data->get_hash().to_hex());
    }
  }

  std::string get_data() {
    return dump_as_boc(data);
  }

  // @prop Code
  void set_code(const std::string& code_) {
    log_debug("Start parse code");
    auto code_parsed = parseStringToCell(code_);
    log_debug("Code parsed success");

    if (code_parsed.is_null()) {
      throw std::invalid_argument("Code root need to have at least 1 root cell ;)");
    }

    code = code_parsed;

    if (log_level >= LOG_DEBUG) {
      std::stringstream log;
      py::print("Code loaded: " + code->get_hash().to_hex());
    }
  }

  std::string get_code() const {
    return dump_as_boc(code);
  }

  void set_stack(py::object stack) {
    stackVm.clear();

    auto iter = py::iter(std::move(stack));
    while (iter != py::iterator::sentinel()) {
      auto value = *iter;
      py::print("got value: ", value);
      auto parsedStackItem = cast_python_item_to_stack_entry(value);
      stackVm.push(parsedStackItem);
      ++iter;
    }
  }

  void set_libs(py::list cells) {
    lib_set.clear();  // remove old libs

    auto iter = py::iter(std::move(cells));
    while (iter != py::iterator::sentinel()) {
      auto value = *iter;
      auto stack_entry = cast_python_item_to_stack_entry(value);
      if (stack_entry.is_cell()) {
        lib_set.push_back(stack_entry.as_cell());
      } else {
        throw std::invalid_argument("All libs must be cells");
      }
      ++iter;
    }
  }

  void clear_stack() {
    stackVm.clear();
  }

  std::vector<py::object> run_vm() {
    if (code.is_null()) {
      throw std::invalid_argument("To run VM, please pass code");
    }

    auto stack_ = td::make_ref<vm::Stack>();

    std::vector<td::Ref<vm::Cell>> lib_set;

    vm::VmLog vm_log;

    if (log_level >= LOG_DEBUG) {
      vm_log = vm::VmLog();
      vm_log.log_interface = new PythonLogger();
    } else {
      vm_log = vm::VmLog::Null();
    }

    auto balance = block::CurrencyCollection{c7_balanceRemainingGrams};

    td::Ref<vm::CellSlice> my_addr;
    if (!c7_myaddress.empty()) {
      block::StdAddress tmp;
      tmp.parse_addr(c7_myaddress);
      my_addr = block::tlb::MsgAddressInt().pack_std_address(tmp);
    } else {
      vm::CellBuilder cb;
      cb.store_long(0);
      my_addr = vm::load_cell_slice_ref(cb.finalize());
    }

    td::Ref<vm::Cell> global_config;
    if (!c7_globalConfig.empty()) {
      global_config = parseStringToCell(c7_globalConfig);
    }

    auto init_c7 =
        vm::make_tuple_ref(td::make_refint(0x076ef1ea),            // [ magic:0x076ef1ea
                           td::make_refint(0),                     //   actions:Integer
                           td::make_refint(0),                     //   msgs_sent:Integer
                           td::make_refint(c7_unixtime),           //   unixtime:Integer
                           td::make_refint(c7_blocklt),            //   block_lt:Integer
                           td::make_refint(c7_translt),            //   trans_lt:Integer
                           td::make_refint(c7_randseed),           //   rand_seed:Integer
                           balance.as_vm_tuple(),                  //   balance_remaining:[Integer (Maybe Cell)]
                           std::move(my_addr),                     //  myself:MsgAddressInt
                           vm::StackEntry::maybe(global_config));  //  global_config:(Maybe Cell) ] = SmartContractInfo;

    log_debug("Use code: " + code->get_hash().to_hex());

    log_debug("Load cp0");
    vm::init_op_cp0(allowDebug);

    int flags = 0;
    if (sameC3) {
      flags += 1;
    }

    if (log_level > LOG_DEBUG) {
      flags += 4;  // dump stack
    }

    vm::VmState vm_local{code,
                         td::make_ref<vm::Stack>(stackVm),
                         gas_limits,
                         flags,
                         data,
                         vm_log,
                         std::move(lib_set),
                         std::move(init_c7)};

    vm_init_state_hash_out = vm_local.get_state_hash().to_hex();
    exit_code_out = vm_local.run();
    vm_final_state_hash_out = vm_local.get_final_state_hash(exit_code_out).to_hex();
    vm_steps_out = (int)vm_local.get_steps_count();

    auto gas = vm_local.get_gas_limits();
    gas_used_out = std::min<long long>(gas.gas_consumed(), gas.gas_limit);
    gas_credit_out = gas.gas_credit;
    success_out = (gas_credit_out == 0 && vm_local.committed());

    if (success_out) {
      new_data_out = dump_as_boc(vm_local.get_committed_state().c4);
      actions_out = dump_as_boc(vm_local.get_committed_state().c5);
    }

    log_debug("VM terminated with exit code " + std::to_string(exit_code_out));

    std::vector<py::object> pyStack;

    auto stack = vm_local.get_stack();
    for (auto idx = 0; idx < stack.depth(); idx++) {
      log_debug("Parse stack item #" + std::to_string(idx));
      pyStack.push_back(cast_stack_item_to_python_object(stack.at(idx)));
    }

    return pyStack;
  }

  int get_exit_code() const {
    return exit_code_out;
  }

  long long get_vm_steps() const {
    return vm_steps_out;
  }

  long long get_gas_used() const {
    return gas_used_out;
  }

  long long get_gas_credit() const {
    return gas_credit_out;
  }

  bool get_success() const {
    return success_out;
  }

  std::string get_new_data() const {
    return new_data_out;
  }

  std::string get_actions() const {
    return actions_out;
  }

  std::string get_vm_final_state_hash() const {
    return vm_final_state_hash_out;
  }

  std::string get_vm_init_state_hash() const {
    return vm_init_state_hash_out;
  }

  static void dummy_set() {
    throw std::invalid_argument("Not settable");
  }
};

PYBIND11_MODULE(tvm_python, m) {
  static py::exception<vm::VmError> exc(m, "VmError");
  py::register_exception_translator([](std::exception_ptr p) {
    try {
      if (p)
        std::rethrow_exception(p);
    } catch (const vm::VmError& e) {
      exc(e.get_msg());
    } catch (const vm::VmFatal& e) {
      exc("VMFatal error");
    } catch (std::exception& e) {
      PyErr_SetString(PyExc_RuntimeError, e.what());
    }
  });

  m.def("method_name_to_id", &method_name_to_id);
  m.def("code_disasseble", &code_disasseble);

  py::class_<PyTVM>(m, "PyTVM")
      .def(py::init<int, std::string, std::string, bool, bool>(), py::arg("log_level") = 0, py::arg("code") = "",
           py::arg("data") = "", py::arg("allow_debug") = false, py::arg("same_c3") = true)
      .def_property("code", &PyTVM::get_code, &PyTVM::set_code)
      .def_property("data", &PyTVM::set_data, &PyTVM::get_data)
      .def("set_stack", &PyTVM::set_stack)
      .def("set_libs", &PyTVM::set_libs)
      .def("clear_stack", &PyTVM::clear_stack)
      .def("set_gasLimit", &PyTVM::set_gasLimit, py::arg("gas_limit") = 0, py::arg("gas_max") = -1)
      .def("run_vm", &PyTVM::run_vm)
      .def("set_c7", &PyTVM::set_c7, py::arg("unixtime") = 0, py::arg("blocklt") = 0, py::arg("translt") = 0,
           py::arg("randseed") = 0, py::arg("balanceGrams") = 0, py::arg("address") = "", py::arg("globalConfig") = "")

      .def_property("exit_code", &PyTVM::get_exit_code, &PyTVM::dummy_set)
      .def_property("vm_steps", &PyTVM::get_vm_steps, &PyTVM::dummy_set)
      .def_property("gas_used", &PyTVM::get_gas_used, &PyTVM::dummy_set)
      .def_property("gas_credit", &PyTVM::get_gas_credit, &PyTVM::dummy_set)
      .def_property("success", &PyTVM::get_success, &PyTVM::dummy_set)
      .def_property("vm_final_state_hash", &PyTVM::get_vm_final_state_hash, &PyTVM::dummy_set)
      .def_property("vm_init_state_hash", &PyTVM::get_vm_init_state_hash, &PyTVM::dummy_set)
      .def_property("new_data", &PyTVM::get_new_data, &PyTVM::dummy_set)
      .def_property("actions", &PyTVM::get_actions, &PyTVM::dummy_set)

      .def("__repr__", [](const PyTVM& a) { return "tvm_python.PyTVM"; });
}