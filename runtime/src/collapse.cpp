// Observation collapse. See stanli/collapse.hpp.
//
// Analysis. Every element that can reach a density argument or a target
// term gets a value number. Numbers are assigned by exact table lookup
// (opcode, variant, operand numbers or data bit patterns), never by hash
// alone: a collision must not be able to put two different observations in
// one group. Slots are mutable buffers, so the numbers live per slot element
// and are overwritten as the single forward pass reaches each write. Only
// slots in the backward cone of a term are numbered, and data that no op
// writes is numbered on demand, so a large data matrix costs nothing.
//
// Rewrite. A collapsing density is re-emitted over one representative per
// row, in its elementwise form, followed by a dot product with the row
// counts. Its arguments are restricted to the representatives by pushing a
// gather up through the ops that built them: f(a, b)[rows] is
// f(a[rows], b[rows]), a gather of a gather is one gather, a gather of data
// is smaller data. Where a rule does not apply, a plain gather of the full
// vector is always correct, so the proof each rule needs is local. The ops
// that built the full-length arguments are then removed if nothing else
// reads them.
#include <stanli/collapse.hpp>
#include <stanli/optable.hpp>

#include "collapse_linear.hpp"
#include "pass_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace stanli {
namespace {

using Vn = int64_t;

// Tags for table keys that are not an (opcode, variant) pair.
constexpr int64_t kDataTag = -1;
constexpr int64_t kSlotTag = -2;
constexpr int64_t kMatvecTag = -3;
constexpr int64_t kIdataTag = -4;
constexpr int64_t kPlaceTag = -5;
constexpr int64_t kRowTag = -6;

// A pure scalar op is keyed on its immediates too; past this many it is
// simply left unnumbered.
constexpr int64_t kMaxKeyedIdata = 64;

bool has_int_groups(uint16_t opcode) {
  return opcode == OP_BINOMIAL_LPMF || opcode == OP_BINOMIAL_LOGIT_LPMF ||
         opcode == OP_BETA_BINOMIAL_LPMF;
}

bool has_statistic(uint16_t opcode) {
  return opcode == OP_NORMAL_LPDF || opcode == OP_LOGNORMAL_LPDF;
}

bool is_density(uint16_t opcode) {
  return has_op_trait(opcode, op_trait::kRerollAnyDensity);
}

// cse.cpp's rule for ops that must run as often as the graph says.
bool never_merge(uint16_t oc) {
  return is_effectful_op(oc) || has_op_trait(oc, op_trait::kVariantGrouped) ||
         oc == OP_ISLAND || oc == OP_ODE || oc == OP_DAE ||
         oc == OP_ODE_ADJOINT;
}

// How an op's output elements follow from its inputs, for the ops this pass
// models. Every shape is a pure, whole write of `out`.
enum class Shape {
  kFresh,        // not modelled: every output element is its own value
  kElementwise,  // out[i] = f(in0[i or 0], in1[i or 0], ...)
  kGather,       // out[k] = in[idata[k]]
  kIndex,        // out[0] = in[idata[0]]
  kStore,        // out = in0 with out[idata[0]] = in1[0]
  kSliceRead,    // out[i] = in[start + i * stride]
  kSliceWrite,   // out = in0 with out[start + i * stride] = in1[i]
  kMatvec,       // out = X * beta, X column-major
  kPureScalar,   // one value, a function of its inputs and immediates
};

int64_t slice_stride(const Op& op) {
  switch (op.opcode) {
    case OP_SLICE:
    case OP_SET_SLICE:
    case OP_SET_SLICE_INPLACE:
      return op.n_idata == 1 ? 1 : 0;
    case OP_SLICE_STRIDED:
    case OP_SET_SLICE_STRIDED:
    case OP_SET_SLICE_STRIDED_INPLACE:
      return op.n_idata == 2 ? op.idata[1] : 0;
    default:
      return 0;
  }
}

Shape classify(const Graph& g, const Op& op) {
  if (op.out < 0 || op.out2 >= 0 || op.udata != nullptr || op.dyn_lengths)
    return Shape::kFresh;
  for (int j = 0; j < op.n_in; ++j)
    if (op.in[j] < 0) return Shape::kFresh;
  const int64_t len = g.slots[(size_t)op.out].len;
  const auto in_len = [&](int j) { return g.slots[(size_t)op.in[j]].len; };

  if ((has_op_trait(op.opcode, op_trait::kRerollWidenable) ||
       op.opcode == OP_BCAST_FMA) &&
      op.n_in >= 1 && op.n_in <= 3 && op.n_idata == 0) {
    bool fits = true;
    for (int j = 0; j < op.n_in; ++j)
      fits &= in_len(j) == 1 || in_len(j) == len;
    if (fits) return Shape::kElementwise;
  }
  switch (op.opcode) {
    case OP_GATHER:
      if (op.n_in != 1 || op.n_idata != len) return Shape::kFresh;
      for (int64_t k = 0; k < len; ++k)
        if (op.idata[k] < 0 || op.idata[k] >= in_len(0)) return Shape::kFresh;
      return Shape::kGather;
    case OP_INDEX:
      return op.n_in == 1 && op.n_idata == 1 && len == 1 && op.idata[0] >= 0 &&
                     op.idata[0] < in_len(0)
                 ? Shape::kIndex
                 : Shape::kFresh;
    case OP_SET_INDEX:
    case OP_SET_INDEX_INPLACE:
      return op.n_in == 2 && op.n_idata == 1 && in_len(0) == len &&
                     in_len(1) == 1 && op.idata[0] >= 0 && op.idata[0] < len
                 ? Shape::kStore
                 : Shape::kFresh;
    case OP_SLICE:
    case OP_SLICE_STRIDED: {
      const int64_t stride = slice_stride(op);
      return op.n_in == 1 && stride > 0 && op.idata[0] >= 0 &&
                     (len == 0 || op.idata[0] + (len - 1) * stride < in_len(0))
                 ? Shape::kSliceRead
                 : Shape::kFresh;
    }
    case OP_SET_SLICE:
    case OP_SET_SLICE_INPLACE:
    case OP_SET_SLICE_STRIDED:
    case OP_SET_SLICE_STRIDED_INPLACE: {
      const int64_t stride = slice_stride(op);
      return op.n_in == 2 && stride > 0 && in_len(0) == len &&
                     op.idata[0] >= 0 &&
                     (in_len(1) == 0 ||
                      op.idata[0] + (in_len(1) - 1) * stride < len)
                 ? Shape::kSliceWrite
                 : Shape::kFresh;
    }
    case OP_MATVEC:
      return op.n_in == 2 && op.n_idata == 2 && op.idata[0] == len &&
                     op.idata[1] == in_len(1) &&
                     in_len(0) == (int64_t)op.idata[0] * op.idata[1]
                 ? Shape::kMatvec
                 : Shape::kFresh;
    default:
      break;
  }
  if (len == 1 && op.n_idata <= kMaxKeyedIdata && !never_merge(op.opcode)) {
    const Kernel* kernel = find_kernel(op.opcode);
    if (kernel != nullptr && !kernel->make_state) return Shape::kPureScalar;
  }
  return Shape::kFresh;
}

// The outcome layout of an integer density: `width` observations, each
// reading one value per group. Flat immediates are one group; binomial-style
// immediates are [len, vals...] groups with len == -1 marking a scalar.
struct Outcomes {
  bool ok = true;
  bool grouped = false;
  int64_t width = 1;
  std::vector<std::pair<const int*, int64_t>> groups;  // values, length
  int at(size_t group, int64_t i) const {
    return groups[group].first[groups[group].second == 1 ? 0 : i];
  }
};

Outcomes outcomes_of(const Op& op) {
  Outcomes o;
  if (!has_op_trait(op.opcode, op_trait::kRerollIdataDensity)) return o;
  if (!has_int_groups(op.opcode)) {
    if (op.n_idata < 1) {
      o.ok = false;
      return o;
    }
    o.groups.emplace_back(op.idata, op.n_idata);
    o.width = op.n_idata;
    return o;
  }
  o.grouped = true;
  for (int64_t m = 0; m < op.n_idata;) {
    const int64_t len = op.idata[m] == -1 ? 1 : op.idata[m];
    if (len < 1 || m + 1 + len > op.n_idata ||
        (len != 1 && o.width != 1 && o.width != len)) {
      o.ok = false;
      return o;
    }
    if (len != 1) o.width = len;
    o.groups.emplace_back(op.idata + m + 1, len);
    m += 1 + len;
  }
  return o;
}

// The value table for keys of up to four words, which is nearly all of them:
// open addressing over the words themselves, so a lookup allocates nothing.
// Equality is of the words and their count, exactly; the hash only picks
// where to look.
class SmallTable {
 public:
  static constexpr size_t kWords = 4;

  void reserve(size_t entries) {
    size_t cells = 256;
    while (3 * cells < 4 * entries) cells *= 2;
    if (cells > cells_.size()) rehash(cells);
  }

