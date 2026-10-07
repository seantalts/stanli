// Observation collapse, analysis half. See stanli/collapse.hpp.
//
// Every element that can reach a density argument gets a value number.
// Numbers are assigned by exact table lookup (opcode, variant, operand
// numbers or data bit patterns), never by hash alone: a collision must not
// be able to put two different observations in one group.
//
// Slots are mutable buffers, so the numbers live per slot element and are
// overwritten as the single forward pass reaches each write. Only slots in
// the backward cone of a candidate term are numbered; a data matrix is read
// row by row from its fill and never numbered element by element.
#include <stanli/collapse.hpp>
#include <stanli/optable.hpp>

#include "pass_util.hpp"

#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace stanli {
namespace {

using Vn = int64_t;

// Tags for table keys that are not an (opcode, variant) pair.
constexpr int64_t kDataTag = -1;
constexpr int64_t kSlotTag = -2;
constexpr int64_t kMatvecTag = -3;

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

// out[i] = f(in0[i or 0], in1[i or 0], ...) and nothing else.
bool is_elementwise(const Op& op) {
  return (has_op_trait(op.opcode, op_trait::kRerollWidenable) ||
          op.opcode == OP_BCAST_FMA) &&
         op.n_in >= 1 && op.n_in <= 3 && op.n_idata == 0 &&
         op.udata == nullptr && op.out2 < 0 && !op.dyn_lengths;
}

bool is_store(const Op& op) {
  return (op.opcode == OP_SET_INDEX || op.opcode == OP_SET_INDEX_INPLACE) &&
         op.n_in == 2 && op.n_idata == 1 && !op.dyn_lengths;
}

// out[i] = in[start + i * stride]; 0 if `op` is not such a read.
int64_t slice_read_stride(const Op& op) {
  if (op.n_in != 1 || op.udata != nullptr || op.dyn_lengths) return 0;
  if (op.opcode == OP_SLICE && op.n_idata == 1) return 1;
  if (op.opcode == OP_SLICE_STRIDED && op.n_idata == 2) return op.idata[1];
  return 0;
}

// out = in0 with out[start + i * stride] = in1[i]; 0 if not such a write.
int64_t slice_write_stride(const Op& op) {
  if (op.n_in != 2 || op.udata != nullptr || op.dyn_lengths) return 0;
  if ((op.opcode == OP_SET_SLICE || op.opcode == OP_SET_SLICE_INPLACE) &&
      op.n_idata == 1)
    return 1;
  if ((op.opcode == OP_SET_SLICE_STRIDED ||
       op.opcode == OP_SET_SLICE_STRIDED_INPLACE) &&
      op.n_idata == 2)
    return op.idata[1];
  return 0;
}

// Ops whose output elements are a function of input elements this pass
// follows, so their inputs join the cone.
bool is_transparent(const Op& op) {
  if (op.out < 0) return false;
  if (is_elementwise(op) || is_store(op)) return true;
  if (slice_read_stride(op) > 0 || slice_write_stride(op) > 0) return true;
  if (op.opcode == OP_GATHER || op.opcode == OP_INDEX)
    return op.n_in == 1 && op.udata == nullptr && !op.dyn_lengths;
  return false;
}

// The outcome layout of an integer density: `width` observations, each
// reading one value per group. Flat immediates are one group; binomial-style
// immediates are [len, vals...] groups with len == -1 marking a scalar.
struct Outcomes {
  bool ok = true;
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

struct Numbering {
  const Graph& g;
  std::vector<std::vector<Vn>> vn;  // per cone slot, per element
  std::vector<char> in_cone;
  std::vector<int64_t> version;
  std::unordered_map<Key, Vn, KeyHash> table;
  std::unordered_set<Vn> nan_data;
  Key key;
  Vn next = 1;

  explicit Numbering(const Graph& graph)
      : g(graph),
        vn(graph.slots.size()),
        in_cone(graph.slots.size(), 0),
        version(graph.slots.size(), 0) {}

  Vn fresh() { return next++; }

  Vn intern() {
    const auto ins = table.emplace(key, next);
    if (ins.second) ++next;
    return ins.first->second;
  }

  Vn data(double x) {
    int64_t bits;
    std::memcpy(&bits, &x, sizeof bits);
    key.w.assign({kDataTag, bits});
    const Vn v = intern();
    if (x != x) nan_data.insert(v);
    return v;
  }

