/* 
    This file is part of TON Blockchain source code.

    TON Blockchain is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation; either version 2
    of the License, or (at your option) any later version.

    TON Blockchain is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with TON Blockchain.  If not, see <http://www.gnu.org/licenses/>.

    In addition, as a special exception, the copyright holders give permission 
    to link the code of portions of this program with the OpenSSL library. 
    You must obey the GNU General Public License in all respects for all 
    of the code used other than OpenSSL. If you modify file(s) with this 
    exception, you may extend this exception to your version of the file(s), 
    but you are not obligated to do so. If you do not wish to do so, delete this 
    exception statement from your version. If you delete this exception statement 
    from all source files in the program, then also delete it here.

    Copyright 2017-2020 Telegram Systems LLP
*/
#include <vector>
#include <string>
#include <map>
#include <set>
#include <stack>
#include <utility>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <functional>
#include <sstream>
#include <cstdio>
#include <cassert>
#include <cstdlib>
#include <getopt.h>

#include "common/refcnt.hpp"
#include "common/bigint.hpp"
#include "common/refint.h"
#include "parser/srcread.h"
#include "parser/lexer.h"
#include "parser/symtable.h"
#include "td/utils/Slice-decl.h"
#include "td/utils/format.h"
#include "td/utils/crypto.h"
#include "tlbc-aux.h"
#include "tlbc-data.h"
#include "tlbc-gen-cpp.h"
#include "tlbc-gen-py.h"

int verbosity;

namespace src {

/*
 * 
 *   KEYWORD DEFINITION
 * 
 */

enum { _Eof = -1, _Ident = 0, _Number, _Special, _Eq = 0x80, _Leq, _Geq, _Neq, _Type, _EMPTY };

void define_keywords() {
  sym::symbols.add_kw_char('+')
      .add_kw_char('-')
      .add_kw_char('*')
      .add_kw_char(':')
      .add_kw_char(';')
      .add_kw_char('(')
      .add_kw_char(')')
      .add_kw_char('{')
      .add_kw_char('}')
      .add_kw_char('[')
      .add_kw_char(']')
      .add_kw_char('=')
      .add_kw_char('_')
      .add_kw_char('?')
      .add_kw_char('.')
      .add_kw_char('~')
      .add_kw_char('^');

  sym::symbols.add_keyword("==", _Eq)
      .add_keyword("<=", _Leq)
      .add_keyword(">=", _Geq)
      .add_keyword("!=", _Neq)
      .add_keyword("Type", _Type)
      .add_keyword("EMPTY", _EMPTY);
}

// parses constant bitstrings in format \#[0-9a-f]*_? or \$[01]*_?
unsigned long long get_special_value(std::string str) {
  std::size_t i = 1, n = str.size();
  if (n <= 1) {
    return 0;
  }
  unsigned long long val = 0;
  int bits = 0;
  if (str[0] == '#') {
    for (; i < n; i++) {
      int c = str[i];
      if (c == '_') {
        break;
      }
      if (c >= '0' && c <= '9') {
        c -= '0';
      } else if (c >= 'A' && c <= 'F') {
        c -= 'A' - 10;
      } else if (c >= 'a' && c <= 'f') {
        c -= 'a' - 10;
      } else {
        return 0;
      }
      if (bits > 60) {
        return 0;
      }
      val |= (unsigned long long)c << (60 - bits);
      bits += 4;
    }
  } else if (str[0] == '$') {
    if (str[1] != '_') {
      for (; i < n; i++) {
        int c = str[i];
        c -= '0';
        if (c & -2) {
          return 0;
        }
        if (bits > 63) {
          return 0;
        }
        val |= (unsigned long long)c << (63 - bits);
        bits++;
      }
    }
  } else {
    return 0;
  }
  if (i < n - 1) {
    return 0;
  }
  if (i == n - 1 && bits) {
    // trailing _
    while (bits && !((val >> (64 - bits)) & 1)) {
      --bits;
    }
    if (bits) {
      --bits;
    }
  }
  if (bits == 64) {
    return 0;
  }
  return val | (1ULL << (63 - bits));
}

int lexem_is_special(std::string str) {
  return get_special_value(str) ? Lexem::Special : 0;
}

}  // namespace src

namespace sym {

enum class IdSc : char { undef = 0, lc = 1, uc = 2, blc = 3 };
// subclass:
// 1 = first letter or first letter after last . is lowercase
// 2 = ... uppercase
// 3 = 1 + first character (after last ., if present) is a !
// 0 = else
int compute_symbol_subclass(std::string str) {
  IdSc res = IdSc::undef;
  int t = 0, s = 0;
  for (char c : str) {
    if (c == '.') {
      res = IdSc::undef;
      s = t = 0;
    } else if (res == IdSc::undef) {
      if (!s) {
        s = (c == '!' ? 1 : -1);
      }
      if ((c | 0x20) >= 'a' && (c | 0x20) <= 'z') {
        res = (c & 0x20 ? IdSc::lc : IdSc::uc);
      }
      if (t && (((unsigned)c & 0xc0) == 0x80)) {
        t = (t << 6) | ((unsigned)c & 0x3f);
        if (t >= 0x410 && t < 0x450) {
          res = (t < 0x430 ? IdSc::uc : IdSc::lc);
        }
      }
      t = (((unsigned)c & 0xe0) == 0xc0 ? (c & 0x1f) : 0);
    }
  }
  if (s == 1 && res == IdSc::lc) {
    res = IdSc::blc;
  }
  return (int)res;
}

inline bool is_lc_ident(sym_idx_t idx) {
  auto sc = symbols.get_subclass(idx);
  return sc == (int)IdSc::lc || sc == (int)IdSc::blc;
}

inline bool is_spec_lc_ident(sym_idx_t idx) {
  auto sc = symbols.get_subclass(idx);
  return sc == (int)IdSc::blc;
}

inline bool is_uc_ident(sym_idx_t idx) {
  return symbols.get_subclass(idx) == (int)IdSc::uc;
}

}  // namespace sym

namespace tlbc {

td::LinearAllocator AR(1 << 22);

/*
 * 
 *  AUXILIARY DATA TYPES
 * 
 */

// headers are in tlbc-aux.h

std::ostream& operator<<(std::ostream& os, const BitPfxCollection& p) {
  p.show(os);
  return os;
}

void BitPfxCollection::show(std::ostream& os) const {
  char first = '{';
  for (unsigned long long val : pfx) {
    os << first;
    while (val & (All - 1)) {
      os << (val >> 63);
      val <<= 1;
    }
    os << '*';
    first = ',';
  }
  if (first == '{') {
    os << '{';
  }
  os << '}';
}

void BitPfxCollection::merge_back(unsigned long long z) {
  if (!pfx.size()) {
    pfx.push_back(z);
    return;
  }
  unsigned long long w = td::lower_bit64(z);
  while (pfx.size()) {
    unsigned long long t = z ^ pfx.back();
    if (!t) {
      return;
    }
    if (t != (w << 1)) {
      break;
    }
    z -= w;
    w <<= 1;
    pfx.pop_back();
  }
  pfx.push_back(z);
}

BitPfxCollection& BitPfxCollection::operator*=(unsigned long long prepend) {
  if (!prepend) {
    clear();
    return *this;
  }
  if (prepend == All) {
    return *this;
  }
  int l = 63 - td::count_trailing_zeroes_non_zero64(prepend);
  prepend &= prepend - 1;
  std::size_t i, j = 0, n = pfx.size();
  for (i = 0; i < n; i++) {
    unsigned long long z = pfx[i], zw = td::lower_bit64(z);
    z >>= l;
    z |= prepend;
    if (!(zw >> l)) {
      z |= 1;
    }
    if (!j || pfx[j - 1] != z) {
      pfx[j++] = z;
    }
  }
  pfx.resize(j);
  return *this;
}

BitPfxCollection BitPfxCollection::operator*(unsigned long long prepend) const {
  if (!prepend) {
    return BitPfxCollection{};
  }
  if (prepend == All) {
    return *this;
  }
  BitPfxCollection res;
  res.pfx.reserve(pfx.size());
  int l = 63 - td::count_trailing_zeroes_non_zero64(prepend);
  prepend &= prepend - 1;
  std::size_t i, n = pfx.size();
  for (i = 0; i < n; i++) {
    unsigned long long z = pfx[i], zw = td::lower_bit64(z);
    z >>= l;
    z |= prepend;
    if (!(zw >> l)) {
      z |= 1;
    }
    res.merge_back(z);
  }
  return res;
}

BitPfxCollection BitPfxCollection::operator+(const BitPfxCollection& other) const {
  if (!other.pfx.size()) {
    return *this;
  }
  if (!pfx.size()) {
    return other;
  }
  BitPfxCollection res;
  res.pfx.reserve(pfx.size() + other.pfx.size());
  std::size_t i = 0, j = 0, m = pfx.size(), n = other.pfx.size();
  struct Interval {
    unsigned long long z, a, b;
    void operator=(unsigned long long _z) {
      z = _z;
      a = (_z & (_z - 1));
      b = (_z | (_z - 1));
    }
    bool contains(const Interval& other) const {
      return a <= other.a && other.b <= b;
    }
  };
  Interval U, V;
  U = pfx[0];
  V = other.pfx[0];
  while (i < m && j < n) {
    if (U.b < V.b || (U.b == V.b && U.a >= V.a)) {
      if (U.a < V.a) {
        res.merge_back(U.z);
      }
      if (++i == m) {
        break;
      }
      U = pfx[i];
    } else {
      if (V.a < U.a) {
        res.merge_back(V.z);
      }
      if (++j == n) {
        break;
      }
      V = other.pfx[j];
    }
  }
  while (i < m) {
    res.merge_back(pfx[i++]);
  }
  while (j < n) {
    res.merge_back(other.pfx[j++]);
  }
  return res;
}

bool BitPfxCollection::operator+=(const BitPfxCollection& other) {
  BitPfxCollection tmp = *this + other;
  if (*this == tmp) {
    return false;
  } else {
    *this = tmp;
    return true;
  }
}

void AdmissibilityInfo::set_all(bool val) {
  dim = 0;
  info.clear();
  info.resize(1, val);
}

std::ostream& operator<<(std::ostream& os, const AdmissibilityInfo& p) {
  p.show(os);
  return os;
}

void AdmissibilityInfo::show(std::ostream& os) const {
  os << '[';
  for (bool x : info) {
    os << (int)x;
  }
  os << ']';
}

void AdmissibilityInfo::extend(int dim1) {
  if (dim < dim1) {
    std::size_t i, n = info.size(), n1 = (size_t(1) << (2 * dim1));
    assert(n);
    info.resize(n1);
    for (i = n; i < n1; i++) {
      info[i] = info[i - n];
    }
    dim = dim1;
  }
}

void AdmissibilityInfo::operator|=(const AdmissibilityInfo& other) {
  extend(other.dim);
  std::size_t i, j, n = info.size(), n1 = other.info.size();
  assert(n1 && !(n1 & (n1 - 1)));
  for (i = j = 0; i < n; i++) {
    info[i] = info[i] | other.info[j];
    j = (j + 1) & (n1 - 1);
  }
}

void AdmissibilityInfo::set_by_pattern(int pdim, int pattern[]) {
  extend(pdim);
  std::size_t n = info.size();
  for (std::size_t x = 0; x < n; x++) {
    std::size_t y = x;
    bool f = true;
    for (int i = 0; i < pdim; i++) {
      if (!((pattern[i] >> (y & 3)) & 1)) {
        f = false;
        break;
      }
      y >>= 2;
    }
    if (f) {
      info[x] = true;
    }
  }
}

int AdmissibilityInfo::conflicts_at(const AdmissibilityInfo& other) const {
  std::size_t i, n1 = info.size(), n2 = other.info.size(), n = std::max(n1, n2);
  for (i = 0; i < n; i++) {
    if (info[i & (n1 - 1)] && other.info[i & (n2 - 1)]) {
      return (int)i;
    }
  }
  return -1;
}

bool AdmissibilityInfo::extract1(char A[4], char tag, int p1) const {
  char B[4];
  std::memset(B, 0, sizeof(B));
  p1 <<= 1;
  std::size_t n = info.size() - 1;
  for (std::size_t x = 0; x <= n; x++) {
    if (info[x]) {
      B[(x >> p1) & 3] = 1;
    }
  }
  int m1 = ((n >> p1) & 3);
  for (int i = 0; i < 4; i++) {
    if (B[i & m1]) {
      if (A[i] && A[i] != tag) {
        A[i] = -1;
        return false;
      }
      A[i] = tag;
    }
  }
  return true;
}

bool AdmissibilityInfo::extract2(char A[4][4], char tag, int p1, int p2) const {
  char B[4][4];
  std::memset(B, 0, sizeof(B));
  p1 <<= 1;
  p2 <<= 1;
  std::size_t n = info.size() - 1;
  for (std::size_t x = 0; x <= n; x++) {
    if (info[x]) {
      B[(x >> p1) & 3][(x >> p2) & 3] = 1;
    }
  }
  int m1 = ((n >> p1) & 3);
  int m2 = ((n >> p2) & 3);
  for (int i = 0; i < 4; i++) {
    for (int j = 0; j < 4; j++) {
      if (B[i & m1][j & m2]) {
        if (A[i][j] && A[i][j] != tag) {
          A[i][j] = -1;
          return false;
        }
        A[i][j] = tag;
      }
    }
  }
  return true;
}

bool AdmissibilityInfo::extract3(char A[4][4][4], char tag, int p1, int p2, int p3) const {
  char B[4][4][4];
  std::memset(B, 0, sizeof(B));
  p1 <<= 1;
  p2 <<= 1;
  p3 <<= 1;
  std::size_t n = info.size() - 1;
  for (std::size_t x = 0; x <= n; x++) {
    if (info[x]) {
      B[(x >> p1) & 3][(x >> p2) & 3][(x >> p3) & 3] = 1;
    }
  }
  int m1 = ((n >> p1) & 3);
  int m2 = ((n >> p2) & 3);
  int m3 = ((n >> p3) & 3);
  for (int i = 0; i < 4; i++) {
    for (int j = 0; j < 4; j++) {
      for (int k = 0; k < 4; k++) {
        if (B[i & m1][j & m2][k & m3]) {
          if (A[i][j][k] && A[i][j][k] != tag) {
            A[i][j][k] = -1;
            return false;
          }
          A[i][j][k] = tag;
        }
      }
    }
  }
  return true;
}

void ConflictGraph::set_clique(ConflictSet set) {
  if (set.x) {
    for (int i = 0; i < 64; i++) {
      if (set[i]) {
        g[i] |= set;
      }
    }
  }
}

std::ostream& operator<<(std::ostream& os, const BinTrie& bt) {
  bt.show(os);
  return os;
}

void BinTrie::show(std::ostream& os, unsigned long long pfx) const {
  unsigned long long x = pfx, u = (td::lower_bit64(x) >> 1);
  while (x & ((1ULL << 63) - 1)) {
    os << (x >> 63);
    x <<= 1;
  }
  os << " t=" << tag << "; dt=" << down_tag << "; ud=" << useful_depth << std::endl;
  if (left) {
    left->show(os, pfx - u);
  }
  if (right) {
    right->show(os, pfx + u);
  }
}

void BinTrie::ins_path(unsigned long long path, unsigned long long new_tag) {
  if (!path || !new_tag) {
    return;
  }
  if (!(path & ((1ULL << 63) - 1))) {
    tag |= new_tag;
    return;
  } else if ((long long)path >= 0) {
    left = insert_path(std::move(left), path << 1, new_tag);
  } else {
    right = insert_path(std::move(right), path << 1, new_tag);
  }
  if (left && right) {
    tag |= left->tag & right->tag;
  }
}

std::unique_ptr<BinTrie> BinTrie::insert_path(std::unique_ptr<BinTrie> root, unsigned long long path,
                                              unsigned long long tag) {
  if (!path || !tag) {
    return root;
  }
  if (root) {
    root->ins_path(path, tag);
    return root;
  }
  if (!(path & ((1ULL << 63) - 1))) {
    return std::make_unique<BinTrie>(tag);
  }
  if ((long long)path >= 0) {
    return std::make_unique<BinTrie>(0, insert_path({}, path << 1, tag), std::unique_ptr<BinTrie>{});
  } else {
    return std::make_unique<BinTrie>(0, std::unique_ptr<BinTrie>{}, insert_path({}, path << 1, tag));
  }
}

std::unique_ptr<BinTrie> BinTrie::insert_paths(std::unique_ptr<BinTrie> root, const BitPfxCollection& paths,
                                               unsigned long long tag) {
  if (tag) {
    for (auto x : paths.pfx) {
      root = insert_path(std::move(root), x, tag);
    }
  }
  return root;
}

unsigned long long BinTrie::lookup_tag(unsigned long long path) const {
  const BinTrie* node = lookup_node_const(path);
  return node ? node->tag : 0;
}

BinTrie* BinTrie::lookup_node(unsigned long long path) {
  if (!path) {
    return nullptr;
  }
  if (!(path & ((1ULL << 63) - 1))) {
    return this;
  }
  if ((long long)path >= 0) {
    return left ? left->lookup_node(path << 1) : nullptr;
  } else {
    return right ? right->lookup_node(path << 1) : nullptr;
  }
}

const BinTrie* BinTrie::lookup_node_const(unsigned long long path) const {
  if (!path) {
    return nullptr;
  }
  if (!(path & ((1ULL << 63) - 1))) {
    return this;
  }
  if ((long long)path >= 0) {
    return left ? left->lookup_node_const(path << 1) : nullptr;
  } else {
    return right ? right->lookup_node_const(path << 1) : nullptr;
  }
}

void BinTrie::set_conflict_graph(ConflictGraph& gr, unsigned long long colors) const {
  colors |= tag;
  if (!left || !right) {
    gr.set_clique(ConflictSet{colors});
  }
  if (left) {
    left->set_conflict_graph(gr, colors);
  }
  if (right) {
    right->set_conflict_graph(gr, colors);
  }
}

int BinTrie::compute_useful_depth(unsigned long long colors) {
  int res = 0;  // useless;
  down_tag = tag = colors |= tag;
  if (left) {
    res = left->compute_useful_depth(colors);
    down_tag |= left->down_tag;
  }
  if (right) {
    res = std::max(res, right->compute_useful_depth(colors));
    down_tag |= right->down_tag;
  }
  if (res > 0) {
    return useful_depth = res + 1;
  }
  if (left && right && (left->down_tag & ~right->down_tag) != 0 && (right->down_tag & ~left->down_tag) != 0) {
    return useful_depth = 1;
  }
  return useful_depth = 0;
}

unsigned long long BinTrie::build_submap(int depth, unsigned long long A[]) const {
  if (!depth) {
    A[0] = down_tag | (useful_depth ? (1ULL << 63) : 0);
    return down_tag != 0;
  }
  int n = (1 << (depth - 1));
  unsigned long long r1 = 0, r2 = 0;
  if (left) {
    r1 = left->build_submap(depth - 1, A);
  } else {
    std::memset(A, 0, n * 8);
  }
  if (right) {
    r2 = right->build_submap(depth - 1, A + n);
  } else {
    std::memset(A + n, 0, n * 8);
  }
  if (A[n] != A[n - 1] || (long long)A[n] < 0) {
    r2 |= 1;
  } else {
    r2 &= ~1;
  }
  return r1 | (r2 << n);
}

unsigned long long BinTrie::build_submap_at(int depth, unsigned long long A[], unsigned long long pfx) const {
  const BinTrie* node = lookup_node_const(pfx);
  if (!node) {
    std::memset(A, 0, 8 << depth);
    return 0;
  }
  return node->build_submap(depth, A);
}

unsigned long long BinTrie::find_conflict_path(unsigned long long colors, unsigned long long mask) const {
  colors |= tag & mask;
  if (!left && !right) {
    return colors & (colors - 1) ? (1ULL << 63) : 0;
  }
  if (!left) {
    if (colors & (colors - 1)) {
      return (1ULL << 62);  // $0
    } else {
      unsigned long long x = right->find_conflict_path(colors, mask);
      return x ? ((x >> 1) | (1ULL << 63)) : 0;
    }
  } else if (!right) {
    if (colors & (colors - 1)) {
      return (3ULL << 62);  // $1
    } else {
      return left->find_conflict_path(colors, mask) >> 1;
    }
  }
  unsigned long long x = left->find_conflict_path(colors, mask);
  unsigned long long y = right->find_conflict_path(colors, mask);
  if (td::lower_bit64(y) > td::lower_bit64(x)) {
    return (y >> 1) | (1ULL << 63);
  } else {
    return x >> 1;
  }
}

std::ostream& operator<<(std::ostream& os, MinMaxSize t) {
  t.show(os);
  return os;
}

void MinMaxSize::normalize() {
  if (minmax_size & (0xfff800f8U * 0x100000001ULL)) {
    nrm(0xf8, 0x7);
    nrm(0xfff80000U, 0x7ff00);
    nrm(0xf8ULL << 32, 7ULL << 32);
    nrm(0xfff80000ULL << 32, 0x7ff00ULL << 32);
  }
}

MinMaxSize::unpacked::unpacked(MinMaxSize val) {
  val.normalize();
  max_refs = val.minmax_size & 0xff;
  max_bits = (val.minmax_size >> 8) & 0x7ff;
  min_refs = (val.minmax_size >> 32) & 0xff;
  min_bits = (val.minmax_size >> 40) & 0x7ff;
}

MinMaxSize MinMaxSize::unpacked::pack() const {
  unsigned long long t = ((unsigned long long)(min_bits * 0x100 + min_refs) << 32);
  t += (max_bits * 0x100 + max_refs);
  return MinMaxSize{t};
}

MinMaxSize& MinMaxSize::repeat(int count) {
  if (count <= 0) {
    return clear();
  }
  if (count == 1) {
    return *this;
  }
  unpacked z{*this};
  count = std::min(count, 1024);
  z.max_refs = std::min(z.max_refs * count, 7U);
  z.max_bits = std::min(z.max_bits * count, 0x7ffU);
  z.min_refs = std::min(z.min_refs * count, 7U);
  z.min_bits = std::min(z.min_bits * count, 0x7ffU);
  return *this = z.pack();
}

MinMaxSize& MinMaxSize::repeat_at_least(int count) {
  count = std::min(std::max(count, 0), 1024);
  unpacked z{*this};
  if (z.max_refs) {
    z.max_refs = 7;
  }
  if (z.max_bits) {
    z.max_bits = 0x7ff;
  }
  z.min_refs = std::min(z.min_refs * count, 7U);
  z.min_bits = std::min(z.min_bits * count, 0x7ffU);
  return *this = z.pack();
}

MinMaxSize& MinMaxSize::operator|=(MinMaxSize y) {
  unpacked z{*this}, w{y};
  z.min_refs = std::min(z.min_refs, w.min_refs);
  z.min_bits = std::min(z.min_bits, w.min_bits);
  z.max_refs = std::max(z.max_refs, w.max_refs);
  z.max_bits = std::max(z.max_bits, w.max_bits);
  return *this = z.pack();
}

void MinMaxSize::show(std::ostream& os) const {
  unpacked z{*this};
  z.show(os);
}

void MinMaxSize::unpacked::show(std::ostream& os) const {
  bool fixed = (min_bits == max_bits && min_refs == max_refs);
  if (fixed) {
    os << '=';
  }
  if (min_bits >= 1024 && min_refs >= 7) {
    os << "infty";
  } else {
    os << min_bits;
    if (min_refs) {
      os << "+" << min_refs << "R";
    }
  }
  if (!fixed) {
    os << "..";
    if (max_bits >= 1024 && max_refs >= 7) {
      os << "infty";
    } else {
      os << max_bits;
      if (max_refs) {
        os << "+" << max_refs << "R";
      }
    }
  }
}

}  // namespace tlbc