  // The value stored under `w[0..n)`, or `fresh` after storing it.
  Vn find_or_insert(const int64_t* w, size_t n, Vn fresh) {
    if (4 * (used_ + 1) > 3 * cells_.size())
      rehash(cells_.empty() ? 256 : 2 * cells_.size());
    Cell probe;
    for (size_t k = 0; k < kWords; ++k) probe.w[k] = k < n ? w[k] : 0;
    probe.tag = hash(probe, n);
    const size_t mask = cells_.size() - 1;
    for (size_t at = (size_t)probe.tag & mask;; at = (at + 1) & mask) {
      Cell& cell = cells_[at];
      if (cell.value == 0) {
        cell = probe;
        cell.value = fresh;
        ++used_;
        return fresh;
      }
      if (cell.tag == probe.tag && cell.w[0] == probe.w[0] &&
          cell.w[1] == probe.w[1] && cell.w[2] == probe.w[2] &&
          cell.w[3] == probe.w[3])
        return cell.value;
    }
  }

 private:
  // `tag` is the hash with the word count in its low three bits, so two
  // keys of different lengths never compare equal.
  struct Cell {
    int64_t w[kWords];
    uint64_t tag = 0;
    Vn value = 0;  // value numbers start at one
  };

  static uint64_t hash(const Cell& c, size_t n) {
    uint64_t h = 0x9e3779b97f4a7c15ull;
    for (size_t k = 0; k < kWords; ++k) {
      h ^= (uint64_t)c.w[k];
      h *= 0xff51afd7ed558ccdull;
      h ^= h >> 29;
    }
    return (h << 3) | (uint64_t)n;
  }

  void rehash(size_t cells) {
    std::vector<Cell> old = std::move(cells_);
    cells_.assign(cells, Cell{});
    const size_t mask = cells - 1;
    for (const Cell& cell : old) {
      if (cell.value == 0) continue;
      size_t at = (size_t)cell.tag & mask;
      while (cells_[at].value != 0) at = (at + 1) & mask;
      cells_[at] = cell;
    }
  }

  std::vector<Cell> cells_;
  size_t used_ = 0;
};

struct Numbering {
  // The numbers of one slot's elements, inside `flat`.
  struct Span {
    Vn* p = nullptr;
    size_t n = 0;
    bool empty() const { return n == 0; }
    size_t size() const { return n; }
    Vn& operator[](size_t i) const { return p[i]; }
    Vn* begin() const { return p; }
    Vn* end() const { return p + n; }
    Span& operator=(const std::vector<Vn>& values) {
      std::copy(values.begin(), values.end(), p);
      return *this;
    }
  };

  const Graph& g;
  // Per element of every op-written cone slot, in one block: a vector per
  // slot is an allocation per scalar of an unrolled model.
  std::vector<Vn> flat;
  std::vector<int64_t> base;  // a slot's place in `flat`, or -1
  std::vector<char> in_cone;
  std::vector<int> n_writes;
  std::vector<int64_t> version;
  std::vector<Vn> param_base;
  std::unordered_map<int, const std::vector<double>*> fill_of;
  std::unordered_map<Key, Vn, KeyHash> table;  // the few longer keys
  SmallTable small;
  Key key;
  Vn next = 1;
  // Compare data by the slot element it sits in, not by its value: two
  // values are then equal only if ordinary CSE would see the same slots.
  bool by_slot = false;

  explicit Numbering(const Graph& graph)
      : g(graph),
        base(graph.slots.size(), -1),
        in_cone(graph.slots.size(), 0),
        n_writes(graph.slots.size(), 0),
        version(graph.slots.size(), 0),
        param_base(graph.slots.size(), 0) {}

  Span span(int s) {
    const int64_t at = base[(size_t)s];
    return at < 0 ? Span{}
                  : Span{flat.data() + at, (size_t)g.slots[(size_t)s].len};
  }

  Vn fresh() { return next++; }

  Vn intern() {
    if (key.w.size() <= SmallTable::kWords) {
      const Vn v = small.find_or_insert(key.w.data(), key.w.size(), next);
      if (v == next) ++next;
      return v;
    }
    const auto ins = table.emplace(key, next);
    if (ins.second) ++next;
    return ins.first->second;
  }

  // One number for a row of numbers, built a word at a time so that no key
  // is ever longer than the small table takes.
  Vn row(const std::vector<int64_t>& words) {
    Vn v = 0;
    for (const int64_t word : words) {
      const int64_t w[3] = {kRowTag, v, word};
      const Vn got = small.find_or_insert(w, 3, next);
      if (got == next) ++next;
      v = got;
    }
    return v;
  }

  Vn data(double x) {
    int64_t bits;
    std::memcpy(&bits, &x, sizeof bits);
    key.w.assign({kDataTag, bits});
    return intern();
  }

  // Element `at` of data slot s: by its value, or by where it sits.
  Vn datum(int s, size_t at, double x) {
    if (!by_slot) return data(x);
    key.w.assign({kPlaceTag, s, (int64_t)at});
    return intern();
  }

  // A whole slot as it stands now, for arguments taken as one value.
  Vn whole_slot(int s) {
    key.w.assign({kSlotTag, s, version[(size_t)s]});
    return intern();
  }

  const std::vector<double>* data_of(int s) const {
    if (g.slots[(size_t)s].is_param || n_writes[(size_t)s] != 0) return nullptr;
    const auto f = fill_of.find(s);
    return f != fill_of.end() &&
                   (int64_t)f->second->size() == g.slots[(size_t)s].len
               ? f->second
               : nullptr;
  }

  // Element i of slot s as it stands now; a length-one slot broadcasts.
  Vn elem(int s, int64_t i) {
    const size_t at = g.slots[(size_t)s].len == 1 ? 0 : (size_t)i;
    const Span v = span(s);
    if (!v.empty()) return v[at];
    if (n_writes[(size_t)s] == 0) {
      if (g.slots[(size_t)s].is_param) return param_base[(size_t)s] + (Vn)at;
      if (const auto* f = data_of(s)) return datum(s, at, (*f)[at]);
    }
    return fresh();  // outside the cone: equal to nothing
  }

  void fresh_slot(int s) {
    for (Vn& v : span(s)) v = fresh();
  }

  // The values of every cone slot written by ops, before any op has run.
  void materialize() {
    size_t elements = 0;
    for (size_t s = 0; s < g.slots.size(); ++s)
      if (in_cone[s] && n_writes[s] != 0) elements += (size_t)g.slots[s].len;
    // Gathers and stores copy numbers, so about half of the elements ever
    // reach the table; it grows if that is short.
    small.reserve(elements / 2);
    flat.assign(elements, 0);
    size_t placed = 0;
    for (size_t s = 0; s < g.slots.size(); ++s) {
      if (!in_cone[s] || n_writes[s] == 0 || g.slots[s].len == 0) continue;
      base[s] = (int64_t)placed;
      placed += (size_t)g.slots[s].len;
    }
    for (size_t s = 0; s < g.slots.size(); ++s) {
      if (base[s] < 0) continue;
      const Span v = span((int)s);
      const auto f = fill_of.find((int)s);
      if (!g.slots[s].is_param && f != fill_of.end() &&
          f->second->size() == v.size()) {
        for (size_t k = 0; k < v.size(); ++k)
          v[k] = datum((int)s, k, (*f->second)[k]);
      } else {
        fresh_slot((int)s);
      }
    }
  }