  // A whole slot as it stands now, for arguments that are not per
  // observation (a shared cutpoint vector).
  Vn whole_slot(int s) {
    key.w.assign({kSlotTag, s, version[(size_t)s]});
    return intern();
  }

  Vn elem(int s, int64_t i) const {
    const auto& v = vn[(size_t)s];
    return v[v.size() == 1 ? 0 : (size_t)i];
  }

  void fresh_slot(int s) {
    for (Vn& v : vn[(size_t)s]) v = fresh();
  }
};

}  // namespace

CollapseReport analyze_collapse(
    const Graph& g,
    const std::vector<std::pair<int, std::vector<double>>>& fills,
    const std::vector<int>& target_terms) {
  CollapseReport report;
  const size_t n_slots = g.slots.size();
  const size_t n_ops = g.ops.size();

  std::unordered_set<int> targets(target_terms.begin(), target_terms.end());
  std::vector<int> readers(n_slots, 0);
  std::vector<char> active(n_slots, 0);
  for (size_t s = 0; s < n_slots; ++s) active[s] = g.slots[s].is_param;
  std::vector<char> op_active(n_ops, 0);
  for (size_t i = 0; i < n_ops; ++i) {
    const Op& op = g.ops[i];
    if (op.udata != nullptr) ++report.opaque_ops[op.opcode];
    bool any = false;
    for (int j = 0; j < op.n_in; ++j) {
      if (op.in[j] < 0) continue;
      ++readers[(size_t)op.in[j]];
      any |= active[(size_t)op.in[j]] != 0;
    }
    op_active[i] = any;
    if (op.out >= 0) active[(size_t)op.out] |= any;
    if (op.out2 >= 0) active[(size_t)op.out2] |= any;
  }

  // Candidates: densities with elementwise arguments whose inputs depend on
  // a parameter. A density of data alone is a constant, not a likelihood.
  std::vector<char> candidate(n_ops, 0);
  Numbering num(g);
  bool any_candidate = false;
  for (size_t i = 0; i < n_ops; ++i) {
    const Op& op = g.ops[i];
    if (!is_density(op.opcode) || !op_active[i] || op.out < 0 ||
        op.udata != nullptr)
      continue;
    candidate[i] = 1;
    any_candidate = true;
    for (int j = 0; j < op.n_in; ++j)
      if (op.in[j] >= 0) num.in_cone[(size_t)op.in[j]] = 1;
  }
  if (!any_candidate) return report;

  // Ops are in evaluation order, so one reverse sweep closes the cone.
  for (size_t i = n_ops; i-- > 0;) {
    const Op& op = g.ops[i];
    if (!is_transparent(op) || !num.in_cone[(size_t)op.out]) continue;
    for (int j = 0; j < op.n_in; ++j)
      if (op.in[j] >= 0) num.in_cone[(size_t)op.in[j]] = 1;
  }

  std::unordered_map<int, const std::vector<double>*> fill_of;
  for (const auto& f : fills) fill_of.emplace(f.first, &f.second);
  for (size_t s = 0; s < n_slots; ++s) {
    if (!num.in_cone[s]) continue;
    auto& v = num.vn[s];
    v.resize((size_t)g.slots[s].len);
    const auto f = fill_of.find((int)s);
    if (!g.slots[s].is_param && f != fill_of.end() &&
        f->second->size() == v.size()) {
      for (size_t k = 0; k < v.size(); ++k) v[k] = num.data((*f->second)[k]);
    } else {
      num.fresh_slot((int)s);
    }
  }

  // Scalar families, keyed on what must match for two ops to be one term.
  std::unordered_map<Key, size_t, KeyHash> family_of;
  struct Family {
    size_t term;
    std::unordered_set<Vn> rows;
    bool refused = false;
  };
  std::vector<Family> families;
  Key row;

  const auto refuse = [](CollapseTerm& t, const char* why) {
    if (t.refusal == nullptr) t.refusal = why;
  };

  for (size_t i = 0; i < n_ops; ++i) {
    const Op& op = g.ops[i];

    if (candidate[i]) {
      const Outcomes outcomes = outcomes_of(op);
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
      const bool is_target = g.slots[(size_t)op.out].len == 1 &&
                             targets.count(op.out) != 0 &&
                             readers[(size_t)op.out] == 0;
      const bool real_variate = outcomes.groups.empty();
      const bool variate_is_data =
          !real_variate || (op.n_in > 0 && !active[(size_t)op.in[0]]);

      // One row key per observation: outcomes, then each argument.
      const auto row_key = [&](int64_t obs, bool with_variate) {
        row.w.clear();
        if (with_variate)
          for (size_t q = 0; q < outcomes.groups.size(); ++q)
            row.w.push_back(outcomes.at(q, obs));
        for (int j = (real_variate && !with_variate) ? 1 : 0; j < op.n_in; ++j)
          row.w.push_back(num.elem(op.in[j], obs));
      };

      if (shaped && n == 1) {
        // A scalar density: one observation of a family.
        row.w.assign(
            {op.opcode, op.variant, op.n_in, (int64_t)outcomes.groups.size()});
        const auto ins = family_of.emplace(row, families.size());
        if (ins.second) {
          CollapseTerm t;
          t.op = (int)i;
          t.opcode = op.opcode;
          t.variate_is_data = true;
          families.push_back(Family{report.terms.size(), {}, false});
          report.terms.push_back(t);
        }
        Family& fam = families[ins.first->second];
        CollapseTerm& t = report.terms[fam.term];
        ++t.ops;
        ++t.n;
        t.variate_is_data &= variate_is_data;
        if (!is_target) {
          fam.refused = true;
          refuse(t, "a term's value is read by another op");
        }
        row_key(0, true);
        if (real_variate && num.nan_data.count(num.elem(op.in[0], 0))) {
          fam.refused = true;
          refuse(t, "NaN in the variate");
        }
        num.key.w = row.w;
        num.key.w.push_back(kSlotTag - 1);  // a row key, not a node key
        fam.rows.insert(num.intern());
      } else {
        CollapseTerm t;
        t.op = (int)i;
        t.opcode = op.opcode;
        t.ops = 1;
        t.n = n;
        t.variate_is_data = variate_is_data;
        if (!shaped) {
          refuse(t, "argument shapes are not per observation");
        } else {
          std::unordered_set<Key, KeyHash> seen;
          bool nan = false;
          for (int64_t obs = 0; obs < n; ++obs) {
            row_key(obs, true);
            seen.insert(row);
            if (real_variate)
              nan |= num.nan_data.count(num.elem(op.in[0], obs)) != 0;
          }
          t.rows = (int64_t)seen.size();
          if (has_statistic(op.opcode) && variate_is_data) {
            seen.clear();
            for (int64_t obs = 0; obs < n; ++obs) {
              row_key(obs, false);
              seen.insert(row);
            }
            t.groups = (int64_t)seen.size();
          }
          if (!is_target)
            refuse(t, g.slots[(size_t)op.out].len != 1
                          ? "elementwise output"
                          : "the term's value is read by another op");
          if (nan) refuse(t, "NaN in the variate");
          if (n < kCollapseMinObservations) refuse(t, "too few observations");
          const int64_t best = t.groups >= 0 ? t.groups : t.rows;
          if (2 * best > n) refuse(t, "too few repeated rows");
        }
        report.terms.push_back(t);
      }
    }

    // Number what this op writes.
    if (op.out >= 0 && num.in_cone[(size_t)op.out]) {
      const int out = op.out;
      auto& vout = num.vn[(size_t)out];
      const int64_t len = (int64_t)vout.size();
      bool done = false;
      if (is_elementwise(op)) {
        bool fits = true;
        for (int j = 0; j < op.n_in; ++j) {
          const int64_t l = op.in[j] < 0 ? -1 : g.slots[(size_t)op.in[j]].len;
          fits &= l == 1 || l == len;
        }
        if (fits) {
          std::vector<Vn> result((size_t)len);
          for (int64_t k = 0; k < len; ++k) {
            num.key.w.assign({op.opcode, op.variant});
            for (int j = 0; j < op.n_in; ++j)
              num.key.w.push_back(num.elem(op.in[j], k));
            result[(size_t)k] = num.intern();
          }
          vout = std::move(result);
          done = true;
        }
      } else if (op.opcode == OP_GATHER && is_transparent(op) &&
                 op.n_idata == len) {
        const auto& vin = num.vn[(size_t)op.in[0]];
        std::vector<Vn> result((size_t)len);
        done = true;
        for (int64_t k = 0; k < len && done; ++k) {
          const int64_t at = op.idata[k];
          if (at < 0 || at >= (int64_t)vin.size())
            done = false;
          else
            result[(size_t)k] = vin[(size_t)at];
        }
        if (done) vout = std::move(result);
      } else if (op.opcode == OP_INDEX && is_transparent(op) &&
                 op.n_idata == 1 && len == 1) {
        const auto& vin = num.vn[(size_t)op.in[0]];
        const int64_t at = op.idata[0];
        if (at >= 0 && at < (int64_t)vin.size()) {
          vout[0] = vin[(size_t)at];
          done = true;
        }
      } else if (is_store(op)) {
        const int64_t at = op.idata[0];
        if (g.slots[(size_t)op.in[0]].len == len &&
            g.slots[(size_t)op.in[1]].len == 1 && at >= 0 && at < len) {
          std::vector<Vn> result = num.vn[(size_t)op.in[0]];
          result[(size_t)at] = num.vn[(size_t)op.in[1]][0];
          vout = std::move(result);
          done = true;
        }
      } else if (const int64_t stride = slice_read_stride(op); stride > 0) {
        const auto& vin = num.vn[(size_t)op.in[0]];
        const int64_t start = op.idata[0];
        if (start >= 0 &&
            (len == 0 || start + (len - 1) * stride < (int64_t)vin.size())) {
          std::vector<Vn> result((size_t)len);
          for (int64_t k = 0; k < len; ++k)
            result[(size_t)k] = vin[(size_t)(start + k * stride)];
          vout = std::move(result);
          done = true;
        }
      } else if (const int64_t stride = slice_write_stride(op); stride > 0) {
        const auto& src = num.vn[(size_t)op.in[1]];
        const int64_t start = op.idata[0], count = (int64_t)src.size();
        if (g.slots[(size_t)op.in[0]].len == len && start >= 0 &&
            (count == 0 || start + (count - 1) * stride < len)) {
          std::vector<Vn> result = num.vn[(size_t)op.in[0]];
          for (int64_t k = 0; k < count; ++k)
            result[(size_t)(start + k * stride)] = src[(size_t)k];
          vout = std::move(result);
          done = true;
        }
      } else if (op.opcode == OP_MATVEC && op.n_in == 2 && op.n_idata == 2 &&
                 op.udata == nullptr && !op.dyn_lengths) {
        // Rows of a data matrix times one vector: two rows agree when their
        // data agree bitwise. The vector is taken whole.
        const int64_t rows = op.idata[0], cols = op.idata[1];
        const auto x = fill_of.find(op.in[0]);
        if (rows == len && !active[(size_t)op.in[0]] && x != fill_of.end() &&
            (int64_t)x->second->size() == rows * cols) {
          const Vn beta = num.whole_slot(op.in[1]);
          for (int64_t r = 0; r < rows; ++r) {
            num.key.w.assign({kMatvecTag, beta});
            for (int64_t c = 0; c < cols; ++c) {
              int64_t bits;
              std::memcpy(&bits, &(*x->second)[(size_t)(c * rows + r)],
                          sizeof bits);
              num.key.w.push_back(bits);
            }
            vout[(size_t)r] = num.intern();
          }
          done = true;
        }
      }
      if (!done) num.fresh_slot(out);
    }
    if (op.out2 >= 0 && num.in_cone[(size_t)op.out2]) num.fresh_slot(op.out2);
    if (op.out >= 0) ++num.version[(size_t)op.out];
    if (op.out2 >= 0) ++num.version[(size_t)op.out2];
  }

  for (const Family& fam : families) {
    CollapseTerm& t = report.terms[fam.term];
    t.rows = (int64_t)fam.rows.size();
    if (t.n < kCollapseMinObservations) refuse(t, "too few observations");
    if (2 * t.rows > t.n) refuse(t, "too few repeated rows");
  }
  return report;
}

void print_collapse_report(const CollapseReport& report, const char* graph) {
  for (const CollapseTerm& t : report.terms) {
    const char* name = opcode_name(t.opcode);
    std::fprintf(stderr,
                 "COLLAPSE {\"graph\":\"%s\",\"op\":%d,\"opcode\":\"%s\","
                 "\"ops\":%lld,\"n\":%lld,\"rows\":%lld,\"groups\":%lld,"
                 "\"variate_is_data\":%s,\"refusal\":\"%s\"}\n",
                 graph, t.op, name, (long long)t.ops, (long long)t.n,
                 (long long)t.rows, (long long)t.groups,
                 t.variate_is_data ? "true" : "false",
                 t.refusal ? t.refusal : "");
  }
  for (const auto& o : report.opaque_ops)
    std::fprintf(stderr,
                 "COLLAPSE {\"graph\":\"%s\",\"opaque\":\"%s\","
                 "\"count\":%lld}\n",
                 graph, opcode_name(o.first), (long long)o.second);
}

}  // namespace stanli