namespace tlbc {

using src::Lexem;
using src::Lexer;
using sym::sym_idx_t;

/*
 * 
 *  DATA TYPES: Type Expressions, Types, Constructors
 * 
 */

// headers in tlbc-data.h

std::ostream& operator<<(std::ostream& os, const TypeExpr* te) {
  if (te) {
    te->show(os);
  } else {
    os << "(null-type)";
  }
  return os;
}

std::ostream& operator<<(std::ostream& os, const Constructor& cs) {
  cs.show(os);
  return os;
}

TypeExpr type_Type{{}, TypeExpr::te_Type};

TypeExpr* const_type_expr[TypeExpr::max_const_expr];
int const_type_expr_num;

TypeExpr* const_htable[TypeExpr::const_htable_size];

sym_idx_t Nat_name, Eq_name, Less_name, Leq_name;
Type* Nat_type;
Type *Eq_type, *Less_type, *Leq_type;
Type *NatWidth_type, *NatLess_type, *NatLeq_type, *Int_type, *UInt_type;
Type* Bits_type;
Type *Any_type, *Cell_type;

int types_num, builtin_types_num;
std::vector<Type> types;

int Type::last_declared_counter;

void TypeExpr::check_mode(const src::SrcLocation& loc, int mode) {
  if (!(mode & (1 << (is_nat ? 1 : 0)))) {
    if (is_nat) {
      throw src::ParseError{loc, "type expression required"};
    } else {
      throw src::ParseError{loc, "integer expression required"};
    }
  }
  if (tchk_only && !(mode & 8)) {
    throw src::ParseError{where, "type expression can be used only in a type-checking context"};
  }
}

bool TypeExpr::no_tchk() const {
  if (tchk_only) {
    throw src::ParseError{where, "type expression can be used only in a type-checking context"};
    return false;
  }
  return true;
}

TypeExpr* TypeExpr::mk_intconst(const src::SrcLocation& loc, unsigned int_const) {
  return new (AR) TypeExpr{loc, te_IntConst, (int)int_const};
}

TypeExpr* TypeExpr::mk_intconst(const src::SrcLocation& loc, std::string int_const) {
  char* end_ptr = 0;
  long long value = -1;
  if (!int_const.empty()) {
    value = std::strtoll(int_const.c_str(), &end_ptr, 0);
  }
  if (value < 0 || value >= (1LL << 31) || end_ptr != int_const.c_str() + int_const.size()) {
    throw src::ParseError{loc, "integer constant does not fit in an unsigned 31-bit integer"};
  }
  return mk_intconst(loc, (unsigned)value);
}

TypeExpr* TypeExpr::mk_apply_gen(const src::SrcLocation& loc, TypeExpr* expr1, TypeExpr* expr2) {
  if (expr1->tp != te_Apply) {
    throw src::ParseError{loc, "cannot apply one expression to the other"};
  }
  expr1->args.push_back(expr2);
  return expr1;
}

TypeExpr* TypeExpr::mk_mulint(const src::SrcLocation& loc, TypeExpr* expr1, TypeExpr* expr2) {
  if (expr1->tp != te_IntConst && expr2->tp != te_IntConst) {
    throw src::ParseError{loc, "multiplication allowed only by constant values"};
  }
  if (expr2->tp != te_IntConst) {
    std::swap(expr1, expr2);
  }
  if (!expr1->is_nat) {
    throw src::ParseError{expr1->where, "argument to integer multiplication should be a number"};
  }
  if (expr1->tp == te_IntConst) {
    long long product = (long long)expr1->value * expr2->value;
    if (product < 0 || product >= (1LL << 31)) {
      throw src::ParseError{loc, "product does not git in 31 bits"};
    }
    return mk_intconst(loc, (unsigned)product);
  }
  int val = expr2->value;
  if (!val) {
    return expr2;
  }
  // delete expr2;
  return new (AR) TypeExpr{loc, te_MulConst, val, {expr1}, expr1->negated};
}

TypeExpr* TypeExpr::mk_apply(const src::SrcLocation& loc, int tp, TypeExpr* expr1, TypeExpr* expr2) {
  TypeExpr* expr = new (AR) TypeExpr{loc, tp, 0, {expr1, expr2}};
  return expr;
}

TypeExpr* TypeExpr::mk_cellref(const src::SrcLocation& loc, TypeExpr* expr1) {
  TypeExpr* expr = new (AR) TypeExpr{loc, te_Ref, 0, {expr1}};
  return expr;
}

Field& Constructor::new_field(const src::SrcLocation& field_where, bool implicit, sym_idx_t name = 0) {
  assert((long)fields_num == (long)fields.size());
  if (name) {
    sym::SymDef* sym_def = sym::lookup_symbol(name, 1);
    if (sym_def) {
      throw src::ParseError{field_where, "redefined field or parameter " + sym::symbols.get_name(name)};
    }
  }
  fields.emplace_back(field_where, implicit, fields_num++, name);
  return fields.back();
}

std::string Field::get_name() const {
  return sym::symbols.get_name(name);
}

// register symbol in local symbol table
void Field::register_sym() const {
  if (name) {
    sym::SymDef* sym_def = sym::lookup_symbol(name, 1);
    if (sym_def) {
      throw src::ParseError{loc, "redefined field or parameter " + sym::symbols.get_name(name)};
    } else {
      sym_def = sym::define_symbol(name, true, loc);
      if (!sym_def) {
        throw src::ParseError{loc, "cannot register field"};
      }
    }
    delete sym_def->value;
    sym_def->value = new SymVal((int)SymVal::_Param, field_idx, type);
  }
}

bool TypeExpr::close(const src::SrcLocation& loc) {
  if (tp != te_Apply) {
    return true;
  }
  Type* type = type_applied;
  assert(type);
  if (type->arity < 0) {
    type->arity = (int)args.size();
    type->args.resize(type->arity, 0);
  } else if (type->arity != (int)args.size()) {
    throw src::ParseError{where,
                          std::string{"operator `"} + sym::symbols.get_name(type->type_name) +
                              "` applied with incorrect number of arguments, partial type applications not supported"};
    return false;
  }
  is_nat_subtype = type->produces_nat;
  bool is_eq = (type == Eq_type);
  int neg_cnt = 0;
  for (int i = 0; i < type->arity; i++) {
    TypeExpr* arg = args[i];
    int& x = type->args[i];
    if (arg->negated) {
      ++neg_cnt;
      negated = true;
      if (!is_eq) {
        if (x & Type::_IsPos) {
          throw src::ParseError{arg->where, std::string{"passed an argument of incorrect polarity to `"} +
                                                sym::symbols.get_name(type->type_name) + "`"};
        }
        x |= Type::_IsNeg;
      } else if (neg_cnt == 2) {
        throw src::ParseError{loc, "cannot equate two expressions of negative polarity"};
      }
    }
    arg->no_tchk();
    if (arg->is_nat) {
      x |= Type::_IsNat;
    } else {
      x |= Type::_IsType;
      if (arg->negated) {
        throw src::ParseError{arg->where, "cannot use negative types as arguments to other types"};
      }
    }
  }
  tchk_only = negated = neg_cnt;
  return true;
}

TypeExpr* TypeExpr::mk_apply_empty(const src::SrcLocation& loc, sym_idx_t name, Type* type_applied) {
  TypeExpr* expr = new (AR) TypeExpr{loc, te_Apply, name};
  expr->type_applied = type_applied;
  expr->is_nat_subtype = (type_applied->produces_nat && !type_applied->arity);
  return expr;
}

void Type::print_name(std::ostream& os) const {
  if (type_name) {
    os << sym::symbols.get_name(type_name);
  } else {
    os << "TYPE_" << type_idx;
  }
}

std::string Type::get_name() const {
  if (type_name) {
    return sym::symbols.get_name(type_name);
  } else {
    std::ostringstream os;
    os << "TYPE_" << type_idx;
    return os.str();
  }
}

std::string Constructor::get_name() const {
  return sym::symbols.get_name(constr_name);
}

std::string Constructor::get_qualified_name() const {
  return type_defined->get_name() + "::" + get_name();
}

void TypeExpr::show(std::ostream& os, const Constructor* cs, int prio, int mode) const {
  if (mode & 2) {
    prio = 0;
  }
  switch (tp) {
    case te_Type:
      os << "Type";
      break;
    case te_Param: {
      int i = value;
      sym_idx_t param_name = 0;
      if (cs && i >= 0 && i < cs->fields_num) {
        param_name = cs->fields.at(i).name;
      }
      if (negated ^ (mode & 1)) {
        os << '~';
      }
      if (param_name > 0) {
        os << sym::symbols.get_name(param_name);
      } else {
        os << '_' << i + 1;
      }
      break;
    }
    case te_Apply:
      if (!args.size() && !type_applied->type_name && type_applied->constr_num == 1 &&
          !type_applied->constructors.at(0)->constr_name &&
          !(type_applied->constructors[0]->tag & ((1ULL << 63) - 1))) {
        type_applied->constructors[0]->show(os, mode | 4);
      } else {
        if (prio > 90 && args.size()) {
          os << '(';
        }
        type_applied->print_name(os);
        for (TypeExpr* arg : args) {
          os << ' ';  // priority 90
          arg->show(os, cs, 91, mode);
        }
        if (prio > 90 && args.size()) {
          os << ')';
        }
      }
      break;
    case te_Add: {
      assert(args.size() == 2);
      if (prio > 20) {
        os << '(';
      }
      args[0]->show(os, cs, 20, mode);
      os << " + ";  // priority 20
      args[1]->show(os, cs, 21, mode);
      if (prio > 20) {
        os << ')';
      }
      break;
    }
    case te_GetBit: {
      assert(args.size() == 2);
      if (prio > 97) {
        os << '(';
      }
      args[0]->show(os, cs, 98, mode);
      os << ".";  // priority 20
      args[1]->show(os, cs, 98, mode);
      if (prio > 97) {
        os << ')';
      }
      break;
    }
    case te_IntConst: {
      assert(args.empty());
      os << value;
      break;
    }
    case te_MulConst: {
      if (prio > 30) {
        os << '(';
      }
      assert(args.size() == 1);
      os << value << " * ";  // priority 30
      args[0]->show(os, cs, 31, mode);
      if (prio > 30) {
        os << ')';
      }
      break;
    }
    case te_Tuple: {
      assert(args.size() == 2);
      if (prio > 30) {
        os << '(';
      }
      args[0]->show(os, cs, 30, mode);
      os << " * ";  // priority 30
      args[1]->show(os, cs, 31, mode);
      if (prio > 30) {
        os << ')';
      }
      break;
    }
    case te_CondType: {
      assert(args.size() == 2);
      if (prio > 95) {
        os << '(';
      }
      args[0]->show(os, cs, 96, mode);
      os << "?";  // priority 95
      args[1]->show(os, cs, 96, mode);
      if (prio > 95) {
        os << ')';
      }
      break;
    }
    case te_Ref: {
      assert(args.size() == 1);
      os << '^';  // priority 100
      args[0]->show(os, cs, 100, mode);
      break;
    }
    default:
      os << "(unknown-type)";
  }
}

bool TypeExpr::equal(const TypeExpr& other) const {
  if (tp != other.tp || value != other.value || type_applied != other.type_applied ||
      args.size() != other.args.size()) {
    return false;
  }
  for (std::size_t i = 0; i < args.size(); i++) {
    if (!args[i]->equal(*other.args[i])) {
      return false;
    }
  }
  return true;
}

// 0 = 0, 1 = 1, 2 = any even >= 2, 3 = any odd >= 3
// (We work in N/(4~2), or in the free semilattice generated by it.)
int abstract_nat_const(int value) {
  return 1 << ((value & 1) + (value >= 2 ? 2 : 0));
}

unsigned char abstract_add_base_table[4][4] = {{0, 1, 2, 3}, {1, 2, 3, 2}, {2, 3, 2, 3}, {3, 2, 3, 2}};
unsigned char abstract_mul_base_table[4][4] = {{0, 0, 0, 0}, {0, 1, 2, 3}, {0, 2, 2, 2}, {0, 3, 2, 3}};
unsigned char abstract_getbit_b_table[4][4] = {{1, 1, 1, 1}, {2, 1, 1, 1}, {1, 3, 3, 3}, {2, 3, 3, 3}};

unsigned char abstract_add_table[16][16];
unsigned char abstract_mul_table[16][16];
unsigned char abstract_getbit_table[16][16];

void compute_semilat_table(unsigned char table[16][16], const unsigned char base_table[4][4]) {
  for (int x = 0; x < 16; x++) {
    for (int y = 0; y < 16; y++) {
      int res = 0;
      for (int i = 0; i < 4; i++) {
        if ((x >> i) & 1) {
          for (int j = 0; j < 4; j++) {
            if ((y >> j) & 1) {
              res |= 1 << base_table[i][j];
            }
          }
        }
      }
      table[x][y] = (unsigned char)res;
    }
  }
}

void compute_semilat_b_table(unsigned char table[16][16], const unsigned char b_table[4][4]) {
  for (int x = 0; x < 16; x++) {
    for (int y = 0; y < 16; y++) {
      int res = 0;
      for (int i = 0; i < 4; i++) {
        if ((x >> i) & 1) {
          for (int j = 0; j < 4; j++) {
            if ((y >> j) & 1) {
              res |= b_table[i][j];
            }
          }
        }
      }
      table[x][y] = (unsigned char)res;
    }
  }
}

void init_abstract_tables() {
  compute_semilat_table(abstract_add_table, abstract_add_base_table);
  compute_semilat_table(abstract_mul_table, abstract_mul_base_table);
  compute_semilat_b_table(abstract_getbit_table, abstract_getbit_b_table);
}

int abstract_add(int x, int y) {
  return abstract_add_table[x & 15][y & 15];
}

int abstract_mul(int x, int y) {
  return abstract_mul_table[x & 15][y & 15];
}

int abstract_getbit(int x, int y) {
  return abstract_getbit_table[x & 15][y & 15];
}

int TypeExpr::abstract_interpret_nat() const {
  if (!is_nat || tchk_only) {
    return 0;
  }
  switch (tp) {
    case te_Param:
      return 0xf;  // for now, natural parameters can take arbitrary values
    case te_Add:
      assert(args.size() == 2);
      return abstract_add(args[0]->abstract_interpret_nat(), args[1]->abstract_interpret_nat());
    case te_GetBit:
      assert(args.size() == 2);
      return abstract_getbit(args[0]->abstract_interpret_nat(), args[1]->abstract_interpret_nat());
    case te_IntConst:
      return abstract_nat_const(value);
    case te_MulConst:
      assert(args.size() == 1);
      return abstract_mul(args[0]->abstract_interpret_nat(), abstract_nat_const(value));
    default:
      return 0xf;
  }
}

MinMaxSize TypeExpr::compute_size() const {
  if (is_nat) {
    return {0};
  }
  switch (tp) {
    case te_Type:
      return {0};
    case te_Param:
      return {MinMaxSize::Any};  // any size possible for type parameters
    case te_Ref: {
      assert(args.size() == 1);
      bool f = args[0]->compute_size().is_possible();
      return f ? MinMaxSize::OneRef : MinMaxSize::Impossible;
    }
    case te_CondType: {
      assert(args.size() == 2);
      int z = args[0]->abstract_interpret_nat();
      if (!(z & ~1)) {
        return {0};  // always 0
      } else {
        MinMaxSize t = args[1]->compute_size();
        if (z & 1) {
          t.clear_min();
        }
        return t;
      }
    }
    case te_Tuple: {
      assert(args.size() == 2);
      int z = args[0]->abstract_interpret_nat();
      if (!(z & ~1)) {
        return {0};  // always 0
      } else {
        MinMaxSize t = args[1]->compute_size();
        if (args[0]->tp == te_IntConst) {
          t.repeat(args[0]->value);
          return t;
        }
        if (z & 1) {
          t.clear_min();  // zero repetition count possible
        }
        if (z & 12) {
          // may be repeated more than once
          int n = ((z & 1) ? 0 : ((z & 2) ? 1 : 2));  // minimal value of repetition count
          t.repeat_at_least(n);                       // repetition count >= n
        }
        return t;
      }
    }
    case te_Apply: {
      if (args.size() == 1 && args[0]->tp == te_IntConst) {
        int n = args[0]->value;
        if (type_applied == NatWidth_type || type_applied == Int_type || type_applied == UInt_type ||
            type_applied == Bits_type) {
          return MinMaxSize::fixed_size(std::min(n, 2047));
        } else if (type_applied == NatLeq_type) {
          return MinMaxSize::fixed_size(32 - td::count_leading_zeroes32(n));
        } else if (type_applied == NatLess_type) {
          return MinMaxSize::fixed_size(n ? 32 - td::count_leading_zeroes32(n - 1) : 2047);
        }
      }
      return type_applied->size;
    }
  }  // end switch
  return {};
}

bool TypeExpr::compute_any_bits() const {
  if (is_nat) {
    return true;
  }
  switch (tp) {
    case te_Type:
      return true;
    case te_Param:
      return false;
    case te_Ref:
      return true;
    case te_Tuple:
    case te_CondType: {
      assert(args.size() == 2);
      int z = args[0]->abstract_interpret_nat();
      if (!(z & ~1)) {
        return true;  // always 0
      } else {
        return args[1]->compute_any_bits();
      }
    }
    case te_Apply: {
      if (args.size() == 1 && args[0]->tp == te_IntConst) {
        int n = args[0]->value;
        if (type_applied == NatLeq_type) {
          return !(n & (n + 1));
        } else if (type_applied == NatLess_type) {
          return !(n & (n - 1));
        }
      }
      return type_applied->any_bits;
    }
  }  // end switch
  return false;
}

int TypeExpr::is_integer() const {
  if (is_nat) {
    return 1;
  }
  if (tp != te_Apply) {
    return 0;
  }
  const Type* ta = type_applied;
  if (ta == Int_type) {
    return -1;
  } else if (ta == UInt_type) {
    return 1;
  }
  if (ta->is_bool) {
    return 1;
  }
  if (ta->is_builtin) {
    return ta->is_integer;
  }
  return 0;
}

bool TypeExpr::is_ref_to_anon() const {
  return tp == te_Ref && args.at(0)->is_anon();
}

bool TypeExpr::is_anon() const {
  return tp == te_Apply && args.empty() && type_applied->is_anon;
}

unsigned long long TypeExpr::compute_hash() const {
  unsigned long long h = tp * 17239ULL + value * 23917ULL + 1;
  if (type_applied) {
    h += 239017 * type_applied->type_idx;
  }
  for (const TypeExpr* arg : args) {
    h *= 170239;
    if (!arg->negated) {
      h += arg->is_constexpr;
    }
  }
  return h;
}

bool TypeExpr::detect_constexpr() {
  if (is_constexpr) {
    return true;
  }
  bool c = !negated;
  for (TypeExpr* arg : args) {
    if (!arg->detect_constexpr() && !arg->negated) {
      c = false;
    }
  }
  if (!c || tp == te_Param) {
    return false;
  }
  unsigned long long hash = compute_hash();
  unsigned long long h1 = hash % const_htable_size, h2 = 1 + hash % (const_htable_size + 1);
  while (const_htable[h1]) {
    TypeExpr* other = const_htable[h1];
    if (other->tp == tp && other->value == value && other->type_applied == type_applied &&
        other->args.size() == args.size()) {
      bool match = true;
      for (std::size_t i = 0; i < args.size(); i++) {
        if (other->args[i]->negated != args[i]->negated || other->args[i]->is_constexpr != args[i]->is_constexpr) {
          match = false;
          break;
        }
      }
      if (match) {
        is_constexpr = other->is_constexpr;
        assert(is_constexpr);
        return true;
      }
    }
    h1 += h2;
    if (h1 >= const_htable_size) {
      h1 -= const_htable_size;
    }
  }
  assert(const_type_expr_num < max_const_expr - 1);
  const_type_expr[is_constexpr = ++const_type_expr_num] = this;
  const_htable[h1] = this;
  return true;
}

void TypeExpr::const_type_name(std::ostream& os) const {
  if (negated) {
    return;
  }
  switch (tp) {
    case te_Type:
      os << "_Type";
      return;
    case te_Param:
      return;
    case te_Add:
      args[0]->const_type_name(os);
      os << "_plus";
      args[1]->const_type_name(os);
      return;
    case te_GetBit:
      args[0]->const_type_name(os);
      os << "_bit";
      args[1]->const_type_name(os);
      return;
    case te_IntConst:
      os << "_" << value;
      return;
    case te_MulConst:
      os << "_mul" << value;
      return;
    case te_Ref:
      os << "_Ref";
      args[0]->const_type_name(os);
      return;
    case te_Tuple:
      os << "_tuple";
      args[0]->const_type_name(os);
      args[1]->const_type_name(os);
      return;
    case te_CondType:
      os << "_if";
      args[0]->const_type_name(os);
      args[1]->const_type_name(os);
      return;
    case te_Apply:
      os << '_';
      if (type_applied->produces_nat) {
        if (type_applied == Nat_type) {
          os << "nat";
        } else if (type_applied == NatWidth_type) {
          os << "natwidth";
        } else if (type_applied == NatLeq_type) {
          os << "natleq";
        } else if (type_applied == NatLess_type) {
          os << "natless";
        }
      } else {
        os << type_applied->get_name();
      }
      for (const TypeExpr* arg : args) {
        arg->const_type_name(os);
      }
      return;
  }
}

bool TypeExpr::bind_value(bool value_negated, Constructor& cs, bool checking_type) {
  // if checking_type == false:
  //   negated = false, value_negated = false: compute expression, compare to value (only for integers)
  //   negated = false, value_negated = true: compute expression, return it to the "value"
  //   negated = true, value_negated = false: assign the value to the expression to compute some of the variables present in the expression
  // if checking_type == true:
  //   value_negated must be false
  //(debug output)
  //  std::cerr << "binding " << (value_negated ? "negative" : "positive") << " value to expression "
  //            << (checking_type ? "of type " : "");
  //  show(std::cerr, &cs);
  //  std::cerr << std::endl;
  //
  if (!checking_type) {
    no_tchk();
  } else {
    if (is_nat) {
      throw src::ParseError{where, "cannot use check a type against an integer expression"};
    }
    if (value_negated) {
      throw src::ParseError{where, "cannot compute a value knowing only its type"};
    }
  }
  if (negated && value_negated) {
    // both the expression and the value cannot be negative
    throw src::ParseError{where, "expression has wrong polarity"};
  }
  if (!is_nat) {
    // for type expressions:
    if (value_negated) {
      // cannot "return" values that are not integer (i.e. types)
      // throw src::ParseError{where, "cannot assign or return type expressions"};
      // in reality, this check should be only when parsing type parameters after constructors
    }
    if (!checking_type) {
      // "true" equality/assignment of type expressions
      if (!negated && !value_negated) {
        if (tp == te_Apply && args.empty()) {
          throw src::ParseError{where, "use of a global type or an undeclared variable"};
        } else {
          throw src::ParseError{where, "cannot check type expressions for equality"};
        }
      }
      // available only if the expression is a free variable
      if (negated && tp != te_Param) {
        throw src::ParseError{where, "types can be assigned only to free type variables"};
      }
    }
  }
  switch (tp) {
    case te_Add: {
      assert(is_nat && args.size() == 2 && !(args[0]->negated && args[1]->negated));
      assert(negated == (args[0]->negated || args[1]->negated));
      int i = args[0]->negated;
      args[i]->bind_value(negated, cs);
      args[1 - i]->bind_value(false, cs);
      return true;
    }
    case te_IntConst: {
      assert(is_nat && !negated);
      return true;
    }
    case te_MulConst: {
      assert(is_nat && args.size() == 1 && value > 0);
      assert(negated == args[0]->negated);
      args[0]->bind_value(value_negated, cs);
      return true;
    }
    case te_GetBit: {
      assert(is_nat && args.size() == 2 && !args[0]->negated && !args[1]->negated);
      assert(!negated);
      args[0]->bind_value(false, cs);
      args[1]->bind_value(false, cs);
      return true;
    }
    case te_Type: {
      assert(!is_nat && !negated);
      return true;
    }
    case te_Param: {
      assert(value >= 0 && value < cs.fields_num);
      Field& field = cs.fields.at(value);
      if (!negated || checking_type) {
        if (!field.known) {
          throw src::ParseError{where,
                                std::string{"variable `"} + field.get_name() + "` used before being assigned to"};
        } else {
          field.used = true;
        }
      } else if (!field.known) {
        field.known = true;
        // where.show_note(std::string{"variable `"} + field.get_name() + "` is assigned a value here");
      }
      return true;
    }
    case te_Apply:
      if (type_applied == Eq_type) {
        assert(args.size() == 2 && !(args[0]->negated && args[1]->negated));
        assert(negated == (args[0]->negated || args[1]->negated));
        int i = args[0]->negated;
        args[i]->bind_value(negated, cs);
        args[1 - i]->bind_value(false, cs);
        return true;
      } else {
        assert(!negated || checking_type);
        for (TypeExpr* arg : args) {
          if (!arg->negated) {
            arg->bind_value(true, cs);
          }
        }
        for (TypeExpr* arg : args) {
          if (arg->negated) {
            arg->bind_value(false, cs);
          }
        }
        return true;
      }
    case te_CondType:
    case te_Tuple: {
      assert(args.size() == 2);
      assert(!negated && !args[0]->negated && !args[1]->negated);
      assert(args[0]->is_nat);
      assert(!args[1]->is_nat);
      args[0]->bind_value(true, cs);
      args[1]->bind_value(true, cs);
      return true;
    }
    case te_Ref: {
      assert(args.size() == 1);
      return args[0]->bind_value(value_negated, cs, checking_type);
    }
    default:
      throw src::ParseError{where, "cannot bind a value to an expression of unknown sort"};
  }
  return true;
}

void parse_implicit_param(Lexer& lex, Constructor& cs) {
  // ident : # or ident : Type
  if (lex.tp() != src::_Ident) {
    lex.expect(src::_Ident);
  }
  Field& field = cs.new_field(lex.cur().loc, true, lex.cur().val);
  lex.next();
  lex.expect(':');
  if (lex.tp() != src::_Type && (lex.tp() != src::_Ident || lex.cur().val != Nat_name)) {
    throw src::ParseError{lex.cur().loc, "either `Type` or `#` implicit parameter type expected"};
  }
  if (lex.tp() == src::_Type) {
    field.type = &type_Type;
  } else {
    field.type = TypeExpr::mk_apply_empty(lex.cur().loc, Nat_name, Nat_type);
    assert(Nat_type->produces_nat);
    assert(!Nat_type->arity);
    assert(field.type->is_nat_subtype);
  }
  lex.next();
  field.register_sym();
}

sym::SymDef* register_new_type(const src::SrcLocation& loc, sym_idx_t name) {
  // unknown identifier, declare new type
  if (!sym::is_uc_ident(name)) {
    throw src::ParseError{loc, std::string{"implicitly defined type `"} + sym::symbols.get_name(name) +
                                   "` must begin with an uppercase letter"};
  }
  sym::SymDef* sym_def = sym::define_global_symbol(name, true, loc);
  assert(sym_def);
  types.emplace_back(types_num++, name);
  sym_def->value = new SymValType{&types.back()};
  return sym_def;
}

void show_tag(std::ostream& os, unsigned long long tag) {
  if (!tag) {
    return;
  }
  if (!(tag & ((1ULL << 59) - 1))) {
    os << '$';
    int c = 0;
    while (tag & ((1ULL << 63) - 1)) {
      os << (tag >> 63);
      tag <<= 1;
      ++c;
    }
    if (!c) {
      os << '_';
    }
  } else {
    os << '#';
    while (tag & ((1ULL << 63) - 1)) {
      static const char hex_digits[] = "0123456789abcdef";
      os << hex_digits[tag >> 60];
      tag <<= 4;
    }
    if (!tag) {
      os << '_';
    }
  }
}

void Constructor::show(std::ostream& os, int mode) const {
  if (mode & 4) {
    os << '[';
  } else {
    os << sym::symbols.get_name(constr_name);
  }
  if (!(mode & 8)) {
    show_tag(os, tag);
  }
  for (const Field& field : fields) {
    os << ' ';
    if (field.implicit || field.constraint) {
      if (!(mode & 2)) {
        os << '{';
      }
      if (field.name) {
        os << field.get_name() << ':';
      }
      field.type->show(os, this, 0, mode & ~1);
      if (!(mode & 2)) {
        os << '}';
      }
    } else {
      if (field.name) {
        os << field.get_name() << ':';
      }
      field.type->show(os, this, 95, mode & ~1);
    }
  }
  if (mode & 4) {
    os << " ]";
    return;
  }
  os << " = ";
  if (type_defined) {
    type_defined->print_name(os);
  } else {
    os << sym::symbols.get_name(type_name);
  }
  for (int i = 0; i < type_arity; i++) {
    os << ' ';
    if (param_negated.at(i)) {
      os << '~';
    }
    params.at(i)->show(os, this, 100, mode | 1);
  }
  if (!(mode & 2)) {
    os << ';';
  }
}

unsigned long long Constructor::compute_tag() const {
  std::ostringstream os;
  show(os, 10);
  unsigned crc = td::crc32(td::Slice{os.str()});
  if (verbosity > 2) {
    std::cerr << "crc32('" << os.str() << "') = " << std::hex << crc << std::dec << std::endl;
  }
  return ((unsigned long long)crc << 32) | 0x80000000;
}

bool Constructor::compute_is_fwd() {
  if (constr_name || tag_bits || type_arity || fields_num != 1) {
    return is_fwd = false;
  }
  return is_fwd = (!fields[0].implicit && !fields[0].constraint);
}

bool show_tag_warnings;

void Constructor::check_assign_tag() {
  if (constr_name && (!tag || (tag & (1ULL << 63)) == (1ULL << 63))) {
    unsigned long long computed_tag = compute_tag();
    if (!tag) {
      set_tag(computed_tag);
      if (show_tag_warnings) {
        std::ostringstream os;
        os << "constructor `" << sym::symbols.get_name(type_name) << "::" << sym::symbols.get_name(constr_name)
           << "` had no tag, assigned ";
        show_tag(os, computed_tag);
        where.show_warning(os.str());
      }
    } else if (tag != computed_tag && show_tag_warnings) {
      std::ostringstream os;
      os << "constructor `" << sym::symbols.get_name(type_name) << "::" << sym::symbols.get_name(constr_name)
         << "` has explicit tag ";
      show_tag(os, tag);
      os << " different from its computed tag ";
      show_tag(os, computed_tag);
      where.show_warning(os.str());
    }
  } else if (!constr_name && !tag) {
    set_tag(1ULL << 63);
  }
}

void Type::bind_constructor(const src::SrcLocation& loc, Constructor* cs) {
  if (is_final) {
    throw src::ParseError{loc, std::string{"cannot add new constructor `"} + sym::symbols.get_name(cs->constr_name) +
                                   "` to a finalized type `" + sym::symbols.get_name(type_name) + "`"};
  }
  if (arity < 0) {
    arity = cs->type_arity;
    assert(arity >= 0);
    args.resize(arity, 0);
  } else {
    if (arity != cs->type_arity) {
      throw src::ParseError{loc, std::string{"parametrized type `"} + sym::symbols.get_name(type_name) +
                                     "` redefined with different arity"};
    }
  }
  assert(arity == cs->type_arity && arity == (int)cs->params.size() && cs->params.size() == cs->param_negated.size());
  int true_params = 0;
  for (int i = 0; i < arity; i++) {
    auto expr = cs->params.at(i);
    bool negated = cs->param_negated.at(i);
    int& x = args[i];
    x |= (expr->is_nat ? _IsNat : _IsType);
    if ((x & (_IsNat | _IsType)) == (_IsNat | _IsType)) {
      throw src::ParseError{expr->where, std::string{"formal parameter to type `"} + sym::symbols.get_name(type_name) +
                                             "` has incorrect type"};
    }
    x |= (negated ? _IsNeg : _IsPos);
    if ((x & (_IsPos | _IsNeg)) == (_IsPos | _IsNeg)) {
      throw src::ParseError{expr->where, std::string{"formal parameter to type `"} + sym::symbols.get_name(type_name) +
                                             "` has incorrect polarity"};
    }
    if (cs->param_const_val.at(i) < 0) {
      x |= _NonConst;
    }
    true_params += !negated;
  }
  assert(cs->fields_num >= 0 && (long long)cs->fields_num == (long long)cs->fields.size());
  int explicit_fields = 0;
  for (Field& field : cs->fields) {
    if (field.constraint) {
      field.type->bind_value(false, *cs, true);
      field.known = true;
    } else if (!field.implicit) {
      ++explicit_fields;
      field.type->bind_value(false, *cs, true);
      if (!field.known) {
        // field.loc.show_note(std::string{"variable `"} + field.get_name() + "` is assigned a value here");
      }
      field.known = true;
    }
  }
  cs->is_enum = !explicit_fields;
  cs->is_simple_enum = (cs->is_enum && !true_params);
  for (int i = 0; i < arity; i++) {
    auto expr = cs->params[i];
    bool negated = cs->param_negated[i];
    if (negated) {
      expr->bind_value(true, *cs);
    }
  }
  for (Field& field : cs->fields) {
    if (!field.known) {
      throw src::ParseError{field.loc, std::string{"field `"} + field.get_name() + "` is left unbound"};
    }
  }
  if (cs->constr_name) {
    for (auto c : constructors) {
      if (c->constr_name == cs->constr_name) {
        std::string tname = sym::symbols.get_name(type_name);
        std::string cname = tname + "::" + sym::symbols.get_name(cs->constr_name);
        c->where.show_note(std::string{"constructor `"} + cname + "` first defined here");
        throw src::ParseError{cs->where, std::string{"constructor `"} + cname + "` redefined"};
      }
    }
  }
  if (!cs->type_defined && cs->type_name == type_name) {
    cs->type_defined = this;
  }
  cs->check_assign_tag();
  cs->compute_is_fwd();
  is_enum &= cs->is_enum;
  is_simple_enum &= cs->is_simple_enum;
  if (constr_num && (is_special != cs->is_special)) {
    throw src::ParseError{cs->where, std::string{"type `"} + sym::symbols.get_name(type_name) +
                                         "` has mixed special and non-special constructors"};
  }
  is_special = cs->is_special;
  ++(constr_num);
  constructors.push_back(cs);
}

bool Type::unique_constructor_equals(const Constructor& cs, bool allow_other_names) const {
  return constr_num == 1 && constructors.at(0)->isomorphic_to(cs, allow_other_names);
}

bool Constructor::isomorphic_to(const Constructor& cs, bool allow_other_names) const {
  if (constr_name != cs.constr_name || tag != cs.tag || fields_num != cs.fields_num || type_arity != cs.type_arity ||
      params.size() != cs.params.size()) {
    return false;
  }
  for (int i = 0; i < fields_num; i++) {
    if (!fields.at(i).isomorphic_to(cs.fields.at(i), allow_other_names)) {
      return false;
    }
  }
  for (std::size_t i = 0; i < params.size(); i++) {
    if (!params.at(i)->equal(*cs.params.at(i))) {
      return false;
    }
  }
  return true;
}

bool Field::isomorphic_to(const Field& f, bool allow_other_names) const {
  if (f.field_idx != field_idx || f.implicit != implicit || f.constraint != constraint ||
      (!allow_other_names && f.name != name)) {
    return false;
  }
  return f.type->equal(*type);
}

/*
 * 
 *  TL-B SOURCE PARSER
 * 
 */

void parse_field_list(Lexer& lex, Constructor& cs);

TypeExpr* parse_anonymous_constructor(Lexer& lex, Constructor& cs) {
  sym::open_scope(lex);
  Constructor* cs2 = new (AR) Constructor(lex.cur().loc);  // anonymous constructor
  parse_field_list(lex, *cs2);
  if (lex.tp() != ']') {
    lex.expect(']');
  }
  cs2->set_tag(1ULL << 63);
  for (int i = builtin_types_num; i < types_num; i++) {
    if (types.at(i).is_auto && types[i].is_final && types[i].unique_constructor_equals(*cs2)) {
      sym::close_scope(lex);
      if (types[i].parent_type_idx >= 0) {
        types[i].parent_type_idx = -2;
      }
      cs2->~Constructor();
      return TypeExpr::mk_apply_empty(lex.cur().loc, 0, &types[i]);
    }
  }
  types.emplace_back(types_num++, 0);
  Type* type = &types.back();  // anonymous type
  type->bind_constructor(lex.cur().loc, cs2);
  type->is_final = true;
  type->is_auto = true;
  type->is_anon = true;
  type->renew_last_declared();
  sym::close_scope(lex);
  return TypeExpr::mk_apply_empty(lex.cur().loc, 0, type);
}

TypeExpr* parse_expr(Lexer& lex, Constructor& cs, int mode);

// ( E ) | [ {field-def} ] | id | ~id | num | ^T
TypeExpr* parse_term(Lexer& lex, Constructor& cs, int mode) {
  if (lex.tp() == '(') {
    lex.next();
    TypeExpr* expr = parse_expr(lex, cs, mode);
    expr->check_mode(lex.cur().loc, mode);
    lex.expect(')');
    return expr;
  }
  if (lex.tp() == src::_Number) {
    TypeExpr* expr = TypeExpr::mk_intconst(lex.cur().loc, lex.cur().str);
    expr->check_mode(lex.cur().loc, mode);
    lex.next();
    return expr;
  }
  if (lex.tp() == '[') {
    lex.next();
    TypeExpr* expr = parse_anonymous_constructor(lex, cs);
    expr->check_mode(lex.cur().loc, mode);
    lex.expect(']');
    return expr;
  }
  if (lex.tp() == '^') {
    src::SrcLocation loc = lex.cur().loc;
    lex.next();
    TypeExpr* expr = parse_term(lex, cs, mode & ~2);
    expr->close(lex.cur().loc);
    if (expr->is_nat) {
      throw src::ParseError{loc, "cannot create a cell reference type to a natural number"};
    }
    return TypeExpr::mk_cellref(loc, expr);
  }
  bool negate = false;
  if (lex.tp() == '~') {
    lex.next();
    if (lex.tp() != src::_Ident) {
      lex.expect(src::_Ident, "field identifier");
    }
    negate = true;
  }
  if (lex.tp() == src::_Ident) {
    sym_idx_t name = lex.cur().val;
    sym::SymDef* sym_def = sym::lookup_symbol(name);
    if (!sym_def) {
      if (negate) {
        throw src::ParseError{lex.cur().loc, "field identifier expected"};
      }
      sym_def = register_new_type(lex.cur().loc, name);
      if (verbosity > 2) {
        std::cerr << "implicitly defined new type `" << sym::symbols.get_name(name) << "`" << std::endl;
      }
    }
    if (!sym_def->value) {
      throw src::ParseError{lex.cur().loc, "global symbol has no value"};
    }
    if (sym_def->value->type == sym::SymValBase::_Typename) {
      // found a global type identifier
      if (negate) {
        throw src::ParseError{lex.cur().loc, "cannot negate a type"};
      }
      SymValType* svt = dynamic_cast<SymValType*>(sym_def->value);
      assert(svt && svt->type_ref);
      (svt->type_ref->used)++;
      auto res = TypeExpr::mk_apply_empty(lex.cur().loc, name, svt->type_ref);
      lex.next();
      return res;
    }
    SymVal* sym_val = dynamic_cast<SymVal*>(sym_def->value);
    if (sym_def->value->type != sym::SymValBase::_Param || !sym_val) {
      throw src::ParseError{lex.cur().loc, "field identifier expected"};
    }
    if (sym_def->level != sym::scope_level) {
      throw src::ParseError{lex.cur().loc, std::string{"cannot access field `"} + lex.cur().str + "` from outer scope"};
    }
    int i = sym_val->idx;
    assert(i >= 0 && i < cs.fields_num);
    auto res = new (AR) TypeExpr{lex.cur().loc, TypeExpr::te_Param, i};
    auto field_type = cs.fields[i].type;
    assert(field_type);
    if ((mode & 4) && !cs.fields[i].known) {
      // auto-negate, used for parsing type parameters in constructor RHS
      negate = true;
    }
    res->is_nat = field_type->is_nat_subtype;
    // std::cerr << "using field " << lex.cur().str << "; is_nat_subtype = " << res->is_nat << std::endl;
    if (!res->is_nat && field_type->tp != TypeExpr::te_Type) {
      throw src::ParseError{lex.cur().loc,
                            "cannot use a field in an expression unless it is either an integer or a type"};
    }
    if (negate && !cs.fields[i].implicit) {
      throw src::ParseError{lex.cur().loc, "cannot negate an explicit field"};
    }
    res->negated = negate;
    res->check_mode(lex.cur().loc, mode);
    lex.next();
    return res;
  } else {
    lex.expect(src::_Ident, "type identifier");
    return nullptr;
  }
}

// E[.E]
TypeExpr* parse_expr97(Lexer& lex, Constructor& cs, int mode) {
  TypeExpr* expr = parse_term(lex, cs, mode | 3);
  if (lex.tp() == '.') {
    src::SrcLocation where = lex.cur().loc;
    expr->close(lex.cur().loc);
    // std::cerr << "parse ., mode " << mode << std::endl;
    if (!(mode & 2)) {
      throw src::ParseError{where, "bitfield expression cannot be used instead of a type expression"};
    }
    if (!expr->is_nat) {
      throw src::ParseError{where, "cannot apply bit selection operator `.` to types"};
    }
    lex.next();
    TypeExpr* expr2 = parse_term(lex, cs, mode & ~1);
    expr2->close(lex.cur().loc);
    if (expr->negated || expr2->negated) {
      throw src::ParseError{where, "cannot apply bit selection operator `.` to values of negative polarity"};
    }
    expr = TypeExpr::mk_apply(where, TypeExpr::te_GetBit, expr, expr2);
  }
  expr->check_mode(lex.cur().loc, mode);
  return expr;
}

// E ? E [ : E ]
TypeExpr* parse_expr95(Lexer& lex, Constructor& cs, int mode) {
  TypeExpr* expr = parse_expr97(lex, cs, mode | 3);
  if (lex.tp() != '?') {
    expr->check_mode(lex.cur().loc, mode);
    return expr;
  }
  src::SrcLocation where = lex.cur().loc;
  expr->close(where);
  if (!expr->is_nat) {
    throw src::ParseError{where, "cannot apply `?` with non-integer selectors"};
  }
  lex.next();
  TypeExpr* expr2 = parse_term(lex, cs, mode & ~10);
  expr2->close(lex.cur().loc);
  expr2->no_tchk();
  expr = TypeExpr::mk_apply(where, TypeExpr::te_CondType, expr, expr2);
  expr->check_mode(lex.cur().loc, mode);
  return expr;
}

// E E
TypeExpr* parse_expr90(Lexer& lex, Constructor& cs, int mode) {
  TypeExpr* expr = parse_expr95(lex, cs, mode | 3);
  while (lex.tp() == '(' || lex.tp() == src::_Ident || lex.tp() == src::_Number || lex.tp() == '~' || lex.tp() == '^' ||
         lex.tp() == '[') {
    TypeExpr* expr2 = parse_expr95(lex, cs, mode | 3);
    expr2->close(lex.cur().loc);
    expr = TypeExpr::mk_apply_gen(lex.cur().loc, expr, expr2);
  }
  expr->check_mode(lex.cur().loc, mode);
  return expr;
}

// E * E
TypeExpr* parse_expr30(Lexer& lex, Constructor& cs, int mode) {
  TypeExpr* expr = parse_expr90(lex, cs, mode);
  while (lex.tp() == '*') {
    src::SrcLocation where = lex.cur().loc;
    expr->close(lex.cur().loc);
    if (!expr->is_nat) {
      throw src::ParseError{where, "cannot apply `*` to types"};
    }
    lex.next();
    TypeExpr* expr2 = parse_expr90(lex, cs, mode);
    expr2->close(lex.cur().loc);
    if (expr2->is_nat) {
      expr = TypeExpr::mk_mulint(where, expr, expr2);
    } else {
      expr2->no_tchk();
      expr = TypeExpr::mk_apply(where, TypeExpr::te_Tuple, expr, expr2);
    }
  }
  expr->check_mode(lex.cur().loc, mode);
  return expr;
}

// E + E
TypeExpr* parse_expr20(Lexer& lex, Constructor& cs, int mode) {
  TypeExpr* expr = parse_expr30(lex, cs, mode);
  while (lex.tp() == '+') {
    src::SrcLocation where = lex.cur().loc;
    expr->close(lex.cur().loc);
    // std::cerr << "parse +, mode " << mode << std::endl;
    if (!(mode & 2)) {
      throw src::ParseError{where, "sum cannot be used instead of a type expression"};
    }
    if (!expr->is_nat) {
      throw src::ParseError{where, "cannot apply `+` to types"};
    }
    lex.next();
    TypeExpr* expr2 = parse_expr30(lex, cs, mode & ~1);
    expr2->close(lex.cur().loc);
    if (expr->negated && expr2->negated) {
      throw src::ParseError{where, "cannot add two values of negative polarity"};
    }
    bool negated = expr->negated | expr2->negated;
    expr = TypeExpr::mk_apply(where, TypeExpr::te_Add, expr, expr2);
    expr->negated = negated;
  }
  expr->check_mode(lex.cur().loc, mode);
  return expr;
}

// E | E = E | E <= E | E < E | E >= E | E > E
TypeExpr* parse_expr10(Lexer& lex, Constructor& cs, int mode) {
  TypeExpr* expr = parse_expr20(lex, cs, mode | 3);
  int op = lex.tp();
  if (!(op == '=' || op == '<' || op == '>' || op == src::_Leq || op == src::_Geq)) {
    expr->check_mode(lex.cur().loc, mode);
    return expr;
  }
  // std::cerr << "parse <=>, mode " << mode << std::endl;
  sym_idx_t op_name = lex.cur().val;
  src::SrcLocation where = lex.cur().loc;
  expr->close(where);
  if (!(mode & 1)) {
    throw src::ParseError{where, "comparison result used as an integer"};
  }
  if (!expr->is_nat) {
    throw src::ParseError{where, "cannot apply integer comparison to types"};
  }
  lex.next();
  TypeExpr* expr2 = parse_expr20(lex, cs, (mode & ~1) | 2);
  expr2->close(lex.cur().loc);
  if (!expr2->is_nat) {
    throw src::ParseError{lex.cur().loc, "cannot apply integer comparison to types"};
  }
  if (op == '>') {
    std::swap(expr, expr2);
    op = '<';
    op_name = Less_name;
  } else if (op == src::_Geq) {
    std::swap(expr, expr2);
    op = src::_Leq;
    op_name = Leq_name;
  }
  auto sym_def = sym::lookup_symbol(op_name, 2);
  assert(sym_def);
  auto sym_val = dynamic_cast<SymValType*>(sym_def->value);
  assert(sym_val);
  auto expr0 = TypeExpr::mk_apply_empty(where, op_name, sym_val->type_ref);
  expr = TypeExpr::mk_apply_gen(where, std::move(expr0), expr);
  expr = TypeExpr::mk_apply_gen(lex.cur().loc, expr, std::move(expr2));
  expr->check_mode(lex.cur().loc, mode);
  return expr;
}

TypeExpr* parse_expr(Lexer& lex, Constructor& cs, int mode) {
  return parse_expr10(lex, cs, mode);
}

void parse_param(Lexer& lex, Constructor& cs, bool named) {
  // [ ( ident | _ ) : ] type-expr
  src::SrcLocation loc = lex.cur().loc;
  if (named && lex.tp() == '_') {
    lex.next();
    lex.expect(':');
    named = false;
  }
  sym_idx_t param_name = 0;
  if (named) {
    if (lex.tp() != src::_Ident) {
      lex.expect(src::_Ident);
    }
    param_name = lex.cur().val;
    lex.next();
    lex.expect(':');
  }
  Field& field = cs.new_field(loc, false, param_name);
  field.type = parse_expr95(lex, cs, 9);  // must be a type expression
  field.type->close(lex.cur().loc);
  field.type->detect_constexpr();
  field.subrec = field.type->is_ref_to_anon();
  CHECK(!field.name || !field.subrec);
  field.register_sym();
}

void parse_constraint(Lexer& lex, Constructor& cs) {
  Field& field = cs.new_field(lex.cur().loc, true, 0);
  field.type = parse_expr(lex, cs, 9);  // must be a type expression
  field.type->close(lex.cur().loc);
  field.type->detect_constexpr();
  field.constraint = true;
  field.register_sym();
}

void parse_field_list(Lexer& lex, Constructor& cs) {
  while (lex.tp() != '=' && lex.tp() != ']') {
    if (lex.tp() == '{') {
      // either an implicit parameter or a constraint
      lex.next();
      if (lex.tp() == src::_Ident && lex.peek().tp == ':') {
        parse_implicit_param(lex, cs);
      } else {
        parse_constraint(lex, cs);
      }
      lex.expect('}');
    } else if ((lex.tp() == src::_Ident || lex.tp() == '_') && lex.peek().tp == ':') {
      parse_param(lex, cs, true);
    } else {
      parse_param(lex, cs, false);
    }
  }
}

void parse_constructor_def(Lexer& lex) {
  if (lex.tp() != '_' && (lex.tp() != src::_Ident || !sym::is_lc_ident(lex.cur().val))) {
    throw src::ParseError{lex.cur().loc, "constructor name lowercase identifier expected"};
  }
  bool is_special = sym::is_spec_lc_ident(lex.cur().val);
  sym::open_scope(lex);
  Lexem constr_lex = lex.cur();
  int orig_types_num = types_num;
  sym_idx_t constr_name = (lex.tp() == src::_Ident ? lex.cur().val : 0);
  src::SrcLocation where = lex.cur().loc;
  lex.next();
  unsigned long long tag = 0;
  if (lex.tp() == src::_Special) {
    tag = src::get_special_value(lex.cur().str);
    assert(tag);
    lex.next();
  }
  //  LOG(ERROR) << "parsing constructor `" << sym::symbols.get_name(constr_name) << "` with tag " << std::hex << tag
  //             << std::dec;
  auto cs_ref = new (AR) Constructor(where, constr_name, 0, tag);
  Constructor& cs = *cs_ref;
  cs.is_special = is_special;
  parse_field_list(lex, cs);
  lex.expect('=');
  if (lex.tp() != src::_Ident || !sym::is_uc_ident(lex.cur().val)) {
    throw src::ParseError{lex.cur().loc, "type name uppercase identifier expected"};
  }
  Lexem type_lex = lex.cur();
  sym_idx_t type_name = lex.cur().val;
  sym::SymDef* sym_def = sym::lookup_symbol(type_name, 2);
  if (!sym_def) {
    sym_def = register_new_type(lex.cur().loc, type_name);
    if (verbosity > 2) {
      LOG(ERROR) << "defined new type `" << sym::symbols.get_name(type_name) << "`";
    }
    assert(sym_def);
  }
  if (!sym_def || !sym_def->value || sym_def->value->type != sym::SymValBase::_Typename) {
    throw src::ParseError{lex.cur().loc, "parametrized type identifier expected"};
  }
  SymValType* sym_val = dynamic_cast<SymValType*>(sym_def->value);
  assert(sym_val);
  cs.type_name = type_name;
  cs.type_arity = 0;
  Type* type = cs.type_defined = sym_val->type_ref;
  if (type->is_final) {
    throw src::ParseError{lex.cur().loc,
                          std::string{"cannot add new constructor to a finalized type `"} + lex.cur().str + "`"};
  }
  lex.next();
  while (lex.tp() != ';') {
    bool negate = (lex.tp() == '~');
    if (negate) {
      lex.next();
    }
    TypeExpr* type_param = parse_term(lex, cs, negate ? 3 : 7);
    type_param->close(lex.cur().loc);
    int const_val = (!negate && type_param->tp == TypeExpr::te_IntConst) ? type_param->value : -1;
    if (!negate) {
      //std::cerr << "binding value to type parameter expression ";
      //type_param->show(std::cerr, &cs);
      //std::cerr << std::endl;
      type_param->bind_value(negate, cs);
    } else if (!type_param->is_nat) {
      throw src::ParseError{type_param->where, "cannot return type expressions"};
    }
    cs.params.push_back(type_param);
    cs.param_negated.push_back(negate);
    cs.param_const_val.push_back(const_val);
    ++cs.type_arity;
  }
  if (lex.tp() != ';') {
    lex.expect(';');
  }
  type->bind_constructor(lex.cur().loc, cs_ref);
  type->renew_last_declared();
  lex.expect(';');
  sym::close_scope(lex);
  for (int i = orig_types_num; i < types_num; i++) {
    if (types.at(i).is_auto && types[i].parent_type_idx == -1) {
      types[i].parent_type_idx = type->type_idx;
    }
  }
}

/*
 * 
 *  SOURCE PARSER (TOP LEVEL)
 * 
 */

std::vector<const src::FileDescr*> source_fdescr;

bool parse_source(std::istream* is, src::FileDescr* fdescr) {
  src::SourceReader reader{is, fdescr};
  src::Lexer lex{reader, true, "(){}:;? #$. ^~ #", "//", "/*", "*/", ""};
  while (lex.tp() != src::_Eof) {
    parse_constructor_def(lex);
    //    LOG(ERROR) << lex.cur().str << '\t' << lex.cur().name_str();
  }
  return true;
}

bool parse_source_file(const char* filename) {
  if (!filename || !*filename) {
    throw src::Fatal{"source file name is an empty string"};
  }
  src::FileDescr* cur_source = new src::FileDescr{filename};
  source_fdescr.push_back(cur_source);
  std::ifstream ifs{filename};
  if (ifs.fail()) {
    throw src::Fatal{std::string{"cannot open source file `"} + filename + "`"};
  }
  return parse_source(&ifs, cur_source);
}

bool parse_source_stdin() {
  src::FileDescr* cur_source = new src::FileDescr{"stdin", true};
  source_fdescr.push_back(cur_source);
  return parse_source(&std::cin, cur_source);
}

bool parse_source_string(const std::string& tlb_code) {
  src::FileDescr* cur_source = new src::FileDescr{"stdin", true};
  source_fdescr.push_back(cur_source);
  std::istringstream iss(tlb_code);
  return parse_source(&iss, cur_source);
}

/*
 * 
 *   BUILT-IN TYPE DEFINITIONS
 * 
 */

Type* define_builtin_type(std::string name_str, std::string args, bool produces_nat, int size = -1, int min_size = -1,
                          bool any_bits = false, int is_int = 0) {
  sym_idx_t name = sym::symbols.lookup_add(name_str);
  assert(name_str.size() && name);
  int arity = (int)args.size();
  types.emplace_back(types_num++, name, produces_nat, arity, true, true);
  auto type = &types.back();
  type->args.resize(arity, 0);
  int f = (name_str != "#" ? Type::_IsPos : 0);
  for (int i = 0; i < arity; i++) {
    type->args[i] = f | (args[i] == '#' ? Type::_IsNat : Type::_IsType);
  }
  if (is_int) {
    type->is_integer = (char)is_int;
  }
  auto sym_def = sym::define_global_symbol(name, true);
  assert(sym_def);
  sym_def->value = new (AR) SymValType{type};
  if (size < 0) {
    type->size = MinMaxSize::Any;
  } else if (min_size >= 0 && min_size != size) {
    type->size = MinMaxSize::size_range(min_size, size);
  } else {
    type->size = MinMaxSize::fixed_size(size);
    type->has_fixed_size = true;
  }
  type->any_bits = any_bits;
  return type;
}

Type* lookup_type(std::string name_str) {
  sym_idx_t name = sym::symbols.lookup(name_str);
  if (name) {
    auto sym_def = sym::lookup_symbol(name);
    if (sym_def) {
      auto sym_val = dynamic_cast<SymValType*>(sym_def->value);
      if (sym_val) {
        return sym_val->type_ref;
      }
    }
  }
  return nullptr;
}

void define_builtins() {
  types.reserve(10000);
  Nat_type = define_builtin_type("#", "", true, 32, 32, true);
  NatWidth_type = define_builtin_type("##", "#", true, 32, 0, true);
  NatLess_type = define_builtin_type("#<", "#", true, 32, 0);
  NatLeq_type = define_builtin_type("#<=", "#", true, 32, 0);
  Any_type = define_builtin_type("Any", "", false);
  Cell_type = define_builtin_type("Cell", "", false);
  Int_type = define_builtin_type("int", "#", false, 257, 0, true, -1);
  UInt_type = define_builtin_type("uint", "#", false, 256, 0, true, 1);
  Bits_type = define_builtin_type("bits", "#", false, 1023, 0, true, 0);
  for (int i = 1; i <= 257; i++) {
    char buff[8];
    sprintf(buff, "uint%d", i);
    define_builtin_type(buff + 1, "", false, i, i, true, -1);
    if (i < 257) {
      define_builtin_type(buff, "", false, i, i, true, 1);
    }
  }
  for (int i = 1; i <= 1023; i++) {
    char buff[12];
    sprintf(buff, "bits%d", i);
    define_builtin_type(buff, "", false, i, i, true, 0);
  }
  Eq_type = define_builtin_type("=", "##", false, 0, 0, true);
  Less_type = define_builtin_type("<", "##", false, 0, 0, true);
  Leq_type = define_builtin_type("<=", "##", false, 0, 0, true);
  Nat_name = sym::symbols.lookup("#");
  Eq_name = sym::symbols.lookup("=");
  Less_name = sym::symbols.lookup("<");
  Leq_name = sym::symbols.lookup("<=");
  builtin_types_num = types_num;
}

/*
 * 
 *  SCHEME PROCESSING AND CHECKING
 * 
 */

bool Type::cons_all_exact() const {
  unsigned long long sum = 0;
  for (const auto& cons : constructors) {
    sum += (1ULL << (63 - cons->tag_bits));
  }
  return sum == (1ULL << 63);
}

int Type::cons_common_len() const {
  if (!constr_num) {
    return -1;
  }
  int len = constructors.at(0)->tag_bits;
  for (const auto cons : constructors) {
    if (cons->tag_bits != len) {
      return -1;
    }
  }
  return len;
}

bool Constructor::compute_admissible_params() {
  int dim = 0;
  int abs_param[4];
  for (std::size_t i = 0; i < params.size(); i++) {
    if (!param_negated[i] && params[i]->is_nat) {
      int t = params[i]->abstract_interpret_nat();
      assert(t >= 0 && t <= 15);
      // std::cerr << "abstract_interpret( " << params[i] << " ) = " << t << std::endl;
      abs_param[dim++] = t;
      if (!t) {
        admissible_params.clear_all();
        return false;
      }
      if (dim == 4) {
        break;
      }
    }
  }
  while (dim > 0 && abs_param[dim - 1] == 15) {
    --dim;
  }
  if (!dim) {
    admissible_params.set_all();
    return true;
  }
  admissible_params.set_by_pattern(dim, abs_param);
  return true;
}

bool Type::compute_admissible_params() {
  bool admissible = false;
  for (Constructor* cs : constructors) {
    admissible |= cs->compute_admissible_params();
    admissible_params |= cs->admissible_params;
  }
  return admissible;
}

void compute_admissible_params() {
  for (int i = builtin_types_num; i < types_num; i++) {
    (void)types[i].compute_admissible_params();
  }
}

bool Constructor::recompute_begins_with() {
  for (const Field& field : fields) {
    if (!field.implicit && !field.constraint) {
      TypeExpr* expr = field.type;
      if (expr->tp == TypeExpr::te_Ref) {
        continue;
      }
      if (expr->tp != TypeExpr::te_Apply) {
        break;
      }
      BitPfxCollection add = expr->type_applied->begins_with * tag;
      return (begins_with += add);
    }
  }
  BitPfxCollection add{tag};
  if (begins_with == add) {
    return false;
  }
  begins_with += add;
  return true;
}

bool Type::recompute_begins_with() {
  bool changes = false;
  for (Constructor* cs : constructors) {
    if (cs->recompute_begins_with()) {
      changes |= (begins_with += cs->begins_with);
    }
  }
  return changes;
}

void compute_begins_with() {
  bool changes = true;
  while (changes) {
    changes = false;
    for (int i = builtin_types_num; i < types_num; i++) {
      changes |= types[i].recompute_begins_with();
    }
  }
}

bool Constructor::recompute_minmax_size() {
  MinMaxSize sz = MinMaxSize::fixed_size(tag_bits);
  for (const Field& field : fields) {
    if (!field.implicit && !field.constraint) {
      sz += field.type->compute_size();
    }
  }
  if (sz == size) {
    return false;
  }
  size = sz;
  has_fixed_size = sz.is_fixed();
  return true;
}

bool Type::recompute_minmax_size() {
  MinMaxSize sz;
  bool changes = false;
  for (Constructor* cs : constructors) {
    changes |= cs->recompute_minmax_size();
    sz |= cs->size;
  }
  if (sz == size) {
    return changes;
  }
  size = sz;
  has_fixed_size = sz.is_fixed();
  return true;
}

void compute_minmax_sizes() {
  bool changes = true;
  while (changes) {
    changes = false;
    for (int i = builtin_types_num; i < types_num; i++) {
      changes |= types[i].recompute_minmax_size();
    }
  }
}

bool Constructor::recompute_any_bits() {
  bool res = true;
  for (const Field& field : fields) {
    if (!field.implicit && !field.constraint) {
      res &= field.type->compute_any_bits();
    }
  }
  if (res == any_bits) {
    return false;
  }
  any_bits = res;
  return true;
}

bool Type::recompute_any_bits() {
  bool res = begins_with.is_all();
  bool changes = false;
  for (Constructor* cs : constructors) {
    changes |= cs->recompute_any_bits();
    res &= cs->any_bits;
  }
  if (res == any_bits) {
    return changes;
  }
  any_bits = res;
  return true;
}

void compute_any_bits() {
  bool changes = true;
  while (changes) {
    changes = false;
    for (int i = builtin_types_num; i < types_num; i++) {
      changes |= types[i].recompute_any_bits();
    }
  }
}

void Type::detect_basic_types() {
  if (!arity && constr_num > 0 && size.is_fixed() && any_bits) {
    is_unit = !size.min_size();
    is_bool = (size.min_size() == 0x100);
  }
}

void detect_basic_types() {
  for (int i = builtin_types_num; i < types_num; i++) {
    types[i].detect_basic_types();
  }
}

int show_size_warnings() {
  int errors = 0;
  for (int i = builtin_types_num; i < types_num; i++) {
    Type& type = types[i];
    if (!type.size.fits_into_cell() || !type.size.is_possible()) {
      std::cerr << "error: type `" << type.get_name() << "`"
                << (!type.size.is_possible() ? " cannot be instantiated" : " never fits into a cell") << " (size "
                << type.size << ")\n";
      ++errors;
    }
    for (Constructor* cs : type.constructors) {
      if (!cs->size.fits_into_cell() || !cs->size.is_possible()) {
        std::cerr << "error: constructor `" << cs->get_qualified_name() << "`"
                  << (!cs->size.is_possible() ? " cannot be instantiated" : " never fits into a cell") << " (size "
                  << cs->size << ")\n";
        cs->show(std::cerr);
        std::cerr << std::endl;
        cs->where.show_note("defined here");
        ++errors;
      }
    }
  }
  return errors;
}

bool Type::is_const_arg(int p) const {
  return (args.at(p) & (_IsType | _IsNat | _IsPos | _IsNeg | _NonConst)) == (_IsNat | _IsPos);
}

int Type::detect_const_params() {
  for (int i = 0; i < arity; i++) {
    if (is_const_arg(i)) {
      return const_param_idx = i;
    }
  }
  return const_param_idx = -1;
}

std::vector<int> Type::get_all_param_values(int p) const {
  if (p < 0 || p >= arity) {
    return {};
  }
  std::vector<int> res;
  res.reserve(constr_num);
  for (const Constructor* cs : constructors) {
    res.push_back(cs->param_const_val.at(p));
  }
  std::sort(res.begin(), res.end());
  res.erase(std::unique(res.begin(), res.end()), res.end());
  return res;
}

std::vector<int> Type::get_constr_by_param_value(int p, int pv) const {
  std::vector<int> res;
  if (p < 0 || p >= arity) {
    return res;
  }
  for (int i = 0; i < constr_num; i++) {
    if (constructors[i]->param_const_val[p] == pv) {
      res.push_back(i);
    }
  }
  return res;
}

void Type::compute_constructor_trie() {
  if (cs_trie || !constr_num) {
    return;
  }
  unsigned long long z = 1;
  for (Constructor* cs : constructors) {
    if (!z) {
      throw src::ParseError{cs->where,
                            std::string{"cannot work with more than 64 constructors for type `"} + get_name() + "`"};
    }
    cs_trie = BinTrie::insert_paths(std::move(cs_trie), cs->begins_with, z);
    z <<= 1;
  }
  if (cs_trie) {
    useful_depth = cs_trie->compute_useful_depth();
    is_pfx_determ = !cs_trie->find_conflict_path();
  } else {
    useful_depth = 0;
    is_pfx_determ = true;
  }
}

bool Type::check_conflicts() {
  compute_constructor_trie();
  int cp = detect_const_params();
  is_param_pfx_determ = is_param_determ = is_determ = true;
  is_const_param_determ = is_const_param_pfx_determ = (cp >= 0);
  if (!constr_num || !cs_trie) {
    return false;
  }
  assert(constr_num <= 64);
  // std::cerr << "prefix trie for constructors of `" << get_name() << "`" << std::endl;
  // cs_trie->show(std::cerr);
  ConflictGraph pfx_cg;
  cs_trie->set_conflict_graph(pfx_cg);
  for (int i = 0; i < constr_num; i++) {
    AdmissibilityInfo& ap1 = constructors[i]->admissible_params;
    for (int j = 0; j < i; j++) {
      bool cp_same = (constructors[i]->get_const_param(cp) == constructors[j]->get_const_param(cp));
      if (cp_same) {
        is_const_param_determ = false;
        if (pfx_cg[i][j]) {
          is_const_param_pfx_determ = false;
        }
      }
      if (ap1.conflicts_with(constructors[j]->admissible_params)) {
        is_param_determ = false;
        if (pfx_cg[i][j]) {
          is_param_pfx_determ = false;
          if (cp_same) {
            conflict1 = j;
            conflict2 = i;
            is_determ = false;
          }
        }
      }
    }
  }
  return !is_determ;
}

void Type::show_constructor_conflict() {
  assert(cs_trie);
  assert(conflict1 != conflict2);
  int i = conflict1, j = conflict2;
  assert(i >= 0 && i <= j && j < 64 && j < constr_num);
  unsigned long long mask = (1ULL << i) | (1ULL << j);
  unsigned long long pfx = cs_trie->find_conflict_path(0, mask);
  assert(pfx);
  ConflictSet cs_set{cs_trie->lookup_tag(pfx)};
  std::cerr << "found conflict between constructors of type `" << get_name() << "`: prefix ";
  show_tag(std::cerr, pfx);
  AdmissibilityInfo& info1 = constructors[i]->admissible_params;
  AdmissibilityInfo& info2 = constructors[j]->admissible_params;
  bool need_params = !(info1.is_set_all() && info2.is_set_all());
  int params = info1.conflicts_at(info2);
  assert(params >= 0);
  for (int s = 0; s < 64 && s < constr_num; s++) {
    if (cs_set[s] &&
        !(need_params ? constructors[s]->admissible_params[params] : constructors[s]->admissible_params.is_set_all())) {
      cs_set.remove(s);
    }
  }
  assert(cs_set[i] && cs_set[j]);
  std::cerr << " can be present in " << cs_set.size() << " constructors:" << std::endl;
  for (int s = 0; s < 64 && s < constr_num; s++) {
    if (cs_set[s]) {
      std::cerr << "\t";
      constructors[s]->show(std::cerr);
      std::cerr << std::endl;
      constructors[s]->where.show_note("defined here");
    }
  }
  if (need_params) {
    std::cerr << "when type parameters are instantiated as " << get_name();
    char nat = 'a', t = 'A';
    for (int x : args) {
      if (x & _IsNeg) {
        std::cerr << " ~" << (x & _IsNat ? nat++ : t++);
      } else if (x & _IsType) {
        std::cerr << ' ' << t++;
      } else {
        std::cerr << ' ' << (params & 3);
        if (params & 2) {
          std::cerr << "+2*" << nat++;
        }
      }
    }
    std::cerr << std::endl;
  }
}

int check_conflicts() {
  int c = 0;
  for (int i = builtin_types_num; i < types_num; i++) {
    if (types[i].check_conflicts()) {
      ++c;
      types[i].show_constructor_conflict();
    }
  }
  return c;
}

void check_scheme() {
  compute_admissible_params();
  compute_begins_with();
  compute_minmax_sizes();
  compute_any_bits();
  detect_basic_types();
  if (show_size_warnings()) {
    throw src::Fatal{"invalid scheme: some constructors or types cannot be instantiated or do not fit into cells"};
  }
  if (check_conflicts()) {
    throw src::Fatal{"invalid scheme: have conflicts between constructors of some types"};
  }
}

void dump_all_types() {
  std::cerr << types_num << " types defined, out of them " << builtin_types_num << " built-in, "
            << types_num - builtin_types_num << " user-defined\n";
  for (int i = 0; i < builtin_types_num; i++) {
    Type& type = types[i];
    if (type.used) {
      std::cerr << "built-in type #" << i << ": `" << type.get_name() << "`, arity " << type.arity << "; prefixes "
                << type.begins_with << "; size " << type.size;
      if (type.is_unit) {
        std::cerr << " (UNIT)";
      }
      if (type.is_bool) {
        std::cerr << " (BOOL)";
      }
      std::cerr << std::endl;
    }
  }
  for (int i = builtin_types_num; i < types_num; i++) {
    Type& type = types[i];
    std::cerr << "type #" << i << ": `" << type.get_name() << "`, arity " << type.arity << ", " << type.constr_num
              << " constructors\n";
    if (type.const_param_idx >= 0) {
      std::cerr << "  constant parameters:";
      for (int j = 0; j < type.arity; j++) {
        std::cerr << (type.is_const_arg(j) ? " const" : " *");
      }
      std::cerr << std::endl;
    }
    for (Constructor* cs : type.constructors) {
      std::cerr << "  constructor `" << cs->get_name() << "`" << (cs->is_fwd ? " (simple forwarder)\n\t" : "\n\t");
      cs->show(std::cerr);
      std::cerr << "\n\tbegins with " << cs->begins_with << std::endl;
      if (!cs->admissible_params.is_set_all()) {
        std::cerr << "\tadmissibility " << cs->admissible_params << std::endl;
      }
      if (type.const_param_idx >= 0) {
        std::cerr << "\tconstant parameter #" << type.const_param_idx + 1 << " = "
                  << cs->get_const_param(type.const_param_idx) << std::endl;
      }
      std::cerr << "\tsize " << cs->size << (cs->has_fixed_size ? " (fixed)" : "")
                << (cs->any_bits ? " (any bits)" : "") << std::endl;
      for (const Field& field : cs->fields) {
        std::cerr << "\t\tfield `" << field.get_name() << "`: " << field.type << " (used=" << field.used
                  << ") (is_nat_subtype=" << field.type->is_nat_subtype << ")\n";
      }
    }
    if (type.is_unit) {
      std::cerr << "  (UNIT)\n";
    }
    if (type.is_bool) {
      std::cerr << "  (BOOL)\n";
    }
    if (type.is_enum) {
      std::cerr << (type.is_simple_enum ? "  (SIMPLE ENUM)" : "  (ENUM)") << std::endl;
    }
    if (type.constr_num > 1) {
      std::cerr << "  constructor detection: ";
      if (type.is_pfx_determ) {
        std::cerr << "PFX(" << type.useful_depth << ") ";
      }
      if (type.is_param_determ) {
        std::cerr << "PARAM ";
      }
      if (type.is_const_param_determ) {
        std::cerr << "CONST_PARAM ";
      }
      if (type.is_const_param_pfx_determ && !type.is_pfx_determ && !type.is_const_param_determ) {
        std::cerr << "PFX(" << type.useful_depth << ")+CONST_PARAM ";
      }
      if (type.is_param_pfx_determ && !type.is_pfx_determ && !type.is_param_determ && !type.is_const_param_pfx_determ) {
        std::cerr << "PFX(" << type.useful_depth << ")+PARAM ";
      }
      if (type.is_determ && !type.is_const_param_pfx_determ && !type.is_param_pfx_determ) {
        std::cerr << "PFX(" << type.useful_depth << ")+CONST_PARAM+PARAM ";
      }
      if (!type.is_determ) {
        std::cerr << "<CONFLICT>";
      }
      std::cerr << std::endl;
    }
    std::cerr << "  type size " << type.size << (type.has_fixed_size ? " (fixed)" : "")
              << (type.any_bits ? " (any bits)" : "") << std::endl;
    std::cerr << "  type begins with " << type.begins_with << std::endl;
    if (!type.admissible_params.is_set_all()) {
      std::cerr << "  type admissibility " << type.admissible_params << std::endl;
    }
    std::cerr << std::endl;
    if (!type.constr_num && !type.is_final) {
      sym::SymDef* sym_def = sym::lookup_symbol(type.type_name);
      assert(sym_def);
      throw src::ParseError{
          sym_def ? sym_def->loc : src::SrcLocation{},
          std::string{"implicitly defined type `"} + sym::symbols.get_name(type.type_name) + "` has no constructors"};
    }
  }
}

void dump_all_constexpr() {
  std::cerr << "****************\n" << const_type_expr_num << " constant expressions:\n";
  for (int i = 1; i <= const_type_expr_num; i++) {
    std::cerr << "expr #" << i << ": " << const_type_expr[i] << std::endl;
  }
}

/*
 * 
 *   CODE GENERATION
 * 
 */

std::vector<std::string> source_list;

void register_source(std::string source) {
  source_list.push_back(source);
}

void clear_for_redefine() {
  // if we generate several times tlb code in runtime need to clear stuff
  sym::clear_sym_def();
  sym::symbols.clear();
  types.clear();
  source_list.clear();
  source_fdescr.clear();
  global_cpp_ids.clear();

  std::memset(const_htable, 0, sizeof(const_htable));
  std::memset(const_type_expr, 0, sizeof(const_type_expr));

  types_num = 0;
  builtin_types_num = 0;
  const_type_expr_num = 0;
}

int a;

std::string codegen_python_tlb(const std::string& tlb_text) {
  a++;
  clear_for_redefine();

  src::define_keywords();
  tlbc::define_builtins();
  tlbc::init_abstract_tables();

  tlbc::parse_source_string(tlb_text);
  tlbc::check_scheme();

  std::stringstream ss;
  tlbc::generate_py_output(ss, 0);
  source_fdescr.pop_back();

  return ss.str();
}

}  // namespace tlbc