  // Numbers what `op` writes and advances the slot versions.
  void number(const Op& op, Shape shape, const std::vector<char>& active) {
    if (op.out >= 0 && in_cone[(size_t)op.out]) {
      const int out = op.out;
      Span vout = span(out);
      const int64_t len = (int64_t)vout.size();
      switch (shape) {
        case Shape::kElementwise: {
          std::vector<Vn> result((size_t)len);
          std::vector<Vn> operands((size_t)op.n_in);
          for (int64_t k = 0; k < len; ++k) {
            // Operands first: a lookup of data would clobber the key.
            for (int j = 0; j < op.n_in; ++j)
              operands[(size_t)j] = elem(op.in[j], k);
            key.w.assign({(int64_t)op.opcode | ((int64_t)op.variant << 16)});
            key.w.insert(key.w.end(), operands.begin(), operands.end());
            result[(size_t)k] = intern();
          }
          vout = result;
          break;
        }
        case Shape::kGather: {
          std::vector<Vn> result((size_t)len);
          for (int64_t k = 0; k < len; ++k)
            result[(size_t)k] = elem(op.in[0], op.idata[k]);
          vout = result;
          break;
        }
        case Shape::kIndex:
          vout[0] = elem(op.in[0], op.idata[0]);
          break;
        case Shape::kStore: {
          const Vn value = elem(op.in[1], 0);
          if (op.in[0] != out)
            for (int64_t k = 0; k < len; ++k)
              vout[(size_t)k] = elem(op.in[0], k);
          vout[(size_t)op.idata[0]] = value;
          break;
        }
        case Shape::kSliceRead: {
          const int64_t start = op.idata[0], stride = slice_stride(op);
          std::vector<Vn> result((size_t)len);
          for (int64_t k = 0; k < len; ++k)
            result[(size_t)k] = elem(op.in[0], start + k * stride);
          vout = result;
          break;
        }
        case Shape::kSliceWrite: {
          const int64_t start = op.idata[0], stride = slice_stride(op);
          const int64_t count = g.slots[(size_t)op.in[1]].len;
          std::vector<Vn> result((size_t)len);
          for (int64_t k = 0; k < len; ++k)
            result[(size_t)k] = elem(op.in[0], k);
          for (int64_t k = 0; k < count; ++k)
            result[(size_t)(start + k * stride)] = elem(op.in[1], k);
          vout = result;
          break;
        }
        case Shape::kMatvec: {
          // Rows of a data matrix times one vector: two rows agree when
          // their data agree bitwise. The vector is taken whole.
          const int64_t rows = op.idata[0], cols = op.idata[1];
          const auto* x =
              active[(size_t)op.in[0]] ? nullptr : data_of(op.in[0]);
          if (x == nullptr) {
            fresh_slot(out);
            break;
          }
          const Vn beta = whole_slot(op.in[1]);
          for (int64_t r = 0; r < rows; ++r) {
            key.w.assign({kMatvecTag, beta});
            if (by_slot) {
              key.w.push_back(op.in[0]);
              key.w.push_back(r);
              vout[(size_t)r] = intern();
              continue;
            }
            for (int64_t c = 0; c < cols; ++c) {
              int64_t bits;
              std::memcpy(&bits, &(*x)[(size_t)(c * rows + r)], sizeof bits);
              key.w.push_back(bits);
            }
            vout[(size_t)r] = intern();
          }
          break;
        }
        case Shape::kPureScalar: {
          std::vector<Vn> operands;
          for (int j = 0; j < op.n_in; ++j)
            operands.push_back(g.slots[(size_t)op.in[j]].len == 1
                                   ? elem(op.in[j], 0)
                                   : whole_slot(op.in[j]));
          key.w.assign({(int64_t)op.opcode | ((int64_t)op.variant << 16)});
          key.w.insert(key.w.end(), operands.begin(), operands.end());
          if (op.n_idata > 0) {
            key.w.push_back(kIdataTag);
            for (int64_t k = 0; k < op.n_idata; ++k)
              key.w.push_back(op.idata[k]);
          }
          vout[0] = intern();
          break;
        }
        case Shape::kFresh:
          fresh_slot(out);
          break;
      }
    }
    if (op.out2 >= 0 && in_cone[(size_t)op.out2]) fresh_slot(op.out2);
    if (op.out >= 0) ++version[(size_t)op.out];
    if (op.out2 >= 0) ++version[(size_t)op.out2];
  }
};

// What each slot held when: the ops that wrote it, in order. Read-only, so
// the analysis and the rewrite ask the same questions of the same graph.
struct History {
  const Graph& g;
  const Fills& fills;
  std::vector<std::vector<size_t>> writers;  // original op indices, rising
  std::unordered_map<int, size_t> fill_at;
  size_t p = 0;  // the op whose arguments are being examined

  History(const Graph& graph, const Fills& f)
      : g(graph), fills(f), writers(graph.slots.size()) {
    for (size_t i = 0; i < g.ops.size(); ++i) {
      const Op& op = g.ops[i];
      if (op.out >= 0) writers[(size_t)op.out].push_back(i);
      if (op.out2 >= 0 && op.out2 != op.out)
        writers[(size_t)op.out2].push_back(i);
    }
    for (size_t k = 0; k < fills.size(); ++k)
      fill_at.emplace(fills[k].first, k);
  }

  int64_t len(int s) const { return g.slots[(size_t)s].len; }

  // The last op writing s before position `pos`, or -1.
  long last_writer_before(int s, size_t pos) const {
    if ((size_t)s >= writers.size()) return -1;  // a slot this pass made
    const auto& w = writers[(size_t)s];
    const auto it = std::lower_bound(w.begin(), w.end(), pos);
    return it == w.begin() ? -1 : (long)*(it - 1);
  }

  // Is s written by an op in [lo, hi)?
  bool written_in(int s, size_t lo, size_t hi) const {
    if ((size_t)s >= writers.size()) return false;
    const auto& w = writers[(size_t)s];
    const auto it = std::lower_bound(w.begin(), w.end(), lo);
    return it != w.end() && *it < hi;
  }

  // Data no op has written before `at`, or nullptr.
  const std::vector<double>* data_before(int s, size_t at) const {
    if (g.slots[(size_t)s].is_param || last_writer_before(s, at) >= 0)
      return nullptr;
    const auto f = fill_at.find(s);
    if (f == fill_at.end()) return nullptr;
    const auto& values = fills[f->second].second;
    return (int64_t)values.size() == len(s) ? &values : nullptr;
  }

  // Where one element of a stored-into vector came from.
  struct Source {
    enum Kind { kNone, kScalar, kElement } kind = kNone;
    int slot = -1;
    int index = 0;
    size_t at = 0;  // the element is unchanged from here to p
  };

  // Walks back through the stores into s to the one that wrote element e.
  Source source_of(int s, int e, size_t at) const {
    int c = s;
    size_t pos = at;
    for (;;) {
      const long q = last_writer_before(c, pos);
      if (q < 0) return Source{Source::kElement, c, e, pos};
      const Op& w = g.ops[(size_t)q];
      const Shape sh = w.out == c ? classify(g, w) : Shape::kFresh;
      if (sh == Shape::kStore || sh == Shape::kSliceWrite) {
        const int64_t stride = sh == Shape::kStore ? 1 : slice_stride(w);
        const int64_t count = sh == Shape::kStore ? 1 : len(w.in[1]);
        const int64_t k = e - w.idata[0];
        if (k >= 0 && k % stride == 0 && k / stride < count) {
          const int src = w.in[1];
          if (src == c || written_in(src, (size_t)q + 1, p)) return Source{};
          if (sh == Shape::kStore)
            return Source{Source::kScalar, src, 0, (size_t)q};
          return Source{Source::kElement, src, (int)(k / stride), (size_t)q};
        }
        if (w.in[0] != c) {
          // A copying store: the element is the base's, which must then
          // hold still until p.
          c = w.in[0];
          if (written_in(c, (size_t)q, p)) return Source{};
        }
        pos = (size_t)q;
        continue;
      }
      // Some other op wrote all of c; every later writer was a store that
      // left this element alone.
      return Source{Source::kElement, c, e, (size_t)q + 1};
    }
  }
};

// Locations as affine functions of what they are built from: a constant
// plus coefficients on "leaves", the slot elements the arithmetic starts
// from. Sums, differences, and products and quotients by known data are
// followed; anything else is a leaf. The rules for when a writer's inputs
// may be read in its place are the rewrite's (Rewriter::restrict_uncached).
struct Affine {
  const History& h;
  std::vector<std::pair<int, int>> leaves;  // slot, element
  std::map<std::pair<int, int>, int> leaf_of;
  int64_t budget;  // terms this analysis may still create
  using Rows = std::vector<LinearRow>;
  std::map<std::pair<std::pair<int, size_t>, std::vector<int>>, Rows> memo;

  Affine(const History& history, int64_t max_terms)
      : h(history), budget(max_terms) {}

  bool failed() const { return budget < 0; }

  LinearRow leaf(int s, int e) {
    const auto ins = leaf_of.emplace(std::make_pair(s, e), (int)leaves.size());
    if (ins.second) leaves.emplace_back(s, e);
    LinearRow r;
    r.terms.emplace_back(ins.first->second, 1.0);
    return r;
  }

  // into += k * x, both sorted by leaf.
  void axpy(LinearRow& into, double k, const LinearRow& x) {
    into.constant += k * x.constant;
    if (x.terms.empty()) return;
    std::vector<std::pair<int, double>> merged;
    merged.reserve(into.terms.size() + x.terms.size());
    size_t a = 0, b = 0;
    while (a < into.terms.size() || b < x.terms.size()) {
      if (b == x.terms.size() ||
          (a < into.terms.size() && into.terms[a].first < x.terms[b].first)) {
        merged.push_back(into.terms[a++]);
      } else if (a == into.terms.size() ||
                 x.terms[b].first < into.terms[a].first) {
        merged.emplace_back(x.terms[b].first, k * x.terms[b].second);
        ++b;
      } else {
        merged.emplace_back(into.terms[a].first,
                            into.terms[a].second + k * x.terms[b].second);
        ++a;
        ++b;
      }
    }
    budget -= (int64_t)merged.size();
    into.terms = std::move(merged);
  }

  // into = a * b when one of them is a known constant.
  bool product(LinearRow& into, const LinearRow& a, const LinearRow& b) {
    if (!a.terms.empty() && !b.terms.empty()) return false;
    const LinearRow& scaled = a.terms.empty() ? b : a;
    const double k = a.terms.empty() ? a.constant : b.constant;
    into = LinearRow{};
    axpy(into, k, scaled);
    return true;
  }

  // s[which], for s as it stands just before op `at`; the chosen elements
  // must not change between `at` and h.p.
  Rows rows(int s, const std::vector<int>& which, size_t at) {
    if (h.len(s) == 1 && (which.size() != 1 || which[0] != 0)) {
      const Rows one = rows(s, {0}, at);
      return Rows(which.size(), one[0]);
    }
    auto key = std::make_pair(std::make_pair(s, at), which);
    const auto hit = memo.find(key);
    if (hit != memo.end()) return hit->second;
    Rows out = rows_uncached(s, which, at);
    if (!failed()) memo.emplace(std::move(key), out);
    return out;
  }

  Rows rows_uncached(int s, const std::vector<int>& which, size_t at) {
    Rows out(which.size());
    if (failed()) return out;
    const auto all_leaves = [&] {
      for (size_t k = 0; k < which.size(); ++k) out[k] = leaf(s, which[k]);
      budget -= (int64_t)which.size();
      return out;
    };
    const long q = h.last_writer_before(s, at);
    if (q < 0) {
      const auto* values = h.data_before(s, at);
      if (values == nullptr) return all_leaves();
      for (size_t k = 0; k < which.size(); ++k)
        out[k].constant = (*values)[(size_t)which[k]];
      return out;
    }
    const Op& w = h.g.ops[(size_t)q];
    if (w.out != s) return all_leaves();
    const auto holds = [&](int in) {
      return in != s && !h.written_in(in, (size_t)q + 1, h.p);
    };
    switch (classify(h.g, w)) {
      case Shape::kElementwise: {
        switch (w.opcode) {
          case OP_ADD:
          case OP_SUB:
          case OP_NEG:
          case OP_MUL:
          case OP_DIV:
          case OP_FMA:
          case OP_BCAST_FMA:
            break;
          default:
            return all_leaves();
        }
        const int arity = w.opcode == OP_NEG                               ? 1
                          : w.opcode == OP_FMA || w.opcode == OP_BCAST_FMA ? 3
                                                                           : 2;
        if (w.variant != 0 || w.n_in != arity) return all_leaves();
        for (int j = 0; j < w.n_in; ++j)
          if (!holds(w.in[j])) return all_leaves();
        std::vector<Rows> in((size_t)w.n_in);
        for (int j = 0; j < w.n_in; ++j)
          in[(size_t)j] = rows(w.in[j], which, h.p);
        for (size_t k = 0; k < which.size() && !failed(); ++k) {
          LinearRow r;
          bool ok = true;
          switch (w.opcode) {
            case OP_ADD:
              r = in[0][k];
              axpy(r, 1.0, in[1][k]);
              break;
            case OP_SUB:
              r = in[0][k];
              axpy(r, -1.0, in[1][k]);
              break;
            case OP_NEG:
              axpy(r, -1.0, in[0][k]);
              break;
            case OP_MUL:
              ok = product(r, in[0][k], in[1][k]);
              break;
            case OP_DIV:
              ok = in[1][k].terms.empty() && in[1][k].constant != 0;
              if (ok) axpy(r, 1.0 / in[1][k].constant, in[0][k]);
              break;
            case OP_FMA:  // in0 * in1 + in2
              ok = product(r, in[0][k], in[1][k]);
              if (ok) axpy(r, 1.0, in[2][k]);
              break;
            default:  // OP_BCAST_FMA: in0 + in1 * in2
              ok = product(r, in[1][k], in[2][k]);
              if (ok) axpy(r, 1.0, in[0][k]);
              break;
          }
          out[k] = ok ? std::move(r) : leaf(s, which[k]);
        }
        return out;
      }
      case Shape::kGather:
      case Shape::kSliceRead: {
        if (!holds(w.in[0])) return all_leaves();
        const bool gather = w.opcode == OP_GATHER;
        const int64_t start = gather ? 0 : w.idata[0];
        const int64_t stride = gather ? 1 : slice_stride(w);
        std::vector<int> through;
        through.reserve(which.size());
        for (int k : which)
          through.push_back(gather ? w.idata[k] : (int)(start + k * stride));
        return rows(w.in[0], through, h.p);
      }
      case Shape::kIndex:
        if (!holds(w.in[0])) return all_leaves();
        return rows(w.in[0], {w.idata[0]}, h.p);
      case Shape::kMatvec: {
        const int64_t n_rows = w.idata[0], cols = w.idata[1];
        const auto* x = h.data_before(w.in[0], h.p);
        if (x == nullptr || !h.writers[(size_t)w.in[0]].empty() ||
            !holds(w.in[1]))
          return all_leaves();
        std::vector<int> every((size_t)cols);
        for (int64_t c = 0; c < cols; ++c) every[(size_t)c] = (int)c;
        const Rows beta = rows(w.in[1], every, h.p);
        for (size_t k = 0; k < which.size() && !failed(); ++k)
          for (int64_t c = 0; c < cols; ++c) {
            const double coefficient = (*x)[(size_t)(c * n_rows + which[k])];
            if (coefficient != 0) axpy(out[k], coefficient, beta[(size_t)c]);
          }
        return out;
      }
      case Shape::kStore:
      case Shape::kSliceWrite:
        for (size_t k = 0; k < which.size() && !failed(); ++k) {
          const History::Source src = h.source_of(s, which[k], at);
          if (src.kind == History::Source::kNone ||
              (src.slot == s && src.at == at))
            out[k] = leaf(s, which[k]);
          else
            out[k] = rows(src.slot, {src.index}, src.at)[0];
        }
        return out;
      default:
        return all_leaves();
    }
  }
};

// A vector density that collapses: one representative observation per row.
struct VectorPlan {
  size_t op;
  size_t term = 0;  // its entry in the report
  std::vector<int> rep;
  std::vector<int> count;
};

// A normal or lognormal term that collapses to per-group statistics.
struct StatisticPlan {
  size_t op;
  size_t term = 0;                 // its entry in the report
  std::vector<int> rep;            // first observation of each group
  std::vector<double> statistics;  // grouped_statistics' layout
  double variate_constant = 0;     // lognormal's -sum(log y)
  int scale = -1;                  // the scale's slot
  uint8_t variant = 0;             // the closed-form kernel's
  bool propto = false;
  // Set when the locations are affine in few enough values: those values,
  // in the order the kernel reads them, and its data.
  std::vector<std::pair<int, int>> leaves;  // slot, element
  std::vector<double> linear;
};

// Locations affine in more values than this are left to the group form.
constexpr int64_t kLinearMaxParameters = 256;
// A bound on the analysis itself.
constexpr int64_t kLinearMaxTerms = 4000000;

// A quadratic form is worth it when its triangular products are at most
// half the work of forming the locations group by group.
bool worth_a_line(const Affine& affine, const Affine::Rows& lines) {
  const int64_t p = (int64_t)affine.leaves.size();
  const int64_t groups = (int64_t)lines.size();
  int64_t terms = 0;
  for (const LinearRow& line : lines) terms += (int64_t)line.terms.size();
  return !affine.failed() && p >= 1 && p <= kLinearMaxParameters &&
         p <= groups && 2 * p * p <= terms + 3 * groups;
}

// Fills in the plan's quadratic form from its groups' statistics. The
// kernel reads the leaves slot by slot, so they are put in that order.
bool line_data(StatisticPlan& stat, const Affine& affine, Affine::Rows& lines) {
  const size_t p = affine.leaves.size();
  std::vector<int> order(p), place(p);
  for (size_t k = 0; k < p; ++k) order[k] = (int)k;
  std::sort(order.begin(), order.end(), [&](int a, int b) {
    return affine.leaves[(size_t)a] < affine.leaves[(size_t)b];
  });
  for (size_t k = 0; k < p; ++k) place[(size_t)order[k]] = (int)k;
  for (LinearRow& line : lines)
    for (auto& term : line.terms) term.first = place[(size_t)term.first];
  stat.linear = linear_gaussian_data((int64_t)p, lines, stat.statistics);
  if (stat.linear.empty()) return false;
  for (size_t k = 0; k < p; ++k)
    stat.leaves.push_back(affine.leaves[(size_t)order[k]]);
  return true;
}

struct Analysis {
  CollapseReport report;
  std::vector<VectorPlan> vectors;
  std::vector<StatisticPlan> statistics;
  // Scalar target terms by value, in order of first appearance.
  std::vector<std::pair<int, int>> scalars;  // first slot, count
  std::vector<char> scalar_merges;           // per value
  std::vector<int> scalar_group;             // per scalar term, in order
  int scalars_merged = 0;
};

Analysis analyze(const Graph& g, const Fills& fills,
                 const std::vector<int>& target_terms) {
  Analysis a;
  CollapseReport& report = a.report;
  const size_t n_slots = g.slots.size();
  const size_t n_ops = g.ops.size();

  History history(g, fills);
  Numbering num(g);
  for (const auto& f : fills) num.fill_of.emplace(f.first, &f.second);
  std::vector<char> active(n_slots, 0);
  for (size_t s = 0; s < n_slots; ++s) {
    active[s] = g.slots[s].is_param;
    if (g.slots[s].is_param) {
      num.param_base[s] = num.next;
      num.next += g.slots[s].len;
    }
  }
  std::vector<char> op_active(n_ops, 0);
  std::vector<Shape> shape(n_ops);
  for (size_t i = 0; i < n_ops; ++i) {
    const Op& op = g.ops[i];
    shape[i] = classify(g, op);
    if (op.udata != nullptr) ++report.opaque_ops[op.opcode];
    bool any = false;
    for (int j = 0; j < op.n_in; ++j)
      if (op.in[j] >= 0) any |= active[(size_t)op.in[j]] != 0;
    op_active[i] = any;
    if (op.out >= 0) {
      active[(size_t)op.out] |= any;
      ++num.n_writes[(size_t)op.out];
    }
    if (op.out2 >= 0) {
      active[(size_t)op.out2] |= any;
      ++num.n_writes[(size_t)op.out2];
    }
  }

  // Vector candidates: densities over more than one observation whose
  // inputs depend on a parameter. A density of data alone is a constant.
  std::vector<char> candidate(n_ops, 0);
  for (size_t i = 0; i < n_ops; ++i) {
    const Op& op = g.ops[i];
    if (!is_density(op.opcode) || !op_active[i] || op.out < 0 ||
        op.udata != nullptr)
      continue;
    bool vector = outcomes_of(op).width > 1;
    for (int j = 0; j < op.n_in; ++j)
      vector |= op.in[j] >= 0 && g.slots[(size_t)op.in[j]].len > 1;
    if (!vector) continue;
    candidate[i] = 1;
    for (int j = 0; j < op.n_in; ++j)
      if (op.in[j] >= 0) num.in_cone[(size_t)op.in[j]] = 1;
  }
  for (int t : target_terms)
    if (t >= 0 && g.slots[(size_t)t].len == 1) num.in_cone[(size_t)t] = 1;
  // normal_id_glm(y | X, alpha, beta, sigma): the intercept is compared by
  // value, the design by its rows.
  for (size_t i = 0; i < n_ops; ++i)
    if (g.ops[i].opcode == OP_NORMAL_ID_GLM_LPDF && g.ops[i].n_in == 5 &&
        g.ops[i].in[2] >= 0)
      num.in_cone[(size_t)g.ops[i].in[2]] = 1;

  // Ops are in evaluation order, so one reverse sweep closes the cone.
  for (size_t i = n_ops; i-- > 0;) {
    const Op& op = g.ops[i];
    if (shape[i] == Shape::kFresh || shape[i] == Shape::kMatvec ||
        !num.in_cone[(size_t)op.out])
      continue;
    for (int j = 0; j < op.n_in; ++j)
      if (shape[i] != Shape::kPureScalar || g.slots[(size_t)op.in[j]].len == 1)
        num.in_cone[(size_t)op.in[j]] = 1;
  }
  // A second numbering that tells data apart by place: what it calls equal,
  // the later CSE pass merges anyway.
  Numbering same = num;
  same.by_slot = true;
  num.materialize();

  const auto refuse = [](CollapseTerm& t, const char* why) {
    if (t.refusal == nullptr) t.refusal = why;
  };
  std::vector<long> last_writer(n_slots, -1);
  Key row;

  for (size_t i = 0; i < n_ops; ++i) {
    const Op& op = g.ops[i];

    if (candidate[i]) {
      const Outcomes outcomes = outcomes_of(op);
      const bool real_variate = outcomes.groups.empty();
      int64_t n = outcomes.width;
      bool shaped = outcomes.ok && !op.dyn_lengths;
      for (int j = 0; j < op.n_in && shaped; ++j) {
        if (op.in[j] < 0) {
          shaped = false;
          break;
        }
        const int64_t len = g.slots[(size_t)op.in[j]].len;
        if (len == 1) continue;
        if (n != 1 && n != len) shaped = false;
        n = len;
      }
      // Flat outcomes are read one per observation.
      if (!real_variate && !outcomes.grouped && outcomes.width != n)
        shaped = false;
      CollapseTerm t;
      t.op = (int)i;
      t.opcode = op.opcode;
      t.ops = 1;
      t.n = n;
      t.variate_is_data =
          !real_variate || (op.n_in > 0 && !active[(size_t)op.in[0]]);
      if (!shaped) {
        refuse(t, "argument shapes are not per observation");
      } else {
        // One row key per observation: outcomes, then each argument.
        const auto row_key = [&](int64_t obs, bool with_variate) {
          row.w.clear();
          if (with_variate)
            for (size_t q = 0; q < outcomes.groups.size(); ++q)
              row.w.push_back(outcomes.at(q, obs));
          for (int j = (real_variate && !with_variate) ? 1 : 0; j < op.n_in;
               ++j)
            row.w.push_back(num.elem(op.in[j], obs));
        };
        VectorPlan plan;
        plan.op = i;
        std::unordered_map<Vn, int> group_of;
        for (int64_t obs = 0; obs < n; ++obs) {
          row_key(obs, true);
          const auto ins =
              group_of.emplace(num.row(row.w), (int)plan.rep.size());
          if (ins.second) {
            plan.rep.push_back((int)obs);
            plan.count.push_back(1);
          } else {
            ++plan.count[(size_t)ins.first->second];
          }
        }
        t.rows = (int64_t)plan.rep.size();
        StatisticPlan stat;
        stat.op = i;
        bool by_statistic = false, by_line = false;
        const bool summed = g.slots[(size_t)op.out].len == 1;
        if (has_statistic(op.opcode) && t.variate_is_data && op.n_in == 3) {
          const bool lognormal = op.opcode == OP_LOGNORMAL_LPDF;
          std::vector<int> member;
          group_of.clear();
          member.reserve((size_t)n);
          for (int64_t obs = 0; obs < n; ++obs) {
            row_key(obs, false);
            const auto ins =
                group_of.emplace(num.row(row.w), (int)stat.rep.size());
            if (ins.second) stat.rep.push_back((int)obs);
            member.push_back(ins.first->second);
          }
          t.groups = (int64_t)stat.rep.size();
          // The statistics are computed from the data now, so the variate
          // has to be data no op writes, and every value one Stan accepts
          // with a finite density.
          const auto* y = g.slots[(size_t)op.in[0]].len == n
                              ? num.data_of(op.in[0])
                              : nullptr;
          bool usable = y != nullptr && summed && n >= kCollapseMinObservations;
          for (int64_t obs = 0; usable && obs < n; ++obs) {
            const double v = (*y)[(size_t)obs];
            usable = std::isfinite(v) && (!lognormal || v > 0);
          }
          by_statistic = usable && kCollapseNativeRatio * t.groups <= n;
          // One scale for every observation: the locations may be affine in
          // a few values, and then the whole term is a quadratic form.
          bool try_line = usable && g.slots[(size_t)op.in[2]].len == 1;
          Affine affine(history, kLinearMaxTerms);
          Affine::Rows lines;
          if (try_line) {
            history.p = i;
            lines = affine.rows(op.in[1], stat.rep, i);
            try_line = worth_a_line(affine, lines);
          }
          if (by_statistic || try_line) {
            std::vector<double> values = *y;
            Compensated log_y;
            if (lognormal)
              for (double& v : values) {
                v = std::log(v);
                log_y.add(v);
              }
            stat.variate_constant = -log_y.value();
            stat.statistics =
                grouped_statistics(values, member, stat.rep.size());
          }
          // An unset variant is the full density, every argument active.
          stat.scale = op.in[2];
          stat.propto = (op.variant & 0x80u) != 0;
          stat.variant = (uint8_t)((op.variant == 0 || (op.variant & 0x02u)
                                        ? kGroupedLocationActive
                                        : 0) |
                                   (op.variant == 0 || (op.variant & 0x04u)
                                        ? kGroupedScaleActive
                                        : 0) |
                                   (lognormal ? kGroupedLognormal : 0) |
                                   (stat.propto ? kGroupedPropto : 0));
          by_line = try_line && line_data(stat, affine, lines);
        }
        const int64_t ratio = lane_elt_costs_per_element(op.opcode)
                                  ? kCollapseRecorderRatio
                                  : kCollapseNativeRatio;
        if (!summed) refuse(t, "elementwise output");
        if (n < kCollapseMinObservations) refuse(t, "too few observations");
        if (!by_statistic && !by_line && ratio * t.rows > n)
          refuse(t, "too few repeated rows");
        if (t.refusal == nullptr) {
          t.evaluator = by_line ? "linear" : by_statistic ? "groups" : "rows";
          stat.term = plan.term = report.terms.size();
          if (by_line || by_statistic)
            a.statistics.push_back(std::move(stat));
          else
            a.vectors.push_back(std::move(plan));
        }
      }
      report.terms.push_back(t);
    }

    if (op.opcode == OP_NORMAL_ID_GLM_LPDF && op_active[i] && op.n_in == 5 &&
        op.n_idata >= 2 && op.out >= 0 && op.udata == nullptr) {
      // The same density with its locations already written as a design
      // times a vector: a quadratic form or nothing.
      const int64_t rows = op.idata[0], cols = op.idata[1];
      CollapseTerm t;
      t.op = (int)i;
      t.opcode = op.opcode;
      t.ops = 1;
      t.n = t.rows = rows;
      t.variate_is_data = (op.variant & 0x01u) == 0;
      const auto* y = num.data_of(op.in[0]);
      const auto* x = num.data_of(op.in[1]);
      const auto in_len = [&](int j) { return g.slots[(size_t)op.in[j]].len; };
      bool usable = y != nullptr && x != nullptr && (op.variant & 0x03u) == 0 &&
                    !op.dyn_lengths && !(op.n_idata >= 5 && op.idata[4] == 1) &&
                    in_len(0) == rows && in_len(1) == rows * cols &&
                    (in_len(2) == 1 || in_len(2) == rows) &&
                    in_len(3) == cols && in_len(4) == 1 &&
                    g.slots[(size_t)op.out].len == 1 &&
                    rows >= kCollapseMinObservations;
      for (int64_t obs = 0; usable && obs < rows; ++obs)
        usable = std::isfinite((*y)[(size_t)obs]);
      StatisticPlan stat;
      stat.op = i;
      bool by_line = false;
      if (usable) {
        std::vector<int> member;
        std::unordered_map<Vn, int> group_of;
        member.reserve((size_t)rows);
        for (int64_t obs = 0; obs < rows; ++obs) {
          row.w.assign({num.elem(op.in[2], obs)});
          for (int64_t c = 0; c < cols; ++c) {
            int64_t bits;
            std::memcpy(&bits, &(*x)[(size_t)(c * rows + obs)], sizeof bits);
            row.w.push_back(bits);
          }
          const auto ins =
              group_of.emplace(num.row(row.w), (int)stat.rep.size());
          if (ins.second) stat.rep.push_back((int)obs);
          member.push_back(ins.first->second);
        }
        t.groups = (int64_t)stat.rep.size();
        history.p = i;
        Affine affine(history, kLinearMaxTerms);
        Affine::Rows lines = affine.rows(op.in[2], stat.rep, i);
        std::vector<int> every((size_t)cols);
        for (int64_t c = 0; c < cols; ++c) every[(size_t)c] = (int)c;
        const Affine::Rows beta = affine.rows(op.in[3], every, i);
        for (size_t k = 0; k < lines.size() && !affine.failed(); ++k)
          for (int64_t c = 0; c < cols; ++c) {
            const double coefficient = (*x)[(size_t)(c * rows + stat.rep[k])];
            if (coefficient != 0)
              affine.axpy(lines[k], coefficient, beta[(size_t)c]);
          }
        if (worth_a_line(affine, lines)) {
          stat.statistics = grouped_statistics(*y, member, stat.rep.size());
          // This kernel binds its scale as a parameter whatever it is, so
          // Stan keeps the scale's log term under propto too.
          stat.scale = op.in[4];
          stat.propto = (op.variant & 0x80u) != 0;
          stat.variant =
              (uint8_t)(kGroupedLocationActive | kGroupedScaleActive |
                        kGroupedGlm | (stat.propto ? kGroupedPropto : 0));
          by_line = line_data(stat, affine, lines);
        }
      }
      if (by_line) {
        t.evaluator = "linear";
        stat.term = report.terms.size();
        a.statistics.push_back(std::move(stat));
      } else {
        refuse(t, usable ? "too few repeated rows"
                         : "argument shapes are not per observation");
      }
      report.terms.push_back(t);
    }

    num.number(op, shape[i], active);
    if (op.out >= 0) last_writer[(size_t)op.out] = (long)i;
    if (op.out2 >= 0) last_writer[(size_t)op.out2] = (long)i;
  }

  // Scalar target terms: equal values are one term and a count.
  std::unordered_map<Vn, size_t> scalar_of;
  std::vector<std::unordered_set<Vn>> places;  // per value
  struct ByOpcode {
    int first_op;
    int64_t n = 0;
    std::unordered_set<Vn> values;
  };
  std::map<uint16_t, ByOpcode> by_opcode;
  for (int t : target_terms) {
    if (t < 0 || g.slots[(size_t)t].len != 1) continue;
    const Vn v = num.elem(t, 0);
    const auto ins = scalar_of.emplace(v, a.scalars.size());
    if (ins.second) {
      a.scalars.emplace_back(t, 1);
      places.emplace_back();
    } else {
      ++a.scalars[ins.first->second].second;
    }
    a.scalar_group.push_back((int)ins.first->second);
    // A vector density's own sum is reported with that density.
    const long producer = last_writer[(size_t)t];
    if (producer < 0 || candidate[(size_t)producer]) continue;
    const uint16_t opcode = g.ops[(size_t)producer].opcode;
    const auto entry = by_opcode.emplace(opcode, ByOpcode{(int)producer});
    ++entry.first->second.n;
    entry.first->second.values.insert(v);
  }
  // Every repeated value is merged, or none is: merging only some leaves
  // the later lane fusion a mix it prices badly (measured on
  // dogs_hierarchical and multi_occupancy). What merging saves, in op
  // dispatches: each further set of slots a value shows up through is at
  // least one op only this pass can save, and each repeat is one input
  // fewer to the target sum (a fifth of an op). Terms that read the very
  // same slots are merged by CSE next for nothing, so they count for the
  // sum alone. What it costs is one multiply per merged value.
  // That second numbering is run only when some value does repeat.
  bool any_repeat = false;
  for (const auto& term : a.scalars) any_repeat |= term.second >= 2;
  if (any_repeat) {
    same.materialize();
    for (size_t i = 0; i < n_ops; ++i) same.number(g.ops[i], shape[i], active);
    size_t next = 0;
    for (int t : target_terms)
      if (t >= 0 && g.slots[(size_t)t].len == 1)
        places[(size_t)a.scalar_group[next++]].insert(same.elem(t, 0));
  }
  double saved = 0;
  int repeats = 0;
  for (size_t k = 0; k < a.scalars.size(); ++k) {
    const int count = a.scalars[k].second;
    a.scalar_merges.push_back(count >= 2);
    if (count < 2) continue;
    repeats += count - 1;
    saved += (double)(places[k].size() - 1) + 0.2 * (count - 1) - 1.0;
  }
  if (saved >= (double)kCollapseMinObservations) a.scalars_merged = repeats;
  for (const auto& entry : by_opcode) {
    if (entry.second.n < 2) continue;
    CollapseTerm t;
    t.op = entry.second.first_op;
    t.opcode = entry.first;
    t.ops = t.n = entry.second.n;
    t.rows = (int64_t)entry.second.values.size();
    t.variate_is_data = true;
    if (a.scalars_merged == 0) refuse(t, "merging would not pay");
    if (t.rows == t.n) refuse(t, "too few repeated rows");
    if (t.refusal == nullptr) t.evaluator = "rows";
    report.terms.push_back(t);
  }
  return a;
}

// Emits the ops that restrict a slot to chosen elements, as it stands just
// before one original op.
struct Rewriter : History {
  Graph& graph;
  Fills& data;
  std::vector<std::vector<Op>> before;  // new ops, by the op they precede
  std::map<std::pair<std::pair<int, size_t>, std::vector<int>>, int> memo;
  int fallbacks = 0;