#include "tlbc-gen-cpp.cpp"
#include "tlbc-gen-py.cpp"

/*
 * 
 *   TLBC MAIN
 * 
 */

void usage(const char* progname) {
  std::cerr << "usage: " << progname
            << " [-v][-i][-h][-c][-z][-t][-T][-q][-p][-n<namespace>][-o<output-filename>] {<tlb-filename> ...}\n"
            << "-v\tIncrease verbosity level\n"
            << "-t\tShow tag mismatch warnings\n"
            << "-p\tGenerate Python code\n"
            << "-q\tOmit code generation (TLB scheme check only)\n"
            << "-h\tGenerate C++ header file only (usually .h or .hpp)\n"
            << "-c\tGenerate C++ source file only (usually .cpp)\n"
            << "-T\tAdd type pointer members into generated C++ data record classes\n"
            << "-z\tAppend .cpp or .hpp to output filename\n"
            << "-n<namespace>\tPut generated code into specified namespace (default `tlb`)\n";
  std::exit(2);
}

std::string output_filename;

int main(int argc, char* const argv[]) {
  tlbc::codegen_python_tlb(
      "unit$_ = Unit;\n"
      "true$_ = Truet;\n"
      "// EMPTY False;\n"
      "bool_false$0 = Bool;\n"
      "bool_true$1 = Bool;\n"
      "bool_false$0 = BoolFalse;\n"
      "bool_true$1 = BoolTrue;\n"
      "nothing$0 {X:Type} = Maybe X;\n"
      "just$1 {X:Type} value:X = Maybe X;\n"
      "left$0 {X:Type} {Y:Type} value:X = Either X Y;\n"
      "right$1 {X:Type} {Y:Type} value:Y = Either X Y;\n"
      "pair$_ {X:Type} {Y:Type} first:X second:Y = Both X Y;\n"
      "\n"
      "bit$_ (## 1) = Bit;\n"
      "/*\n"
      " *\n"
      " *   FROM hashmap.tlb\n"
      " *\n"
      " */\n"
      "// ordinary Hashmap / HashmapE, with fixed length keys\n"
      "//\n"
      "hm_edge#_ {n:#} {X:Type} {l:#} {m:#} label:(HmLabel ~l n)\n"
      "          {n = (~m) + l} node:(HashmapNode m X) = Hashmap n X;\n"
      "\n"
      "hmn_leaf#_ {X:Type} value:X = HashmapNode 0 X;\n"
      "hmn_fork#_ {n:#} {X:Type} left:^(Hashmap n X)\n"
      "           right:^(Hashmap n X) = HashmapNode (n + 1) X;\n"
      "\n"
      "hml_short$0 {m:#} {n:#} len:(Unary ~n) {n <= m} s:(n * Bit) = HmLabel ~n m;\n"
      "hml_long$10 {m:#} n:(#<= m) s:(n * Bit) = HmLabel ~n m;\n"
      "hml_same$11 {m:#} v:Bit n:(#<= m) = HmLabel ~n m;\n"
      "\n"
      "unary_zero$0 = Unary ~0;\n"
      "unary_succ$1 {n:#} x:(Unary ~n) = Unary ~(n + 1);\n"
      "\n"
      "hme_empty$0 {n:#} {X:Type} = HashmapE n X;\n"
      "hme_root$1 {n:#} {X:Type} root:^(Hashmap n X) = HashmapE n X;\n"
      "\n"
      "// true#_ = True;\n"
      "_ {n:#} _:(Hashmap n Truet) = BitstringSet n;\n"
      "\n"
      "//  HashmapAug, hashmap with an extra value\n"
      "//   (augmentation) of type Y at every node\n"
      "//\n"
      "ahm_edge#_ {n:#} {X:Type} {Y:Type} {l:#} {m:#}\n"
      "  label:(HmLabel ~l n) {n = (~m) + l}\n"
      "  node:(HashmapAugNode m X Y) = HashmapAug n X Y;\n"
      "ahmn_leaf#_ {X:Type} {Y:Type} extra:Y value:X = HashmapAugNode 0 X Y;\n"
      "ahmn_fork#_ {n:#} {X:Type} {Y:Type} left:^(HashmapAug n X Y)\n"
      "  right:^(HashmapAug n X Y) extra:Y = HashmapAugNode (n + 1) X Y;\n"
      "\n"
      "ahme_empty$0 {n:#} {X:Type} {Y:Type} extra:Y\n"
      "          = HashmapAugE n X Y;\n"
      "ahme_root$1 {n:#} {X:Type} {Y:Type} root:^(HashmapAug n X Y)\n"
      "  extra:Y = HashmapAugE n X Y;\n"
      "\n"
      "// VarHashmap / VarHashmapE, with variable-length keys\n"
      "//\n"
      "vhm_edge#_ {n:#} {X:Type} {l:#} {m:#} label:(HmLabel ~l n)\n"
      "           {n = (~m) + l} node:(VarHashmapNode m X)\n"
      "           = VarHashmap n X;\n"
      "vhmn_leaf$00 {n:#} {X:Type} value:X = VarHashmapNode n X;\n"
      "vhmn_fork$01 {n:#} {X:Type} left:^(VarHashmap n X)\n"
      "             right:^(VarHashmap n X) value:(Maybe X)\n"
      "             = VarHashmapNode (n + 1) X;\n"
      "vhmn_cont$1 {n:#} {X:Type} branch:Bit child:^(VarHashmap n X)\n"
      "            value:X = VarHashmapNode (n + 1) X;\n"
      "\n"
      "// nothing$0 {X:Type} = Maybe X;\n"
      "// just$1 {X:Type} value:X = Maybe X;\n"
      "\n"
      "vhme_empty$0 {n:#} {X:Type} = VarHashmapE n X;\n"
      "vhme_root$1 {n:#} {X:Type} root:^(VarHashmap n X)\n"
      "            = VarHashmapE n X;\n"
      "\n"
      "//\n"
      "// PfxHashmap / PfxHashmapE, with variable-length keys\n"
      "//                           constituting a prefix code\n"
      "//\n"
      "\n"
      "phm_edge#_ {n:#} {X:Type} {l:#} {m:#} label:(HmLabel ~l n)\n"
      "           {n = (~m) + l} node:(PfxHashmapNode m X)\n"
      "           = PfxHashmap n X;\n"
      "\n"
      "phmn_leaf$0 {n:#} {X:Type} value:X = PfxHashmapNode n X;\n"
      "phmn_fork$1 {n:#} {X:Type} left:^(PfxHashmap n X)\n"
      "            right:^(PfxHashmap n X) = PfxHashmapNode (n + 1) X;\n"
      "\n"
      "phme_empty$0 {n:#} {X:Type} = PfxHashmapE n X;\n"
      "phme_root$1 {n:#} {X:Type} root:^(PfxHashmap n X)\n"
      "            = PfxHashmapE n X;\n"
      "/*\n"
      " *\n"
      " *  END hashmap.tlb\n"
      " *\n"
      " */\n"
      "//\n"
      "// TON BLOCK LAYOUT\n"
      "//\n"
      "addr_none$00 = MsgAddressExt;\n"
      "addr_extern$01 len:(## 9) external_address:(bits len)\n"
      "             = MsgAddressExt;\n"
      "anycast_info$_ depth:(#<= 30) { depth >= 1 }\n"
      "   rewrite_pfx:(bits depth) = Anycast;\n"
      "addr_std$10 anycast:(Maybe Anycast)\n"
      "   workchain_id:int8 address:bits256  = MsgAddressInt;\n"
      "addr_var$11 anycast:(Maybe Anycast) addr_len:(## 9)\n"
      "   workchain_id:int32 address:(bits addr_len) = MsgAddressInt;\n"
      "_ _:MsgAddressInt = MsgAddress;\n"
      "_ _:MsgAddressExt = MsgAddress;\n"
      "//\n"
      "var_uint$_ {n:#} len:(#< n) value:(uint (len * 8))\n"
      "         = VarUInteger n;\n"
      "var_int$_ {n:#} len:(#< n) value:(int (len * 8))\n"
      "        = VarInteger n;\n"
      "nanograms$_ amount:(VarUInteger 16) = Grams;\n"
      "\n"
      "_ grams:Grams = Coins;\n"
      "\n"
      "//\n"
      "extra_currencies$_ dict:(HashmapE 32 (VarUInteger 32))\n"
      "                 = ExtraCurrencyCollection;\n"
      "currencies$_ grams:Grams other:ExtraCurrencyCollection\n"
      "           = CurrencyCollection;\n"
      "//\n"
      "int_msg_info$0 ihr_disabled:Bool bounce:Bool bounced:Bool\n"
      "  src:MsgAddressInt dest:MsgAddressInt\n"
      "  value:CurrencyCollection ihr_fee:Grams fwd_fee:Grams\n"
      "  created_lt:uint64 created_at:uint32 = CommonMsgInfo;\n"
      "ext_in_msg_info$10 src:MsgAddressExt dest:MsgAddressInt\n"
      "  import_fee:Grams = CommonMsgInfo;\n"
      "ext_out_msg_info$11 src:MsgAddressInt dest:MsgAddressExt\n"
      "  created_lt:uint64 created_at:uint32 = CommonMsgInfo;\n"
      "\n"
      "int_msg_info$0 ihr_disabled:Bool bounce:Bool bounced:Bool\n"
      "  src:MsgAddress dest:MsgAddressInt\n"
      "  value:CurrencyCollection ihr_fee:Grams fwd_fee:Grams\n"
      "  created_lt:uint64 created_at:uint32 = CommonMsgInfoRelaxed;\n"
      "ext_out_msg_info$11 src:MsgAddress dest:MsgAddressExt\n"
      "  created_lt:uint64 created_at:uint32 = CommonMsgInfoRelaxed;\n"
      "\n"
      "tick_tock$_ tick:Bool tock:Bool = TickTock;\n"
      "\n"
      "_ split_depth:(Maybe (## 5)) special:(Maybe TickTock)\n"
      "  code:(Maybe ^Cell) data:(Maybe ^Cell)\n"
      "  library:(Maybe ^Cell) = StateInit;\n"
      "\n"
      "// StateInitWithLibs is used to validate sent and received messages\n"
      "_ split_depth:(Maybe (## 5)) special:(Maybe TickTock)\n"
      "  code:(Maybe ^Cell) data:(Maybe ^Cell)\n"
      "  library:(HashmapE 256 SimpleLib) = StateInitWithLibs;\n"
      "\n"
      "simple_lib$_ public:Bool root:^Cell = SimpleLib;\n"
      "\n"
      "message$_ {X:Type} info:CommonMsgInfo\n"
      "  init:(Maybe (Either StateInit ^StateInit))\n"
      "  body:(Either X ^X) = Message X;\n"
      "\n"
      "message$_ {X:Type} info:CommonMsgInfoRelaxed\n"
      "  init:(Maybe (Either StateInit ^StateInit))\n"
      "  body:(Either X ^X) = MessageRelaxed X;\n"
      "\n"
      "_ (Message Any) = MessageAny;\n"
      "\n"
      "//\n"
      "interm_addr_regular$0 use_dest_bits:(#<= 96)\n"
      "  = IntermediateAddress;\n"
      "interm_addr_simple$10 workchain_id:int8 addr_pfx:uint64\n"
      "  = IntermediateAddress;\n"
      "interm_addr_ext$11 workchain_id:int32 addr_pfx:uint64\n"
      "  = IntermediateAddress;\n"
      "msg_envelope#4 cur_addr:IntermediateAddress\n"
      "  next_addr:IntermediateAddress fwd_fee_remaining:Grams\n"
      "  msg:^(Message Any) = MsgEnvelope;\n"
      "//\n"
      "msg_import_ext$000 msg:^(Message Any) transaction:^Transaction\n"
      "              = InMsg;\n"
      "msg_import_ihr$010 msg:^(Message Any) transaction:^Transaction\n"
      "    ihr_fee:Grams proof_created:^Cell = InMsg;\n"
      "msg_import_imm$011 in_msg:^MsgEnvelope\n"
      "    transaction:^Transaction fwd_fee:Grams = InMsg;\n"
      "msg_import_fin$100 in_msg:^MsgEnvelope\n"
      "    transaction:^Transaction fwd_fee:Grams = InMsg;\n"
      "msg_import_tr$101  in_msg:^MsgEnvelope out_msg:^MsgEnvelope\n"
      "    transit_fee:Grams = InMsg;\n"
      "msg_discard_fin$110 in_msg:^MsgEnvelope transaction_id:uint64\n"
      "    fwd_fee:Grams = InMsg;\n"
      "msg_discard_tr$111 in_msg:^MsgEnvelope transaction_id:uint64\n"
      "    fwd_fee:Grams proof_delivered:^Cell = InMsg;\n"
      "//\n"
      "import_fees$_ fees_collected:Grams\n"
      "  value_imported:CurrencyCollection = ImportFees;\n"
      "\n"
      "_ (HashmapAugE 256 InMsg ImportFees) = InMsgDescr;\n"
      "\n"
      "msg_export_ext$000 msg:^(Message Any)\n"
      "    transaction:^Transaction = OutMsg;\n"
      "msg_export_imm$010 out_msg:^MsgEnvelope\n"
      "    transaction:^Transaction reimport:^InMsg = OutMsg;\n"
      "msg_export_new$001 out_msg:^MsgEnvelope\n"
      "    transaction:^Transaction = OutMsg;\n"
      "msg_export_tr$011  out_msg:^MsgEnvelope\n"
      "    imported:^InMsg = OutMsg;\n"
      "msg_export_deq$1100 out_msg:^MsgEnvelope\n"
      "    import_block_lt:uint63 = OutMsg;\n"
      "msg_export_deq_short$1101 msg_env_hash:bits256\n"
      "    next_workchain:int32 next_addr_pfx:uint64\n"
      "    import_block_lt:uint64 = OutMsg;\n"
      "msg_export_tr_req$111 out_msg:^MsgEnvelope\n"
      "    imported:^InMsg = OutMsg;\n"
      "msg_export_deq_imm$100 out_msg:^MsgEnvelope\n"
      "    reimport:^InMsg = OutMsg;\n"
      "\n"
      "_ enqueued_lt:uint64 out_msg:^MsgEnvelope = EnqueuedMsg;\n"
      "\n"
      "_ (HashmapAugE 256 OutMsg CurrencyCollection) = OutMsgDescr;\n"
      "\n"
      "_ (HashmapAugE 352 EnqueuedMsg uint64) = OutMsgQueue;\n"
      "\n"
      "processed_upto$_ last_msg_lt:uint64 last_msg_hash:bits256 = ProcessedUpto;\n"
      "// key is [ shard:uint64 mc_seqno:uint32 ]\n"
      "_ (HashmapE 96 ProcessedUpto) = ProcessedInfo;\n"
      "\n"
      "ihr_pending$_ import_lt:uint64 = IhrPendingSince;\n"
      "_ (HashmapE 320 IhrPendingSince) = IhrPendingInfo;\n"
      "\n"
      "_ out_queue:OutMsgQueue proc_info:ProcessedInfo\n"
      "  ihr_pending:IhrPendingInfo = OutMsgQueueInfo;\n"
      "//\n"
      "storage_used$_ cells:(VarUInteger 7) bits:(VarUInteger 7)\n"
      "  public_cells:(VarUInteger 7) = StorageUsed;\n"
      "\n"
      "storage_used_short$_ cells:(VarUInteger 7)\n"
      "  bits:(VarUInteger 7) = StorageUsedShort;\n"
      "\n"
      "storage_info$_ used:StorageUsed last_paid:uint32\n"
      "              due_payment:(Maybe Grams) = StorageInfo;\n"
      "\n"
      "account_none$0 = Account;\n"
      "account$1 addr:MsgAddressInt storage_stat:StorageInfo\n"
      "          storage:AccountStorage = Account;\n"
      "\n"
      "account_storage$_ last_trans_lt:uint64\n"
      "    balance:CurrencyCollection state:AccountState\n"
      "  = AccountStorage;\n"
      "\n"
      "account_uninit$00 = AccountState;\n"
      "account_active$1 _:StateInit = AccountState;\n"
      "account_frozen$01 state_hash:bits256 = AccountState;\n"
      "\n"
      "acc_state_uninit$00 = AccountStatus;\n"
      "acc_state_frozen$01 = AccountStatus;\n"
      "acc_state_active$10 = AccountStatus;\n"
      "acc_state_nonexist$11 = AccountStatus;\n"
      "\n"
      "/* duplicates\n"
      "tick_tock$_ tick:Bool tock:Bool = TickTock;\n"
      "\n"
      "_ split_depth:(Maybe (## 5)) special:(Maybe TickTock)\n"
      "  code:(Maybe ^Cell) data:(Maybe ^Cell)\n"
      "  library:(Maybe ^Cell) = StateInit;\n"
      "*/\n"
      "\n"
      "account_descr$_ account:^Account last_trans_hash:bits256\n"
      "  last_trans_lt:uint64 = ShardAccount;\n"
      "\n"
      "depth_balance$_ split_depth:(#<= 30) balance:CurrencyCollection = DepthBalanceInfo;\n"
      "\n"
      "_ (HashmapAugE 256 ShardAccount DepthBalanceInfo) = ShardAccounts;\n"
      "\n"
      "transaction$0111 account_addr:bits256 lt:uint64\n"
      "  prev_trans_hash:bits256 prev_trans_lt:uint64 now:uint32\n"
      "  outmsg_cnt:uint15\n"
      "  orig_status:AccountStatus end_status:AccountStatus\n"
      "  ^[ in_msg:(Maybe ^(Message Any)) out_msgs:(HashmapE 15 ^(Message Any)) ]\n"
      "  total_fees:CurrencyCollection state_update:^(HASH_UPDATE Account)\n"
      "  description:^TransactionDescr = Transaction;\n"
      "\n"
      "!merkle_update#02 {X:Type} old_hash:bits256 new_hash:bits256\n"
      "  old:^X new:^X = MERKLE_UPDATE X;\n"
      "update_hashes#72 {X:Type} old_hash:bits256 new_hash:bits256\n"
      "  = HASH_UPDATE X;\n"
      "!merkle_proof#03 {X:Type} virtual_hash:bits256 depth:uint16 virtual_root:^X = MERKLE_PROOF X;\n"
      "\n"
      "acc_trans#5 account_addr:bits256\n"
      "            transactions:(HashmapAug 64 ^Transaction CurrencyCollection)\n"
      "            state_update:^(HASH_UPDATE Account)\n"
      "          = AccountBlock;\n"
      "\n"
      "_ (HashmapAugE 256 AccountBlock CurrencyCollection) = ShardAccountBlocks;\n"
      "//\n"
      "tr_phase_storage$_ storage_fees_collected:Grams\n"
      "  storage_fees_due:(Maybe Grams)\n"
      "  status_change:AccStatusChange\n"
      "  = TrStoragePhase;\n"
      "\n"
      "acst_unchanged$0 = AccStatusChange;  // x -> x\n"
      "acst_frozen$10 = AccStatusChange;    // init -> frozen\n"
      "acst_deleted$11 = AccStatusChange;   // frozen -> deleted\n"
      "\n"
      "tr_phase_credit$_ due_fees_collected:(Maybe Grams)\n"
      "  credit:CurrencyCollection = TrCreditPhase;\n"
      "\n"
      "tr_phase_compute_skipped$0 reason:ComputeSkipReason\n"
      "  = TrComputePhase;\n"
      "tr_phase_compute_vm$1 success:Bool msg_state_used:Bool\n"
      "  account_activated:Bool gas_fees:Grams\n"
      "  ^[ gas_used:(VarUInteger 7)\n"
      "  gas_limit:(VarUInteger 7) gas_credit:(Maybe (VarUInteger 3))\n"
      "  mode:int8 exit_code:int32 exit_arg:(Maybe int32)\n"
      "  vm_steps:uint32\n"
      "  vm_init_state_hash:bits256 vm_final_state_hash:bits256 ]\n"
      "  = TrComputePhase;\n"
      "cskip_no_state$00 = ComputeSkipReason;\n"
      "cskip_bad_state$01 = ComputeSkipReason;\n"
      "cskip_no_gas$10 = ComputeSkipReason;\n"
      "cskip_suspended$110 = ComputeSkipReason;\n"
      "\n"
      "tr_phase_action$_ success:Bool valid:Bool no_funds:Bool\n"
      "  status_change:AccStatusChange\n"
      "  total_fwd_fees:(Maybe Grams) total_action_fees:(Maybe Grams)\n"
      "  result_code:int32 result_arg:(Maybe int32) tot_actions:uint16\n"
      "  spec_actions:uint16 skipped_actions:uint16 msgs_created:uint16\n"
      "  action_list_hash:bits256 tot_msg_size:StorageUsedShort\n"
      "  = TrActionPhase;\n"
      "\n"
      "tr_phase_bounce_negfunds$00 = TrBouncePhase;\n"
      "tr_phase_bounce_nofunds$01 msg_size:StorageUsedShort\n"
      "  req_fwd_fees:Grams = TrBouncePhase;\n"
      "tr_phase_bounce_ok$1 msg_size:StorageUsedShort\n"
      "  msg_fees:Grams fwd_fees:Grams = TrBouncePhase;\n"
      "//\n"
      "trans_ord$0000 credit_first:Bool\n"
      "  storage_ph:(Maybe TrStoragePhase)\n"
      "  credit_ph:(Maybe TrCreditPhase)\n"
      "  compute_ph:TrComputePhase action:(Maybe ^TrActionPhase)\n"
      "  aborted:Bool bounce:(Maybe TrBouncePhase)\n"
      "  destroyed:Bool\n"
      "  = TransactionDescr;\n"
      "\n"
      "trans_storage$0001 storage_ph:TrStoragePhase\n"
      "  = TransactionDescr;\n"
      "\n"
      "trans_tick_tock$001 is_tock:Bool storage_ph:TrStoragePhase\n"
      "  compute_ph:TrComputePhase action:(Maybe ^TrActionPhase)\n"
      "  aborted:Bool destroyed:Bool = TransactionDescr;\n"
      "//\n"
      "split_merge_info$_ cur_shard_pfx_len:(## 6)\n"
      "  acc_split_depth:(## 6) this_addr:bits256 sibling_addr:bits256\n"
      "  = SplitMergeInfo;\n"
      "trans_split_prepare$0100 split_info:SplitMergeInfo\n"
      "  storage_ph:(Maybe TrStoragePhase)\n"
      "  compute_ph:TrComputePhase action:(Maybe ^TrActionPhase)\n"
      "  aborted:Bool destroyed:Bool\n"
      "  = TransactionDescr;\n"
      "trans_split_install$0101 split_info:SplitMergeInfo\n"
      "  prepare_transaction:^Transaction\n"
      "  installed:Bool = TransactionDescr;\n"
      "\n"
      "trans_merge_prepare$0110 split_info:SplitMergeInfo\n"
      "  storage_ph:TrStoragePhase aborted:Bool\n"
      "  = TransactionDescr;\n"
      "trans_merge_install$0111 split_info:SplitMergeInfo\n"
      "  prepare_transaction:^Transaction\n"
      "  storage_ph:(Maybe TrStoragePhase)\n"
      "  credit_ph:(Maybe TrCreditPhase)\n"
      "  compute_ph:TrComputePhase action:(Maybe ^TrActionPhase)\n"
      "  aborted:Bool destroyed:Bool\n"
      "  = TransactionDescr;\n"
      "\n"
      "smc_info#076ef1ea actions:uint16 msgs_sent:uint16\n"
      "  unixtime:uint32 block_lt:uint64 trans_lt:uint64\n"
      "  rand_seed:bits256 balance_remaining:CurrencyCollection\n"
      "  myself:MsgAddressInt global_config:(Maybe Cell) = SmartContractInfo;\n"
      "//\n"
      "//\n"
      "out_list_empty$_ = OutList 0;\n"
      "out_list$_ {n:#} prev:^(OutList n) action:OutAction\n"
      "  = OutList (n + 1);\n"
      "action_send_msg#0ec3c86d mode:(## 8)\n"
      "  out_msg:^(MessageRelaxed Any) = OutAction;\n"
      "action_set_code#ad4de08e new_code:^Cell = OutAction;\n"
      "action_reserve_currency#36e6b809 mode:(## 8)\n"
      "  currency:CurrencyCollection = OutAction;\n"
      "libref_hash$0 lib_hash:bits256 = LibRef;\n"
      "libref_ref$1 library:^Cell = LibRef;\n"
      "action_change_library#26fa1dd4 mode:(## 7)\n"
      "  libref:LibRef = OutAction;\n"
      "\n"
      "out_list_node$_ prev:^Cell action:OutAction = OutListNode;\n"
      "//\n"
      "//\n"
      "shard_ident$00 shard_pfx_bits:(#<= 60)\n"
      "  workchain_id:int32 shard_prefix:uint64 = ShardIdent;\n"
      "\n"
      "ext_blk_ref$_ end_lt:uint64\n"
      "  seq_no:uint32 root_hash:bits256 file_hash:bits256\n"
      "  = ExtBlkRef;\n"
      "\n"
      "block_id_ext$_ shard_id:ShardIdent seq_no:uint32\n"
      "  root_hash:bits256 file_hash:bits256 = BlockIdExt;\n"
      "\n"
      "master_info$_ master:ExtBlkRef = BlkMasterInfo;\n"
      "\n"
      "shard_state#9023afe2 global_id:int32\n"
      "  shard_id:ShardIdent\n"
      "  seq_no:uint32 vert_seq_no:#\n"
      "  gen_utime:uint32 gen_lt:uint64\n"
      "  min_ref_mc_seqno:uint32\n"
      "  out_msg_queue_info:^OutMsgQueueInfo\n"
      "  before_split:(## 1)\n"
      "  accounts:^ShardAccounts\n"
      "  ^[ overload_history:uint64 underload_history:uint64\n"
      "  total_balance:CurrencyCollection\n"
      "  total_validator_fees:CurrencyCollection\n"
      "  libraries:(HashmapE 256 LibDescr)\n"
      "  master_ref:(Maybe BlkMasterInfo) ]\n"
      "  custom:(Maybe ^McStateExtra)\n"
      "  = ShardStateUnsplit;\n"
      "\n"
      "_ ShardStateUnsplit = ShardState;\n"
      "split_state#5f327da5 left:^ShardStateUnsplit right:^ShardStateUnsplit = ShardState;\n"
      "\n"
      "shared_lib_descr$00 lib:^Cell publishers:(Hashmap 256 Truet)\n"
      "  = LibDescr;\n"
      "\n"
      "block_info#9bc7a987 version:uint32\n"
      "  not_master:(## 1)\n"
      "  after_merge:(## 1) before_split:(## 1)\n"
      "  after_split:(## 1)\n"
      "  want_split:Bool want_merge:Bool\n"
      "  key_block:Bool vert_seqno_incr:(## 1)\n"
      "  flags:(## 8) { flags <= 1 }\n"
      "  seq_no:# vert_seq_no:# { vert_seq_no >= vert_seqno_incr }\n"
      "  { prev_seq_no:# } { ~prev_seq_no + 1 = seq_no }\n"
      "  shard:ShardIdent gen_utime:uint32\n"
      "  start_lt:uint64 end_lt:uint64\n"
      "  gen_validator_list_hash_short:uint32\n"
      "  gen_catchain_seqno:uint32\n"
      "  min_ref_mc_seqno:uint32\n"
      "  prev_key_block_seqno:uint32\n"
      "  gen_software:flags . 0?GlobalVersion\n"
      "  master_ref:not_master?^BlkMasterInfo\n"
      "  prev_ref:^(BlkPrevInfo after_merge)\n"
      "  prev_vert_ref:vert_seqno_incr?^(BlkPrevInfo 0)\n"
      "  = BlockInfo;\n"
      "\n"
      "prev_blk_info$_ prev:ExtBlkRef = BlkPrevInfo 0;\n"
      "prev_blks_info$_ prev1:^ExtBlkRef prev2:^ExtBlkRef = BlkPrevInfo 1;\n"
      "\n"
      "block#11ef55aa global_id:int32\n"
      "  info:^BlockInfo value_flow:^ValueFlow\n"
      "  state_update:^(MERKLE_UPDATE ShardState)\n"
      "  extra:^BlockExtra = Block;\n"
      "\n"
      "block_extra in_msg_descr:^InMsgDescr\n"
      "  out_msg_descr:^OutMsgDescr\n"
      "  account_blocks:^ShardAccountBlocks\n"
      "  rand_seed:bits256\n"
      "  created_by:bits256\n"
      "  custom:(Maybe ^McBlockExtra) = BlockExtra;\n"
      "//\n"
      "value_flow#b8e48dfb ^[ from_prev_blk:CurrencyCollection\n"
      "  to_next_blk:CurrencyCollection\n"
      "  imported:CurrencyCollection\n"
      "  exported:CurrencyCollection ]\n"
      "  fees_collected:CurrencyCollection\n"
      "  ^[\n"
      "  fees_imported:CurrencyCollection\n"
      "  recovered:CurrencyCollection\n"
      "  created:CurrencyCollection\n"
      "  minted:CurrencyCollection\n"
      "  ] = ValueFlow;\n"
      "\n"
      "value_flow_v2#3ebf98b7 ^[ from_prev_blk:CurrencyCollection\n"
      "  to_next_blk:CurrencyCollection\n"
      "  imported:CurrencyCollection\n"
      "  exported:CurrencyCollection ]\n"
      "  fees_collected:CurrencyCollection\n"
      "  burned:CurrencyCollection\n"
      "  ^[\n"
      "  fees_imported:CurrencyCollection\n"
      "  recovered:CurrencyCollection\n"
      "  created:CurrencyCollection\n"
      "  minted:CurrencyCollection\n"
      "  ] = ValueFlow;\n"
      "\n"
      "//\n"
      "//\n"
      "bt_leaf$0 {X:Type} leaf:X = BinTree X;\n"
      "bt_fork$1 {X:Type} left:^(BinTree X) right:^(BinTree X)\n"
      "          = BinTree X;\n"
      "\n"
      "fsm_none$0 = FutureSplitMerge;\n"
      "fsm_split$10 split_utime:uint32 interval:uint32 = FutureSplitMerge;\n"
      "fsm_merge$11 merge_utime:uint32 interval:uint32 = FutureSplitMerge;\n"
      "\n"
      "shard_descr#b seq_no:uint32 reg_mc_seqno:uint32\n"
      "  start_lt:uint64 end_lt:uint64\n"
      "  root_hash:bits256 file_hash:bits256\n"
      "  before_split:Bool before_merge:Bool\n"
      "  want_split:Bool want_merge:Bool\n"
      "  nx_cc_updated:Bool flags:(## 3) { flags = 0 }\n"
      "  next_catchain_seqno:uint32 next_validator_shard:uint64\n"
      "  min_ref_mc_seqno:uint32 gen_utime:uint32\n"
      "  split_merge_at:FutureSplitMerge\n"
      "  fees_collected:CurrencyCollection\n"
      "  funds_created:CurrencyCollection = ShardDescr;\n"
      "\n"
      "shard_descr_new#a seq_no:uint32 reg_mc_seqno:uint32\n"
      "  start_lt:uint64 end_lt:uint64\n"
      "  root_hash:bits256 file_hash:bits256\n"
      "  before_split:Bool before_merge:Bool\n"
      "  want_split:Bool want_merge:Bool\n"
      "  nx_cc_updated:Bool flags:(## 3) { flags = 0 }\n"
      "  next_catchain_seqno:uint32 next_validator_shard:uint64\n"
      "  min_ref_mc_seqno:uint32 gen_utime:uint32\n"
      "  split_merge_at:FutureSplitMerge\n"
      "  ^[ fees_collected:CurrencyCollection\n"
      "     funds_created:CurrencyCollection ] = ShardDescr;\n"
      "\n"
      "_ (HashmapE 32 ^(BinTree ShardDescr)) = ShardHashes;\n"
      "\n"
      "bta_leaf$0 {X:Type} {Y:Type} extra:Y leaf:X = BinTreeAug X Y;\n"
      "bta_fork$1 {X:Type} {Y:Type} left:^(BinTreeAug X Y)\n"
      "           right:^(BinTreeAug X Y) extra:Y = BinTreeAug X Y;\n"
      "\n"
      "_ fees:CurrencyCollection create:CurrencyCollection = ShardFeeCreated;\n"
      "_ (HashmapAugE 96 ShardFeeCreated ShardFeeCreated) = ShardFees;\n"
      "\n"
      "_ config_addr:bits256 config:^(Hashmap 32 ^Cell)\n"
      "  = ConfigParams;\n"
      "\n"
      "validator_info$_\n"
      "  validator_list_hash_short:uint32\n"
      "  catchain_seqno:uint32\n"
      "  nx_cc_updated:Bool\n"
      "= ValidatorInfo;\n"
      "\n"
      "validator_base_info$_\n"
      "  validator_list_hash_short:uint32\n"
      "  catchain_seqno:uint32\n"
      "= ValidatorBaseInfo;\n"
      "\n"
      "_ key:Bool max_end_lt:uint64 = KeyMaxLt;\n"
      "_ key:Bool blk_ref:ExtBlkRef = KeyExtBlkRef;\n"
      "\n"
      "_ (HashmapAugE 32 KeyExtBlkRef KeyMaxLt) = OldMcBlocksInfo;\n"
      "\n"
      "\n"
      "counters#_ last_updated:uint32 total:uint64 cnt2048:uint64 cnt65536:uint64 = Counters;\n"
      "creator_info#4 mc_blocks:Counters shard_blocks:Counters = CreatorStats;\n"
      "block_create_stats#17 counters:(HashmapE 256 CreatorStats) = BlockCreateStats;\n"
      "block_create_stats_ext#34 counters:(HashmapAugE 256 CreatorStats uint32) = BlockCreateStats;\n"
      "\n"
      "masterchain_state_extra#cc26\n"
      "  shard_hashes:ShardHashes\n"
      "  config:ConfigParams\n"
      "  ^[ flags:(## 16) { flags <= 1 }\n"
      "     validator_info:ValidatorInfo\n"
      "     prev_blocks:OldMcBlocksInfo\n"
      "     after_key_block:Bool\n"
      "     last_key_block:(Maybe ExtBlkRef)\n"
      "     block_create_stats:(flags . 0)?BlockCreateStats ]\n"
      "  global_balance:CurrencyCollection\n"
      "= McStateExtra;\n"
      "\n"
      "ed25519_pubkey#8e81278a pubkey:bits256 = SigPubKey;  // 288 bits\n"
      "ed25519_signature#5 R:bits256 s:bits256 = CryptoSignatureSimple;  // 516 bits\n"
      "_ CryptoSignatureSimple = CryptoSignature;\n"
      "sig_pair$_ node_id_short:bits256 sign:CryptoSignature = CryptoSignaturePair;  // 256+x ~ 772 bits\n"
      "\n"
      "certificate#4 temp_key:SigPubKey valid_since:uint32 valid_until:uint32 = Certificate;  // 356 bits\n"
      "certificate_env#a419b7d certificate:Certificate = CertificateEnv;  // 384 bits\n"
      "signed_certificate$_ certificate:Certificate certificate_signature:CryptoSignature\n"
      "  = SignedCertificate;  // 356+516 = 872 bits\n"
      "// certificate_signature is the signature of CertificateEnv (with embedded certificate) with persistent key\n"
      "chained_signature#f signed_cert:^SignedCertificate temp_key_signature:CryptoSignatureSimple\n"
      "  = CryptoSignature;   // 4+(356+516)+516 = 520 bits+ref (1392 bits total)\n"
      "// temp_key_signature is the signature of whatever was originally intended to be signed with temp_key from certificate\n"
      "\n"
      "masterchain_block_extra#cca5\n"
      "  key_block:(## 1)\n"
      "  shard_hashes:ShardHashes\n"
      "  shard_fees:ShardFees\n"
      "  ^[ prev_blk_signatures:(HashmapE 16 CryptoSignaturePair)\n"
      "     recover_create_msg:(Maybe ^InMsg)\n"
      "     mint_msg:(Maybe ^InMsg) ]\n"
      "  config:key_block?ConfigParams\n"
      "= McBlockExtra;\n"
      "\n"
      "//\n"
      "//  CONFIGURATION PARAMETERS\n"
      "//\n"
      "\n"
      "validator#53 public_key:SigPubKey weight:uint64 = ValidatorDescr;\n"
      "validator_addr#73 public_key:SigPubKey weight:uint64 adnl_addr:bits256 = ValidatorDescr;\n"
      "validators#11 utime_since:uint32 utime_until:uint32\n"
      "  total:(## 16) main:(## 16) { main <= total } { main >= 1 }\n"
      "  list:(Hashmap 16 ValidatorDescr) = ValidatorSet;\n"
      "validators_ext#12 utime_since:uint32 utime_until:uint32\n"
      "  total:(## 16) main:(## 16) { main <= total } { main >= 1 }\n"
      "  total_weight:uint64 list:(HashmapE 16 ValidatorDescr) = ValidatorSet;\n"
      "\n"
      "_ config_addr:bits256 = ConfigParam 0;\n"
      "_ elector_addr:bits256 = ConfigParam 1;\n"
      "_ minter_addr:bits256 = ConfigParam 2;  // ConfigParam 0 is used if absent\n"
      "_ fee_collector_addr:bits256 = ConfigParam 3;  // ConfigParam 1 is used if absent\n"
      "_ dns_root_addr:bits256 = ConfigParam 4;  // root TON DNS resolver\n"
      "\n"
      "burning_config#01\n"
      "  blackhole_addr:(Maybe bits256)\n"
      "  fee_burn_num:# fee_burn_denom:# { fee_burn_num <= fee_burn_denom } { fee_burn_denom >= 1 } = BurningConfig;\n"
      "_ BurningConfig = ConfigParam 5;\n"
      "\n"
      "_ mint_new_price:Grams mint_add_price:Grams = ConfigParam 6;\n"
      "_ to_mint:ExtraCurrencyCollection = ConfigParam 7;\n"
      "\n"
      "capabilities#c4 version:uint32 capabilities:uint64 = GlobalVersion;\n"
      "_ GlobalVersion = ConfigParam 8;  // all zero if absent\n"
      "_ mandatory_params:(Hashmap 32 Truet) = ConfigParam 9;\n"
      "_ critical_params:(Hashmap 32 Truet) = ConfigParam 10;\n"
      "\n"
      "cfg_vote_cfg#36 min_tot_rounds:uint8 max_tot_rounds:uint8 min_wins:uint8 max_losses:uint8 min_store_sec:uint32 max_store_sec:uint32 bit_price:uint32 cell_price:uint32 = ConfigProposalSetup;\n"
      "cfg_vote_setup#91 normal_params:^ConfigProposalSetup critical_params:^ConfigProposalSetup = ConfigVotingSetup;\n"
      "_ ConfigVotingSetup = ConfigParam 11;\n"
      "\n"
      "cfg_proposal#f3 param_id:int32 param_value:(Maybe ^Cell) if_hash_equal:(Maybe uint256)\n"
      "  = ConfigProposal;\n"
      "cfg_proposal_status#ce expires:uint32 proposal:^ConfigProposal is_critical:Bool\n"
      "  voters:(HashmapE 16 Truet) remaining_weight:int64 validator_set_id:uint256\n"
      "  rounds_remaining:uint8 wins:uint8 losses:uint8 = ConfigProposalStatus;\n"
      "\n"
      "wfmt_basic#1 vm_version:int32 vm_mode:uint64 = WorkchainFormat 1;\n"
      "wfmt_ext#0 min_addr_len:(## 12) max_addr_len:(## 12) addr_len_step:(## 12)\n"
      "  { min_addr_len >= 64 } { min_addr_len <= max_addr_len }\n"
      "  { max_addr_len <= 1023 } { addr_len_step <= 1023 }\n"
      "  workchain_type_id:(## 32) { workchain_type_id >= 1 }\n"
      "  = WorkchainFormat 0;\n"
      "\n"
      "wc_split_merge_timings#0\n"
      "  split_merge_delay:uint32 split_merge_interval:uint32\n"
      "  min_split_merge_interval:uint32 max_split_merge_delay:uint32\n"
      "  = WcSplitMergeTimings;\n"
      "\n"
      "//workchain#a5 enabled_since:uint32 min_split:(## 8) max_split:(## 8)\n"
      "//  { min_split <= max_split } { max_split <= 60 }\n"
      "\n"
      "workchain#a6 enabled_since:uint32 actual_min_split:(## 8)\n"
      "  min_split:(## 8) max_split:(## 8) { actual_min_split <= min_split }\n"
      "  basic:(## 1) active:Bool accept_msgs:Bool flags:(## 13) { flags = 0 }\n"
      "  zerostate_root_hash:bits256 zerostate_file_hash:bits256\n"
      "  version:uint32 format:(WorkchainFormat basic)\n"
      "  = WorkchainDescr;\n"
      "\n"
      "workchain_v2#a7 enabled_since:uint32 actual_min_split:(## 8)\n"
      "  min_split:(## 8) max_split:(## 8) { actual_min_split <= min_split }\n"
      "  basic:(## 1) active:Bool accept_msgs:Bool flags:(## 13) { flags = 0 }\n"
      "  zerostate_root_hash:bits256 zerostate_file_hash:bits256\n"
      "  version:uint32 format:(WorkchainFormat basic)\n"
      "  split_merge_timings:WcSplitMergeTimings\n"
      "  = WorkchainDescr;\n"
      "\n"
      "_ workchains:(HashmapE 32 WorkchainDescr) = ConfigParam 12;\n"
      "\n"
      "complaint_prices#1a deposit:Grams bit_price:Grams cell_price:Grams = ComplaintPricing;\n"
      "_ ComplaintPricing = ConfigParam 13;\n"
      "\n"
      "block_grams_created#6b masterchain_block_fee:Grams basechain_block_fee:Grams\n"
      "  = BlockCreateFees;\n"
      "_ BlockCreateFees = ConfigParam 14;\n"
      "\n"
      "_ validators_elected_for:uint32 elections_start_before:uint32\n"
      "  elections_end_before:uint32 stake_held_for:uint32\n"
      "  = ConfigParam 15;\n"
      "\n"
      "_ max_validators:(## 16) max_main_validators:(## 16) min_validators:(## 16)\n"
      "  { max_validators >= max_main_validators }\n"
      "  { max_main_validators >= min_validators }\n"
      "  { min_validators >= 1 }\n"
      "  = ConfigParam 16;\n"
      "\n"
      "_ min_stake:Grams max_stake:Grams min_total_stake:Grams max_stake_factor:uint32 = ConfigParam 17;\n"
      "\n"
      "_#cc utime_since:uint32 bit_price_ps:uint64 cell_price_ps:uint64\n"
      "  mc_bit_price_ps:uint64 mc_cell_price_ps:uint64 = StoragePrices;\n"
      "_ (Hashmap 32 StoragePrices) = ConfigParam 18;\n"
      "\n"
      "_ global_id:int32 = ConfigParam 19;\n"
      "\n"
      "gas_prices#dd gas_price:uint64 gas_limit:uint64 gas_credit:uint64\n"
      "  block_gas_limit:uint64 freeze_due_limit:uint64 delete_due_limit:uint64\n"
      "  = GasLimitsPrices;\n"
      "\n"
      "gas_prices_ext#de gas_price:uint64 gas_limit:uint64 special_gas_limit:uint64 gas_credit:uint64\n"
      "  block_gas_limit:uint64 freeze_due_limit:uint64 delete_due_limit:uint64\n"
      "  = GasLimitsPrices;\n"
      "\n"
      "gas_flat_pfx#d1 flat_gas_limit:uint64 flat_gas_price:uint64 other:GasLimitsPrices\n"
      "  = GasLimitsPrices;\n"
      "\n"
      "config_mc_gas_prices#_ GasLimitsPrices = ConfigParam 20;\n"
      "config_gas_prices#_ GasLimitsPrices = ConfigParam 21;\n"
      "\n"
      "param_limits#c3 underload:# soft_limit:# { underload <= soft_limit }\n"
      "  hard_limit:# { soft_limit <= hard_limit } = ParamLimits;\n"
      "block_limits#5d bytes:ParamLimits gas:ParamLimits lt_delta:ParamLimits\n"
      "  = BlockLimits;\n"
      "\n"
      "config_mc_block_limits#_ BlockLimits = ConfigParam 22;\n"
      "config_block_limits#_ BlockLimits = ConfigParam 23;\n"
      "\n"
      "// msg_fwd_fees = (lump_price + ceil((bit_price * msg.bits + cell_price * msg.cells)/2^16)) nanograms\n"
      "// ihr_fwd_fees = ceil((msg_fwd_fees * ihr_price_factor)/2^16) nanograms\n"
      "// bits in the root cell of a message are not included in msg.bits (lump_price pays for them)\n"
      "msg_forward_prices#ea lump_price:uint64 bit_price:uint64 cell_price:uint64\n"
      "  ihr_price_factor:uint32 first_frac:uint16 next_frac:uint16 = MsgForwardPrices;\n"
      "\n"
      "// used for messages to/from masterchain\n"
      "config_mc_fwd_prices#_ MsgForwardPrices = ConfigParam 24;\n"
      "// used for all other messages\n"
      "config_fwd_prices#_ MsgForwardPrices = ConfigParam 25;\n"
      "\n"
      "catchain_config#c1 mc_catchain_lifetime:uint32 shard_catchain_lifetime:uint32\n"
      "  shard_validators_lifetime:uint32 shard_validators_num:uint32 = CatchainConfig;\n"
      "\n"
      "catchain_config_new#c2 flags:(## 7) { flags = 0 } shuffle_mc_validators:Bool\n"
      "  mc_catchain_lifetime:uint32 shard_catchain_lifetime:uint32\n"
      "  shard_validators_lifetime:uint32 shard_validators_num:uint32 = CatchainConfig;\n"
      "\n"
      "consensus_config#d6 round_candidates:# { round_candidates >= 1 }\n"
      "  next_candidate_delay_ms:uint32 consensus_timeout_ms:uint32\n"
      "  fast_attempts:uint32 attempt_duration:uint32 catchain_max_deps:uint32\n"
      "  max_block_bytes:uint32 max_collated_bytes:uint32 = ConsensusConfig;\n"
      "\n"
      "consensus_config_new#d7 flags:(## 7) { flags = 0 } new_catchain_ids:Bool\n"
      "  round_candidates:(## 8) { round_candidates >= 1 }\n"
      "  next_candidate_delay_ms:uint32 consensus_timeout_ms:uint32\n"
      "  fast_attempts:uint32 attempt_duration:uint32 catchain_max_deps:uint32\n"
      "  max_block_bytes:uint32 max_collated_bytes:uint32 = ConsensusConfig;\n"
      "\n"
      "consensus_config_v3#d8 flags:(## 7) { flags = 0 } new_catchain_ids:Bool\n"
      "  round_candidates:(## 8) { round_candidates >= 1 }\n"
      "  next_candidate_delay_ms:uint32 consensus_timeout_ms:uint32\n"
      "  fast_attempts:uint32 attempt_duration:uint32 catchain_max_deps:uint32\n"
      "  max_block_bytes:uint32 max_collated_bytes:uint32\n"
      "  proto_version:uint16 = ConsensusConfig;\n"
      "\n"
      "consensus_config_v4#d9 flags:(## 7) { flags = 0 } new_catchain_ids:Bool\n"
      "  round_candidates:(## 8) { round_candidates >= 1 }\n"
      "  next_candidate_delay_ms:uint32 consensus_timeout_ms:uint32\n"
      "  fast_attempts:uint32 attempt_duration:uint32 catchain_max_deps:uint32\n"
      "  max_block_bytes:uint32 max_collated_bytes:uint32\n"
      "  proto_version:uint16 catchain_max_blocks_coeff:uint32 = ConsensusConfig;\n"
      "\n"
      "_ CatchainConfig = ConfigParam 28;\n"
      "_ ConsensusConfig = ConfigParam 29;\n"
      "\n"
      "_ fundamental_smc_addr:(HashmapE 256 Truet) = ConfigParam 31;\n"
      "_ prev_validators:ValidatorSet = ConfigParam 32;\n"
      "_ prev_temp_validators:ValidatorSet = ConfigParam 33;\n"
      "_ cur_validators:ValidatorSet = ConfigParam 34;\n"
      "_ cur_temp_validators:ValidatorSet = ConfigParam 35;\n"
      "_ next_validators:ValidatorSet = ConfigParam 36;\n"
      "_ next_temp_validators:ValidatorSet = ConfigParam 37;\n"
      "\n"
      "validator_temp_key#3 adnl_addr:bits256 temp_public_key:SigPubKey seqno:# valid_until:uint32 = ValidatorTempKey;\n"
      "signed_temp_key#4 key:^ValidatorTempKey signature:CryptoSignature = ValidatorSignedTempKey;\n"
      "_ (HashmapE 256 ValidatorSignedTempKey) = ConfigParam 39;\n"
      "\n"
      "misbehaviour_punishment_config_v1#01\n"
      "  default_flat_fine:Grams default_proportional_fine:uint32\n"
      "  severity_flat_mult:uint16 severity_proportional_mult:uint16\n"
      "  unpunishable_interval:uint16\n"
      "  long_interval:uint16 long_flat_mult:uint16 long_proportional_mult:uint16\n"
      "  medium_interval:uint16 medium_flat_mult:uint16 medium_proportional_mult:uint16\n"
      "   = MisbehaviourPunishmentConfig;\n"
      "_ MisbehaviourPunishmentConfig = ConfigParam 40;\n"
      "\n"
      "size_limits_config#01 max_msg_bits:uint32 max_msg_cells:uint32 max_library_cells:uint32 max_vm_data_depth:uint16\n"
      "  max_ext_msg_size:uint32 max_ext_msg_depth:uint16 = SizeLimitsConfig;\n"
      "size_limits_config_v2#02 max_msg_bits:uint32 max_msg_cells:uint32 max_library_cells:uint32 max_vm_data_depth:uint16\n"
      "  max_ext_msg_size:uint32 max_ext_msg_depth:uint16 max_acc_state_cells:uint32 max_acc_state_bits:uint32 = SizeLimitsConfig;\n"
      "_ SizeLimitsConfig = ConfigParam 43;\n"
      "\n"
      "// key is [ wc:int32 addr:uint256 ]\n"
      "suspended_address_list#00 addresses:(HashmapE 288 Unit) suspended_until:uint32 = SuspendedAddressList;\n"
      "_ SuspendedAddressList = ConfigParam 44;\n"
      "\n"
      "oracle_bridge_params#_ bridge_address:bits256 oracle_mutlisig_address:bits256 oracles:(HashmapE 256 uint256) external_chain_address:bits256 = OracleBridgeParams;\n"
      "_ OracleBridgeParams = ConfigParam 71; // Ethereum bridge\n"
      "_ OracleBridgeParams = ConfigParam 72; // Binance Smart Chain bridge\n"
      "_ OracleBridgeParams = ConfigParam 73; // Polygon bridge\n"
      "\n"
      "// Note that chains in which bridge, minter and jetton-wallet operate are fixated\n"
      "jetton_bridge_prices#_ bridge_burn_fee:Coins bridge_mint_fee:Coins\n"
      "                       wallet_min_tons_for_storage:Coins\n"
      "                       wallet_gas_consumption:Coins\n"
      "                       minter_min_tons_for_storage:Coins\n"
      "                       discover_gas_consumption:Coins = JettonBridgePrices;\n"
      "\n"
      "jetton_bridge_params_v0#00 bridge_address:bits256 oracles_address:bits256 oracles:(HashmapE 256 uint256) state_flags:uint8 burn_bridge_fee:Coins = JettonBridgeParams;\n"
      "jetton_bridge_params_v1#01 bridge_address:bits256 oracles_address:bits256 oracles:(HashmapE 256 uint256) state_flags:uint8 prices:^JettonBridgePrices external_chain_address:bits256 = JettonBridgeParams;\n"
      "\n"
      "_ JettonBridgeParams = ConfigParam 79; // ETH->TON token bridge\n"
      "_ JettonBridgeParams = ConfigParam 81; // BNB->TON token bridge\n"
      "_ JettonBridgeParams = ConfigParam 82; // Polygon->TON token bridge\n"
      "\n"
      "\n"
      "//\n"
      "//  PROOFS\n"
      "//\n"
      "block_signatures_pure#_ sig_count:uint32 sig_weight:uint64\n"
      "  signatures:(HashmapE 16 CryptoSignaturePair) = BlockSignaturesPure;\n"
      "block_signatures#11 validator_info:ValidatorBaseInfo pure_signatures:BlockSignaturesPure = BlockSignatures;\n"
      "block_proof#c3 proof_for:BlockIdExt root:^Cell signatures:(Maybe ^BlockSignatures) = BlockProof;\n"
      "\n"
      "chain_empty$_ = ProofChain 0;\n"
      "chain_link$_ {n:#} root:^Cell prev:n?^(ProofChain n) = ProofChain (n + 1);\n"
      "top_block_descr#d5 proof_for:BlockIdExt signatures:(Maybe ^BlockSignatures)\n"
      "  len:(## 8) { len >= 1 } { len <= 8 } chain:(ProofChain len) = TopBlockDescr;\n"
      "\n"
      "//\n"
      "//  COLLATED DATA\n"
      "//\n"
      "top_block_descr_set#4ac789f3 collection:(HashmapE 96 ^TopBlockDescr) = TopBlockDescrSet;\n"
      "\n"
      "//\n"
      "//  VALIDATOR MISBEHAVIOR COMPLAINTS\n"
      "//\n"
      "prod_info#34 utime:uint32 mc_blk_ref:ExtBlkRef state_proof:^(MERKLE_PROOF Block)\n"
      "  prod_proof:^(MERKLE_PROOF ShardState) = ProducerInfo;\n"
      "no_blk_gen from_utime:uint32 prod_info:^ProducerInfo = ComplaintDescr;\n"
      "no_blk_gen_diff prod_info_old:^ProducerInfo prod_info_new:^ProducerInfo = ComplaintDescr;\n"
      "validator_complaint#bc validator_pubkey:bits256 description:^ComplaintDescr created_at:uint32 severity:uint8 reward_addr:uint256 paid:Grams suggested_fine:Grams suggested_fine_part:uint32 = ValidatorComplaint;\n"
      "complaint_status#2d complaint:^ValidatorComplaint voters:(HashmapE 16 Truet) vset_id:uint256 weight_remaining:int64 = ValidatorComplaintStatus;\n"
      "\n"
      "//\n"
      "//  TVM REFLECTION\n"
      "//\n"
      "vm_stk_null#00 = VmStackValue;\n"
      "vm_stk_tinyint#01 value:int64 = VmStackValue;\n"
      "vm_stk_int#0201_ value:int257 = VmStackValue;\n"
      "vm_stk_nan#02ff = VmStackValue;\n"
      "vm_stk_cell#03 cell:^Cell = VmStackValue;\n"
      "_ cell:^Cell st_bits:(## 10) end_bits:(## 10) { st_bits <= end_bits }\n"
      "  st_ref:(#<= 4) end_ref:(#<= 4) { st_ref <= end_ref } = VmCellSlice;\n"
      "vm_stk_slice#04 _:VmCellSlice = VmStackValue;\n"
      "vm_stk_builder#05 cell:^Cell = VmStackValue;\n"
      "vm_stk_cont#06 cont:VmCont = VmStackValue;\n"
      "vm_tupref_nil$_ = VmTupleRef 0;\n"
      "vm_tupref_single$_ entry:^VmStackValue = VmTupleRef 1;\n"
      "vm_tupref_any$_ {n:#} ref:^(VmTuple (n + 2)) = VmTupleRef (n + 2);\n"
      "vm_tuple_nil$_ = VmTuple 0;\n"
      "vm_tuple_tcons$_ {n:#} head:(VmTupleRef n) tail:^VmStackValue = VmTuple (n + 1);\n"
      "vm_stk_tuple#07 len:(## 16) data:(VmTuple len) = VmStackValue;\n"
      "\n"
      "vm_stack#_ depth:(## 24) stack:(VmStackList depth) = VmStack;\n"
      "vm_stk_cons#_ {n:#} rest:^(VmStackList n) tos:VmStackValue = VmStackList (n + 1);\n"
      "vm_stk_nil#_ = VmStackList 0;\n"
      "\n"
      "_ cregs:(HashmapE 4 VmStackValue) = VmSaveList;\n"
      "gas_limits#_ remaining:int64 _:^[ max_limit:int64 cur_limit:int64 credit:int64 ]\n"
      "  = VmGasLimits;\n"
      "_ libraries:(HashmapE 256 ^Cell) = VmLibraries;\n"
      "\n"
      "vm_ctl_data$_ nargs:(Maybe uint13) stack:(Maybe VmStack) save:VmSaveList\n"
      "cp:(Maybe int16) = VmControlData;\n"
      "vmc_std$00 cdata:VmControlData code:VmCellSlice = VmCont;\n"
      "vmc_envelope$01 cdata:VmControlData next:^VmCont = VmCont;\n"
      "vmc_quit$1000 exit_code:int32 = VmCont;\n"
      "vmc_quit_exc$1001 = VmCont;\n"
      "vmc_repeat$10100 count:uint63 body:^VmCont after:^VmCont = VmCont;\n"
      "vmc_until$110000 body:^VmCont after:^VmCont = VmCont;\n"
      "vmc_again$110001 body:^VmCont = VmCont;\n"
      "vmc_while_cond$110010 cond:^VmCont body:^VmCont\n"
      "after:^VmCont = VmCont;\n"
      "vmc_while_body$110011 cond:^VmCont body:^VmCont\n"
      "after:^VmCont = VmCont;\n"
      "vmc_pushint$1111 value:int32 next:^VmCont = VmCont;\n"
      "\n"
      "//\n"
      "//  DNS RECORDS\n"
      "//\n"
      "_ (HashmapE 256 ^DNSRecord) = DNS_RecordSet;\n"
      "\n"
      "chunk_ref$_ {n:#} ref:^(TextChunks (n + 1)) = TextChunkRef (n + 1);\n"
      "chunk_ref_empty$_ = TextChunkRef 0;\n"
      "text_chunk$_ {n:#} len:(## 8) data:(bits (len * 8)) next:(TextChunkRef n) = TextChunks (n + 1);\n"
      "text_chunk_empty$_ = TextChunks 0;\n"
      "text$_ chunks:(## 8) rest:(TextChunks chunks) = Text;\n"
      "dns_text#1eda _:Text = DNSRecord;\n"
      "\n"
      "dns_next_resolver#ba93 resolver:MsgAddressInt = DNSRecord;  // usually in record #-1\n"
      "\n"
      "dns_adnl_address#ad01 adnl_addr:bits256 flags:(## 8) { flags <= 1 }\n"
      "  proto_list:flags . 0?ProtoList = DNSRecord;  // often in record #2\n"
      "proto_list_nil$0 = ProtoList;\n"
      "proto_list_next$1 head:Protocol tail:ProtoList = ProtoList;\n"
      "proto_http#4854 = Protocol;\n"
      "\n"
      "dns_smc_address#9fd3 smc_addr:MsgAddressInt flags:(## 8) { flags <= 1 }\n"
      "  cap_list:flags . 0?SmcCapList = DNSRecord;   // often in record #1\n"
      "cap_list_nil$0 = SmcCapList;\n"
      "cap_list_next$1 head:SmcCapability tail:SmcCapList = SmcCapList;\n"
      "cap_method_seqno#5371 = SmcCapability;\n"
      "cap_method_pubkey#71f4 = SmcCapability;\n"
      "cap_is_wallet#2177 = SmcCapability;\n"
      "cap_name#ff name:Text = SmcCapability;\n"
      "\n"
      "dns_storage_address#7473 bag_id:bits256 = DNSRecord;\n"
      "\n"
      "//\n"
      "// PAYMENT CHANNELS\n"
      "//\n"
      "\n"
      "chan_config$_  init_timeout:uint32 close_timeout:uint32 a_key:bits256 b_key:bits256\n"
      "  a_addr:^MsgAddressInt b_addr:^MsgAddressInt channel_id:uint64 min_A_extra:Grams = ChanConfig;\n"
      "\n"
      "chan_state_init$000  signed_A:Bool signed_B:Bool min_A:Grams min_B:Grams expire_at:uint32 A:Grams B:Grams = ChanState;\n"
      "chan_state_close$001 signed_A:Bool signed_B:Bool promise_A:Grams promise_B:Grams expire_at:uint32 A:Grams B:Grams = ChanState;\n"
      "chan_state_payout$010 A:Grams B:Grams = ChanState;\n"
      "\n"
      "chan_promise$_ channel_id:uint64 promise_A:Grams promise_B:Grams = ChanPromise;\n"
      "chan_signed_promise#_ sig:(Maybe ^bits512) promise:ChanPromise = ChanSignedPromise;\n"
      "\n"
      "chan_msg_init#27317822 inc_A:Grams inc_B:Grams min_A:Grams min_B:Grams channel_id:uint64 = ChanMsg;\n"
      "chan_msg_close#f28ae183 extra_A:Grams extra_B:Grams promise:ChanSignedPromise  = ChanMsg;\n"
      "chan_msg_timeout#43278a28 = ChanMsg;\n"
      "chan_msg_payout#37fe7810 = ChanMsg;\n"
      "\n"
      "chan_signed_msg$_ sig_A:(Maybe ^bits512) sig_B:(Maybe ^bits512) msg:ChanMsg = ChanSignedMsg;\n"
      "\n"
      "chan_op_cmd#912838d1 msg:ChanSignedMsg = ChanOp;\n"
      "\n"
      "\n"
      "chan_data$_ config:^ChanConfig state:^ChanState = ChanData;");

  //  int i;
  //  bool interactive = false;
  //  bool no_code_gen = false;
  //  bool py_gen = false;
  //
  //  while ((i = getopt(argc, argv, "chin:o:qpTtvz")) != -1) {
  //    switch (i) {
  //      case 'i':
  //        interactive = true;
  //        break;
  //      case 'p':
  //        py_gen = true;
  //        break;
  //      case 'v':
  //        ++verbosity;
  //        break;
  //      case 'o':
  //        output_filename = optarg;
  //        break;
  //      case 'c':
  //        tlbc::gen_cpp = true;
  //        break;
  //      case 'h':
  //        tlbc::gen_hpp = true;
  //        break;
  //      case 'q':
  //        no_code_gen = true;
  //        break;
  //      case 'n':
  //        tlbc::cpp_namespace = optarg;
  //        break;
  //      case 'T':
  //        tlbc::add_type_members = true;
  //        break;
  //      case 't':
  //        tlbc::show_tag_warnings = true;
  //        break;
  //      case 'z':
  //        tlbc::append_suffix = true;
  //        break;
  //      default:
  //        usage(argv[0]);
  //    }
  //  }
  //  if (verbosity >= 3) {
  //    tlbc::show_tag_warnings = true;
  //  }
  //
  //  src::define_keywords();
  //  tlbc::init_abstract_tables();
  //  tlbc::define_builtins();
  //
  //  int ok = 0, proc = 0;
  //  try {
  //    while (optind < argc) {
  //      tlbc::register_source(argv[optind]);
  //      ok += tlbc::parse_source_file(argv[optind++]);
  //      proc++;
  //    }
  //    if (interactive) {
  //      tlbc::register_source("");
  //      ok += tlbc::parse_source_stdin();
  //      proc++;
  //    }
  //    if (ok < proc) {
  //      throw src::Fatal{"output code generation omitted because of errors"};
  //    }
  //    if (!proc) {
  //      throw src::Fatal{"no source files, no output"};
  //    }
  //    tlbc::check_scheme();
  //    if (verbosity > 0) {
  //      tlbc::dump_all_types();
  //      tlbc::dump_all_constexpr();
  //    }
  //    if (!no_code_gen) {
  //      if (py_gen) {
  //        tlbc::init_forbidden_py_idents();
  //        tlbc::generate_py_output(output_filename);
  //      } else {
  //        tlbc::init_forbidden_cpp_idents();
  //        tlbc::generate_cpp_output(output_filename);
  //      }
  //    }
  //  } catch (src::Fatal& fatal) {
  //    std::cerr << "fatal: " << fatal << std::endl;
  //    std::exit(1);
  //  } catch (src::Error& error) {
  //    std::cerr << error << std::endl;
  //    std::exit(1);
  //  }
}