  Rewriter(Graph& mutable_graph, Fills& mutable_fills)
      : History(mutable_graph, mutable_fills),
        graph(mutable_graph),
        data(mutable_fills),
        before(mutable_graph.ops.size()) {}

  void attach(Op& op, std::vector<int> idata) {
    graph.idata_pool.push_back(std::move(idata));
    op.idata = graph.idata_pool.back().data();
    op.n_idata = (int64_t)graph.idata_pool.back().size();
  }

  int data_slot(std::vector<double> values) {
    const int s = graph.add_slot((int64_t)values.size(), false);
    data.emplace_back(s, std::move(values));
    return s;
  }

  // A gather of a vector some op built in full: correct, but the ops that
  // built it still run at full length.
  int fallback(int s, const std::vector<int>& which) {
    ++fallbacks;
    return gather(s, which);
  }

  // Everything emitted since a mark can be taken back.
  struct Mark {
    size_t slots, fills, idata, ops;
  };
  Mark mark() const {
    return Mark{graph.slots.size(), data.size(), graph.idata_pool.size(),
                before[p].size()};
  }
  void rollback(const Mark& m) {
    graph.slots.resize(m.slots);
    data.resize(m.fills);
    graph.idata_pool.resize(m.idata);
    before[p].resize(m.ops);
    memo.clear();
  }

  int gather(int s, const std::vector<int>& which) {
    Op op;
    op.opcode = OP_GATHER;
    op.n_in = 1;
    op.in[0] = s;
    op.out = graph.add_slot((int64_t)which.size(), false);
    attach(op, which);
    before[p].push_back(op);
    return op.out;
  }

  // One vector of the given slot elements, in order, which is sorted by
  // slot: a slot that is wanted whole and alone is used as it is.
  int packed(const std::vector<std::pair<int, int>>& elements) {
    const int64_t total = (int64_t)elements.size();
    const int first = elements[0].first;
    bool whole = len(first) == total;
    for (int64_t k = 0; whole && k < total; ++k)
      whole = elements[(size_t)k] == std::make_pair(first, (int)k);
    if (whole) return first;
    const int base = data_slot(std::vector<double>((size_t)total, 0.0));
    const int out = graph.add_slot(total, false);
    for (int64_t at = 0; at < total;) {
      const int s = elements[(size_t)at].first;
      std::vector<int> which;
      int64_t next = at;
      for (; next < total && elements[(size_t)next].first == s; ++next)
        which.push_back(elements[(size_t)next].second);
      bool in_order = (int64_t)which.size() == len(s);
      for (size_t k = 0; in_order && k < which.size(); ++k)
        in_order = which[k] == (int)k;
      Op op;
      const bool scalar = len(s) == 1;
      op.opcode = scalar ? (at == 0 ? OP_SET_INDEX : OP_SET_INDEX_INPLACE)
                         : (at == 0 ? OP_SET_SLICE : OP_SET_SLICE_INPLACE);
      op.n_in = 2;
      op.in[0] = at == 0 ? base : out;
      op.in[1] = scalar || in_order ? s : gather(s, which);
      op.out = out;
      attach(op, {(int)at});
      before[p].push_back(op);
      at = next;
    }
    return out;
  }

  // A slot holding s[which], for s as it stands just before op `at`. The
  // chosen elements must not change between `at` and p.
  int restricted(int s, const std::vector<int>& which, size_t at) {
    if (len(s) == 1) return s;
    auto key = std::make_pair(std::make_pair(s, at), which);
    const auto hit = memo.find(key);
    if (hit != memo.end()) return hit->second;
    const int r = restrict_uncached(s, which, at);
    memo.emplace(std::move(key), r);
    return r;
  }

  int restrict_uncached(int s, const std::vector<int>& which, size_t at) {
    const long q = last_writer_before(s, at);
    if (q < 0) {
      if (const auto* values = data_before(s, at)) {
        std::vector<double> picked;
        picked.reserve(which.size());
        for (int k : which) picked.push_back((*values)[(size_t)k]);
        return data_slot(std::move(picked));
      }
      return gather(s, which);
    }
    const Op w = g.ops[(size_t)q];  // a copy: emitting grows the graph
    if (w.out != s) return fallback(s, which);
    // An input the writer read must still hold that value at p.
    const auto holds = [&](int in) {
      return in != s && !written_in(in, (size_t)q + 1, p);
    };
    switch (classify(g, w)) {
      case Shape::kElementwise: {
        for (int j = 0; j < w.n_in; ++j)
          if (!holds(w.in[j])) return fallback(s, which);
        Op op = w;
        op.primal_source = -1;
        for (int j = 0; j < w.n_in; ++j)
          op.in[j] = restricted(w.in[j], which, p);
        op.out = graph.add_slot((int64_t)which.size(), false);
        before[p].push_back(op);
        return op.out;
      }
      case Shape::kGather: {
        if (!holds(w.in[0])) return fallback(s, which);
        std::vector<int> through;
        through.reserve(which.size());
        for (int k : which) through.push_back(w.idata[k]);
        return restricted(w.in[0], through, p);
      }
      case Shape::kSliceRead: {
        if (!holds(w.in[0])) return fallback(s, which);
        const int64_t start = w.idata[0], stride = slice_stride(w);
        std::vector<int> through;
        through.reserve(which.size());
        for (int k : which) through.push_back((int)(start + k * stride));
        return restricted(w.in[0], through, p);
      }
      case Shape::kMatvec: {
        const int64_t rows = w.idata[0], cols = w.idata[1];
        const auto* x = data_before(w.in[0], p);
        if (x == nullptr || !writers[(size_t)w.in[0]].empty() ||
            !holds(w.in[1]))
          return fallback(s, which);
        const int64_t kept = (int64_t)which.size();
        std::vector<double> picked((size_t)(kept * cols));
        for (int64_t c = 0; c < cols; ++c)
          for (int64_t r = 0; r < kept; ++r)
            picked[(size_t)(c * kept + r)] =
                (*x)[(size_t)(c * rows + which[(size_t)r])];
        Op op = w;
        op.primal_source = -1;
        op.in[0] = data_slot(std::move(picked));
        op.out = graph.add_slot(kept, false);
        attach(op, {(int)kept, (int)cols});
        before[p].push_back(op);
        return op.out;
      }
      case Shape::kStore:
      case Shape::kSliceWrite: {
        std::vector<Source> from;
        from.reserve(which.size());
        bool scalars = true, one_slot = true;
        for (int e : which) {
          const Source src = source_of(s, e, at);
          if (src.kind == Source::kNone) return fallback(s, which);
          scalars &= src.kind == Source::kScalar;
          one_slot &= src.kind == Source::kElement &&
                      (from.empty() ||
                       (src.slot == from[0].slot && src.at == from[0].at));
          from.push_back(src);
        }
        if (one_slot && !from.empty() &&
            !(from[0].slot == s && from[0].at == at)) {
          std::vector<int> through;
          through.reserve(from.size());
          for (const Source& src : from) through.push_back(src.index);
          return restricted(from[0].slot, through, from[0].at);
        }
        if (scalars && !from.empty()) {
          // Pack the scalars as the lowering does: one copying store from
          // a filled base, then in-place stores into its result.
          const int64_t kept = (int64_t)from.size();
          const int base = data_slot(std::vector<double>((size_t)kept, 0.0));
          const int packed = graph.add_slot(kept, false);
          for (int64_t k = 0; k < kept; ++k) {
            Op op;
            op.opcode = k == 0 ? OP_SET_INDEX : OP_SET_INDEX_INPLACE;
            op.n_in = 2;
            op.in[0] = k == 0 ? base : packed;
            op.in[1] = from[(size_t)k].slot;
            op.out = packed;
            attach(op, {(int)k});
            before[p].push_back(op);
          }
          return packed;
        }
        return fallback(s, which);
      }
      default:
        return fallback(s, which);
    }
  }
};

}  // namespace

CollapseReport analyze_collapse(const Graph& g, const Fills& fills,
                                const std::vector<int>& target_terms) {
  return analyze(g, fills, target_terms).report;
}

CollapseStats collapse_observations(Graph& g, Fills& fills,
                                    std::vector<int>& target_terms,
                                    const std::vector<int>& extra_roots,
                                    CollapseReport* report) {
  CollapseStats st;
  if (std::getenv("STANLI_NO_COLLAPSE")) return st;
  Analysis a = analyze(g, fills, target_terms);
  if (a.vectors.empty() && a.statistics.empty() && a.scalars_merged == 0) {
    if (report != nullptr) *report = std::move(a.report);
    return st;
  }

  const size_t n_old = g.ops.size();
  Rewriter rw(g, fills);
  std::vector<char> replaced(n_old, 0);
  // Where an argument could only be gathered from its full-length vector,
  // the ops behind it still run in full and only the density shrinks: that
  // is taken back unless the rows are few enough to pay on their own.
  const auto unpaid = [&](size_t term, int64_t rows, int64_t n) {
    if (rw.fallbacks == 0 || kCollapseUnrestrictedRatio * rows <= n)
      return false;
    a.report.terms[term].refusal = "its arguments could not be restricted";
    a.report.terms[term].evaluator = nullptr;
    return true;
  };
  for (const VectorPlan& plan : a.vectors) {
    rw.p = plan.op;
    rw.memo.clear();
    rw.fallbacks = 0;
    const Rewriter::Mark mark = rw.mark();
    const Op d = g.ops[plan.op];
    const Outcomes outcomes = outcomes_of(d);
    const int64_t rows = (int64_t)plan.rep.size();

    Op e = d;
    e.primal_source = -1;
    // An unset mask means every argument is active; the elementwise form
    // reads the mask as written.
    if (e.variant == 0) e.variant = (uint8_t)((1u << d.n_in) - 1u);
    e.variant = (uint8_t)(e.variant | 0x40u);
    int64_t n = outcomes.width;
    for (int j = 0; j < d.n_in; ++j) {
      n = std::max(n, g.slots[(size_t)d.in[j]].len);
      e.in[j] = rw.restricted(d.in[j], plan.rep, plan.op);
    }
    if (!outcomes.groups.empty()) {
      std::vector<int> idata;
      for (const auto& group : outcomes.groups) {
        if (outcomes.grouped)
          idata.push_back(group.second == 1 ? -1 : (int)rows);
        if (group.second == 1)
          idata.push_back(group.first[0]);
        else
          for (int obs : plan.rep) idata.push_back(group.first[obs]);
      }
      rw.attach(e, std::move(idata));
    }
    if (unpaid(plan.term, rows, n)) {
      rw.rollback(mark);
      continue;
    }
    e.out = g.add_slot(rows, false);
    rw.before[plan.op].push_back(e);

    Op dot;
    dot.opcode = OP_DOT;
    dot.n_in = 2;
    dot.in[0] = e.out;
    dot.in[1] =
        rw.data_slot(std::vector<double>(plan.count.begin(), plan.count.end()));
    dot.out = d.out;
    rw.before[plan.op].push_back(dot);
    replaced[plan.op] = 1;
    ++st.vector_terms;
    st.observations += n;
    st.rows += rows;
  }

  for (const StatisticPlan& plan : a.statistics) {
    rw.p = plan.op;
    rw.memo.clear();
    rw.fallbacks = 0;
    const Rewriter::Mark mark = rw.mark();
    const Op d = g.ops[plan.op];
    Op e;
    e.variant = plan.variant;
    e.n_in = 4;
    if (plan.linear.empty()) {
      e.opcode = OP_NORMAL_GROUPED_LPDF;
      e.in[1] = rw.restricted(d.in[1], plan.rep, plan.op);
      e.in[2] = rw.restricted(plan.scale, plan.rep, plan.op);
      if (unpaid(plan.term, (int64_t)plan.rep.size(),
                 g.slots[(size_t)d.in[0]].len)) {
        rw.rollback(mark);
        continue;
      }
      e.in[0] = rw.data_slot(plan.statistics);
      ++st.statistic_terms;
      st.rows += (int64_t)plan.rep.size();
    } else {
      e.opcode = OP_LINEAR_GAUSSIAN_LPDF;
      e.in[1] = rw.packed(plan.leaves);
      e.in[2] = plan.scale;
      e.in[0] = rw.data_slot(plan.linear);
      ++st.linear_terms;
      st.rows += (int64_t)plan.leaves.size();
    }
    // Stan drops the variate's own term under propto; otherwise it is a
    // constant of the data.
    e.in[3] = rw.data_slot({plan.propto ? 0.0 : plan.variate_constant});
    e.out = d.out;
    rw.before[plan.op].push_back(e);
    replaced[plan.op] = 1;
    st.observations += g.slots[(size_t)d.in[0]].len;
  }

  std::vector<Op> ops;
  ops.reserve(n_old + 4 * a.vectors.size());
  for (size_t i = 0; i < n_old; ++i) {
    ops.insert(ops.end(), rw.before[i].begin(), rw.before[i].end());
    if (!replaced[i]) ops.push_back(g.ops[i]);
  }

  if (a.scalars_merged > 0) {
    // In place: the first term of a merged value becomes that term times its
    // count, and the repeats are dropped. No slot may be listed twice: the
    // passes that follow take the target terms to be distinct.
    std::unordered_map<int, int> weight_slot;
    std::vector<int> terms;
    std::vector<char> emitted(a.scalars.size(), 0);
    size_t next = 0;
    for (int t : target_terms) {
      if (t < 0 || g.slots[(size_t)t].len != 1) {
        terms.push_back(t);
        continue;
      }
      const size_t group = (size_t)a.scalar_group[next++];
      const int count = a.scalars[group].second;
      if (!a.scalar_merges[group]) {
        terms.push_back(t);
        continue;
      }
      if (emitted[group]) continue;
      emitted[group] = 1;
      auto w = weight_slot.find(count);
      if (w == weight_slot.end())
        w = weight_slot.emplace(count, rw.data_slot({(double)count})).first;
      Op mul;
      mul.opcode = OP_MUL;
      mul.n_in = 2;
      mul.in[0] = a.scalars[group].first;
      mul.in[1] = w->second;
      mul.out = g.add_slot(1, false);
      ops.push_back(mul);
      terms.push_back(mul.out);
    }
    target_terms = std::move(terms);
    st.scalar_terms_merged = a.scalars_merged;
  }

  // Remove what nothing reads any more: only ops of the shapes this pass
  // models, which are pure and write all of their output. A slot is live
  // whole, or in the elements an index read asked for: a loop that updates
  // a vector element by element reads each one back, and only the stores
  // behind the elements still wanted have to stay.
  std::vector<char> live(g.slots.size(), 0);
  std::unordered_map<int, std::unordered_set<int>> wanted;
  for (int s : extra_roots)
    if (s >= 0) live[(size_t)s] = 1;
  for (int s : target_terms)
    if (s >= 0) live[(size_t)s] = 1;
  if (g.result_slot >= 0) live[(size_t)g.result_slot] = 1;
  const auto want_all = [&](int s) {
    live[(size_t)s] = 1;
    wanted.erase(s);
  };
  std::vector<char> drop(ops.size(), 0);
  for (size_t i = ops.size(); i-- > 0;) {
    const Op& op = ops[i];
    const Shape shape = classify(g, op);
    const bool modelled = shape != Shape::kFresh;
    const auto some = modelled ? wanted.find(op.out) : wanted.end();
    const bool partly = some != wanted.end() && !some->second.empty();
    if (modelled && !live[(size_t)op.out] && !partly) {
      drop[i] = 1;
      ++st.ops_removed;
      continue;
    }
    if (shape == Shape::kStore && !live[(size_t)op.out]) {
      // Only some elements are wanted, and this store writes one.
      const int at = op.idata[0];
      if (op.in[0] == op.out) {
        if (some->second.erase(at) == 0) {
          drop[i] = 1;
          ++st.ops_removed;
          continue;
        }
      } else {
        // A copying store has to run for the elements it passes on, which
        // are then wanted of its base.
        std::unordered_set<int> rest = std::move(some->second);
        wanted.erase(some);
        rest.erase(at);
        if (!live[(size_t)op.in[0]])
          wanted[op.in[0]].insert(rest.begin(), rest.end());
      }
      want_all(op.in[1]);
      continue;
    }
    if (modelled) {
      live[(size_t)op.out] = 0;
      wanted.erase(op.out);
    }
    if (shape == Shape::kIndex) {
      if (!live[(size_t)op.in[0]]) wanted[op.in[0]].insert(op.idata[0]);
      continue;
    }
    for (int j = 0; j < op.n_in; ++j)
      if (op.in[j] >= 0) want_all(op.in[j]);
  }
  std::vector<Op> kept;
  kept.reserve(ops.size() - (size_t)st.ops_removed);
  for (size_t i = 0; i < ops.size(); ++i)
    if (!drop[i]) kept.push_back(ops[i]);
  g.ops = std::move(kept);

  // A slot nothing names any more holds no storage.
  std::vector<char> named(g.slots.size(), 0);
  for (const Op& op : g.ops) {
    for (int j = 0; j < op.n_in; ++j)
      if (op.in[j] >= 0) named[(size_t)op.in[j]] = 1;
    if (op.out >= 0) named[(size_t)op.out] = 1;
    if (op.out2 >= 0) named[(size_t)op.out2] = 1;
  }
  for (int s : extra_roots)
    if (s >= 0) named[(size_t)s] = 1;
  for (int s : target_terms)
    if (s >= 0) named[(size_t)s] = 1;
  if (g.result_slot >= 0) named[(size_t)g.result_slot] = 1;
  for (const auto& f : fills) named[(size_t)f.first] = 1;
  for (size_t s = 0; s < g.slots.size(); ++s)
    if (!named[s] && !g.slots[s].is_param) g.slots[s].len = 0;
  if (report != nullptr) *report = std::move(a.report);
  return st;
}

void print_collapse_report(const CollapseReport& report, const char* graph) {
  for (const CollapseTerm& t : report.terms) {
    const char* name = opcode_name(t.opcode);
    std::fprintf(stderr,
                 "COLLAPSE {\"graph\":\"%s\",\"op\":%d,\"opcode\":\"%s\","
                 "\"ops\":%lld,\"n\":%lld,\"rows\":%lld,\"groups\":%lld,"
                 "\"variate_is_data\":%s,\"refusal\":\"%s\","
                 "\"evaluator\":\"%s\"}\n",
                 graph, t.op, name, (long long)t.ops, (long long)t.n,
                 (long long)t.rows, (long long)t.groups,
                 t.variate_is_data ? "true" : "false",
                 t.refusal ? t.refusal : "", t.evaluator ? t.evaluator : "");
  }
  for (const auto& o : report.opaque_ops)
    std::fprintf(stderr,
                 "COLLAPSE {\"graph\":\"%s\",\"opaque\":\"%s\","
                 "\"count\":%lld}\n",
                 graph, opcode_name(o.first), (long long)o.second);
}

}  // namespace stanli
