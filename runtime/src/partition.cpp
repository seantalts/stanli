// Lane bucketing.
//
// Re-roll asks "does this template repeat with period P". That question
// cannot see a loop whose lanes are interleaved with anything else, and its
// period scan can arrive at a run mis-phased and never find the alignment
// (state_space_stochastic_level_stochastic_seasonal: one perfect four-op
// random walk that re-roll leaves entirely scalar because the window-sum loop
// before it left the scan off by two). This pass asks a different question:
// where does a lane END?
//
// A lane ends at a target term or at an element store, and reaches back as
// far as the ops whose values never leave it. Lane bounds are therefore
// computed, not discovered, and lanes need not be adjacent: they are grouped
// by a structural fingerprint (opcodes, shapes, dataflow edges, immediate
// SHAPES -- never which slot a lane reads, which is what lets two branches of
// a data condition share a bucket), and a bucket is rewritten in place of its
// first lane.
//
// What a bucket's ops read is proven, not assumed: no slot the fused ops read
// from outside may be written while the run is in flight. That one rule
// carries three of the soundness obligations at once -- no cross-lane
// recurrence, no mid-bucket writer, and no destructive in-place store between
// the lanes -- because all three appear as a writer between the first lane's
// position and the last lane's end. Ops only ever move EARLIER, so an
// in-place store proven safe against a later reader stays safe, and a bucket
// that trips the rule splits at the writer rather than declining.
#include <stanli/optable.hpp>
#include <stanli/partition.hpp>

#include <algorithm>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace stanli {
namespace {

constexpr int64_t kMinLanes = 4;
// island.cpp's currencies: ~5 ns per graph op against ~1 ns per element
// moved. A bucket must beat its gathers by more than a rounding error, or it
// churns the graph for nothing.
constexpr int64_t kOpCost = 5;
constexpr int64_t kPartitionMargin = 8 * kOpCost;
// A density element costs about six op dispatches to evaluate. It is the
// term that decides whether re-evaluating a lane the CSE pass would have
// collapsed, or trading a vector density call for W elementwise ones, pays.
constexpr int64_t kDensityElem = 6;
// Splitting is bounded work, not a retry loop: each one costs a re-pass over
// a bucket, and a graph that presents thousands of interleaved writers must
// not turn that into a quadratic term.
constexpr int64_t kMaxSplits = 32;

using Fills = std::vector<std::pair<int, std::vector<double>>>;

bool is_element_store(const Op& op) {
  return (op.opcode == OP_SET_INDEX || op.opcode == OP_SET_INDEX_INPLACE) &&
         op.n_in == 2 && op.n_idata == 1 && op.out == op.in[0];
}

bool is_blocked(const Op& op) {
  return is_effectful_op(op.opcode) || op.opcode == OP_PROD_VEC ||
         op.opcode == OP_EXTREMA_VEC || op.out2 >= 0 || op.udata != nullptr;
}

// Opcodes whose immediates are positions rather than values: two lanes
// reading different elements are the same computation over different data,
// which is exactly what a bucket resolves per input. Everything else keys on
// the values, as ops_match does.
bool idata_is_shape(uint16_t opcode) {
  return opcode == OP_INDEX || opcode == OP_SET_INDEX ||
         opcode == OP_SET_INDEX_INPLACE || opcode == OP_SLICE ||
         opcode == OP_SLICE_STRIDED ||
         has_op_trait(opcode, op_trait::kRerollIdataDensity);
}

// densities_lpmf.cpp's with_int_group: these carry their outcomes as two
// [len, vals...] groups, len == -1 marking a language-level scalar.
// Everything else with kRerollIdataDensity carries one flat outcome array.
bool has_int_groups(uint16_t opcode) {
  return opcode == OP_BINOMIAL_LPMF || opcode == OP_BINOMIAL_LOGIT_LPMF ||
         opcode == OP_BETA_BINOMIAL_LPMF;
}

int64_t group_len(const int* p) { return p[0] == -1 ? 2 : 1 + p[0]; }
int group_elem(const int* p, int64_t e) { return p[0] == -1 ? p[1] : p[1 + e]; }

// These kernels have an elementwise form that costs per element what their
// summed one does (densities_lpmf.cpp), so widening a lane costs them
// nothing. Every other density trades one vectorized call for W recorder
// calls.
bool elt_costs_per_element(uint16_t opcode) {
  switch (opcode) {
    case OP_BERNOULLI_LPMF:
    case OP_BERNOULLI_LOGIT_LPMF:
    case OP_BINOMIAL_LPMF:
    case OP_BINOMIAL_LOGIT_LPMF:
      return false;
    default:
      return true;
  }
}

// Outcome elements per lane, or 0 when the immediates are not a layout this
// pass knows how to concatenate. Groups may be scalar or exactly W wide.
int64_t idata_width(const Op& op) {
  if (!has_op_trait(op.opcode, op_trait::kRerollIdataDensity)) return 1;
  if (!has_int_groups(op.opcode)) return op.n_idata;
  int64_t w = 1, m = 0;
  while (m < op.n_idata) {
    const int64_t len = op.idata[m];
    if (len != -1) {
      if (len < 1 || m + 1 + len > op.n_idata) return 0;
      if (w != 1 && w != len) return 0;
      w = len;
    }
    m += group_len(op.idata + m);
  }
  return m == op.n_idata ? w : 0;
}

struct Lane {
  size_t begin = 0;
  size_t end = 0;  // exclusive; end - 1 is the delimiter
};

enum class InKind { kInvariant, kConstLanes, kLaneLocal };

struct PosIn {
  InKind kind = InKind::kInvariant;
  int producer = -1;           // kLaneLocal: position within the lane
  std::vector<double> values;  // kConstLanes: one value per lane
};

enum class Emit {
  kElide,        // reads the whole base in lane order: the base IS the value
  kSlice,        // one contiguous window
  kGather,       // arbitrary per-lane indices
  kShared,       // same value in every lane: emit the op once
  kRowSum,       // per-lane reduction of a width-W value
  kWiden,        // elementwise, one output element per lane per width
  kEltDensity,   // density consumed in-lane: variant bit 6, out[l] is lane l
  kTermDensity,  // every lane's out is a term: one summed density
  kTermWiden,    // every lane's out is a term: widen, then reduce
  kStore,        // the lane ends in an element store: one slice store
};

struct Pos {
  Emit emit = Emit::kShared;
  std::vector<PosIn> ins;
  std::vector<int> idx;  // kSlice/kStore: {start[, stride]}; kGather: indices
  int64_t width = 1;     // output elements per lane; ignored when kShared
  int64_t rows = 1;      // kEltDensity: outcome elements reduced back per lane
  bool shared = false;   // one value stands for every lane
  int out = -1;          // filled during emission
};

struct Key {
  std::vector<int64_t> w;
  bool operator==(const Key& o) const { return w == o.w; }
};

struct KeyHash {
  size_t operator()(const Key& k) const {
    size_t h = 1469598103934665603ull;
    for (int64_t v : k.w) {
      h ^= static_cast<size_t>(v);
      h *= 1099511628211ull;
    }
    return h;
  }
};

}  // namespace

PartitionStats partition_lanes(Graph& g, Fills& fills,
                               std::vector<int>& target_terms,
                               const std::vector<int>& extra_roots) {
  PartitionStats st;
  if (std::getenv("STANLI_NO_PARTITION")) return st;
  const size_t n_ops = g.ops.size();
  if (n_ops < 2 * (size_t)kMinLanes) return st;

  std::unordered_set<int> term_set(target_terms.begin(), target_terms.end());
  std::unordered_set<int> root_set(extra_roots.begin(), extra_roots.end());
  if (g.result_slot >= 0) root_set.insert(g.result_slot);

  // Most graphs have too few lane ends to hold a bucket at all. Answer that
  // in one scan, before allocating anything per slot.
  int64_t delimiters = 0;
  for (const Op& op : g.ops) {
    ++st.segment_steps;
    if ((op.out >= 0 && term_set.count(op.out) != 0) || is_element_store(op))
      ++delimiters;
  }
  if (delimiters < kMinLanes) return st;

  const size_t n_slots = g.slots.size();
  std::vector<std::vector<size_t>> uses(n_slots);
  std::vector<std::vector<size_t>> writers(n_slots);
  for (size_t u = 0; u < n_ops; ++u) {
    const Op& op = g.ops[u];
    for (int j = 0; j < op.n_in; ++j)
      if (op.in[j] >= 0) uses[(size_t)op.in[j]].push_back(u);
    if (op.out >= 0) writers[(size_t)op.out].push_back(u);
    if (op.out2 >= 0) writers[(size_t)op.out2].push_back(u);
  }

  // Both lists are built by walking u upwards, so both are sorted and every
  // question below is a range question. Answering those by scanning is what
  // made re-roll quadratic in time on ldaK5; binary search reads only the
  // entries that can matter, and every entry read is counted so the scaling
  // test can assert on an exact integer.
  const auto first_at_or_after = [&](const std::vector<size_t>& v, size_t x) {
    size_t lo = 0, hi = v.size();
    while (lo < hi) {
      const size_t mid = lo + (hi - lo) / 2;
      ++st.list_steps;
      if (v[mid] < x)
        lo = mid + 1;
      else
        hi = mid;
    }
    return v.begin() + (ptrdiff_t)lo;
  };
  const auto any_at_or_after = [&](const std::vector<size_t>& v, size_t x) {
    return first_at_or_after(v, x) != v.end();
  };
  // The first entry in [lo, hi) that is not one of `mine`, else n_ops.
  const auto first_in_range_but = [&](const std::vector<size_t>& v, size_t lo,
                                      size_t hi,
                                      const std::unordered_set<size_t>& mine) {
    for (auto it = first_at_or_after(v, lo); it != v.end() && *it < hi; ++it) {
      ++st.list_steps;
      if (mine.count(*it) == 0) return *it;
    }
    return n_ops;
  };

  // ---- segmentation ---------------------------------------------------
  std::vector<Lane> lanes;
  size_t run_begin = 0;
  for (size_t u = 0; u < n_ops; ++u) {
    ++st.segment_steps;
    const Op& op = g.ops[u];
    // A slot the executor reads out of the arena, or a second output, ends
    // the run without opening a lane: those values must still be written
    // where they are.
    if (op.out2 >= 0 || (op.out >= 0 && root_set.count(op.out) != 0)) {
      run_begin = u + 1;
      continue;
    }
    const bool term_delim = op.out >= 0 && term_set.count(op.out) != 0 &&
                            uses[(size_t)op.out].empty() &&
                            writers[(size_t)op.out].size() == 1;
    if (!term_delim && !is_element_store(op)) continue;
    // Reach back over the ops whose values never leave the lane. An op that
    // is read later is not part of any lane: it stays where it is, and the
    // lane starts after it.
    size_t begin = u;
    while (begin > run_begin) {
      ++st.segment_steps;
      const Op& prev = g.ops[begin - 1];
      const int o = prev.out;
      if (o < 0 || prev.out2 >= 0 || g.slots[(size_t)o].is_param ||
          term_set.count(o) != 0 || root_set.count(o) != 0 ||
          writers[(size_t)o].size() != 1 ||
          any_at_or_after(uses[(size_t)o], u + 1))
        break;
      --begin;
    }
    lanes.push_back(Lane{begin, u + 1});
    run_begin = u + 1;
  }
  if ((int64_t)lanes.size() < kMinLanes) return st;

  // ---- fingerprint and bucket -----------------------------------------
  std::unordered_map<Key, int, KeyHash> bucket_of;
  std::vector<std::vector<int>> buckets;  // in first-lane order
  Key key;
  std::unordered_map<int, int> pos_of;
  for (size_t li = 0; li < lanes.size(); ++li) {
    const Lane& lane = lanes[li];
    key.w.assign(1, (int64_t)(lane.end - lane.begin));
    pos_of.clear();
    bool ok = true;
    for (size_t u = lane.begin; u < lane.end && ok; ++u) {
      ++st.fingerprint_steps;
      const Op& op = g.ops[u];
      if (is_blocked(op)) {
        ok = false;
        break;
      }
      key.w.push_back(op.opcode);
      key.w.push_back(op.variant);
      key.w.push_back(op.n_in);
      key.w.push_back(g.slots[(size_t)op.out].len);
      key.w.push_back(term_set.count(op.out) != 0);
      for (int j = 0; j < op.n_in; ++j) {
        const int s = op.in[j];
        key.w.push_back(s >= 0 ? g.slots[(size_t)s].len : -1);
        const auto it = pos_of.find(s);
        key.w.push_back(it == pos_of.end() ? -1 : it->second);
      }
      key.w.push_back(op.n_idata);
      if (!idata_is_shape(op.opcode)) {
        for (int64_t m = 0; m < op.n_idata; ++m) key.w.push_back(op.idata[m]);
      } else if (has_int_groups(op.opcode)) {
        // The group widths, not their values: a scalar group and a vector
        // group concatenate differently and must not share a bucket.
        for (int64_t m = 0; m < op.n_idata;) {
          key.w.push_back(op.idata[m]);
          const int64_t step = group_len(op.idata + m);
          if (step < 2) break;
          m += step;
        }
      }
      pos_of[op.out] = (int)(u - lane.begin);
    }
    if (!ok) continue;
    const auto ins = bucket_of.emplace(key, (int)buckets.size());
    if (ins.second) buckets.emplace_back();
    buckets[(size_t)ins.first->second].push_back((int)li);
  }

  // The dedup'd constant pool: slot -> value, for len-1 fills.
  std::unordered_map<int, double> const_val;
  for (const auto& f : fills)
    if (f.second.size() == 1) const_val.emplace(f.first, f.second[0]);

  std::vector<char> dropped(n_ops, 0);
  std::vector<int> emit_at(n_ops, -1);
  std::vector<std::vector<Op>> emitted;

  // A worklist rather than a loop: a bucket whose base is written between two
  // of its lanes splits at that op and both halves are reconsidered, so an
  // interleaved model still gets the lanes on either side. Splits are capped
  // for the reason the per-period arrays in reroll.cpp are.
  std::vector<std::vector<int>> work(buckets.begin(), buckets.end());
  int64_t splits = 0;
  for (size_t bi = 0; bi < work.size(); ++bi) {
    const std::vector<int> ids = work[bi];
    const int64_t L = (int64_t)ids.size();
    if (L < kMinLanes) continue;
    const Lane& lane0 = lanes[(size_t)ids[0]];
    const int k = (int)(lane0.end - lane0.begin);
    const size_t first_begin = lane0.begin;
    const auto op_at = [&](int p, int64_t l) -> const Op& {
      return g.ops[lanes[(size_t)ids[(size_t)l]].begin + (size_t)p];
    };
    // Split at the op that broke the run and requeue the halves. A lane
    // straddling it belongs to neither.
    size_t split_at = n_ops;
    const auto split_here = [&]() {
      if (split_at >= n_ops || splits >= kMaxSplits) return;
      std::vector<int> lo, hi;
      for (int id : ids) {
        if (lanes[(size_t)id].end <= split_at)
          lo.push_back(id);
        else if (lanes[(size_t)id].begin > split_at)
          hi.push_back(id);
      }
      if (lo.size() == ids.size() || hi.size() == ids.size()) return;
      ++splits;
      if ((int64_t)lo.size() >= kMinLanes) work.push_back(std::move(lo));
      if ((int64_t)hi.size() >= kMinLanes) work.push_back(std::move(hi));
    };
    // Every slot the fused ops read from outside the bucket must hold still
    // for the length of the run: a writer between the lanes is a cross-lane
    // recurrence, a mid-bucket store, or a destructive update, and the fused
    // read happens at the first lane's position.
    const size_t last_end = lanes[(size_t)ids[(size_t)(L - 1)]].end;
    const auto settled = [&](int s) {
      if (s < 0 || (size_t)s >= n_slots) return false;
      const auto it = first_at_or_after(writers[(size_t)s], first_begin);
      if (it == writers[(size_t)s].end() || *it >= last_end) return true;
      split_at = std::min(split_at, *it);
      return false;
    };
    // A store-delimited bucket owns its base: the base is the one slot the
    // lanes write, so its proof runs against the whole run instead.
    const bool store_delim = is_element_store(op_at(k - 1, 0));
    std::unordered_set<size_t> delim_at;
    if (store_delim)
      for (int64_t l = 0; l < L; ++l)
        delim_at.insert(lanes[(size_t)ids[(size_t)l]].end - 1);

    pos_of.clear();
    for (int p = 0; p < k; ++p) pos_of[op_at(p, 0).out] = p;

    std::vector<Pos> pos((size_t)k);
    bool ok = true;
    for (int p = 0; p < k && ok; ++p) {
      const Op& t = op_at(p, 0);
      Pos& ap = pos[(size_t)p];
      ap.ins.resize((size_t)t.n_in);
      const bool is_term = term_set.count(t.out) != 0;
      bool all_shared = true;    // this op computes one value for every lane
      int64_t width = 0;         // per-lane elements of the varying inputs
      bool wide_shared = false;  // a shared input the kernels cannot broadcast

      for (int j = 0; j < t.n_in && ok; ++j) {
        PosIn& in = ap.ins[(size_t)j];
        const auto local = pos_of.find(t.in[j]);
        if (local != pos_of.end() && local->second < p) {
          in.kind = InKind::kLaneLocal;
          in.producer = local->second;
          const Pos& prod = pos[(size_t)local->second];
          if (prod.shared) {
            if (g.slots[(size_t)t.in[j]].len != 1) wide_shared = true;
          } else {
            all_shared = false;
            if (width && width != prod.width) ok = false;
            width = prod.width;
          }
          continue;
        }
        bool invariant = true;
        for (int64_t l = 1; l < L && invariant; ++l)
          invariant = op_at(p, l).in[j] == t.in[j];
        if (invariant) {
          in.kind = InKind::kInvariant;
          if (store_delim && p == k - 1 && j == 0) continue;
          if (!settled(t.in[j])) ok = false;
          if (t.in[j] >= 0 && g.slots[(size_t)t.in[j]].len != 1)
            wide_shared = true;
          continue;
        }
        std::vector<double> vals;
        vals.reserve((size_t)L);
        for (int64_t l = 0; l < L; ++l) {
          const int s = op_at(p, l).in[j];
          const auto cit = const_val.find(s);
          if (cit == const_val.end() || !writers[(size_t)s].empty()) break;
          vals.push_back(cit->second);
        }
        if ((int64_t)vals.size() != L) {
          ok = false;  // a different slot per lane that is not a constant
          break;
        }
        in.kind = InKind::kConstLanes;
        in.values = std::move(vals);
        all_shared = false;
        if (width && width != 1) ok = false;
        width = 1;
      }
      if (!ok) break;
      if (width == 0) width = 1;

      bool idata_shared = true;
      for (int64_t l = 1; l < L && idata_shared; ++l)
        for (int64_t m = 0; m < t.n_idata; ++m)
          if (op_at(p, l).idata[m] != t.idata[m]) {
            idata_shared = false;
            break;
          }

      // Indexed reads of a base the whole bucket shares. These are the only
      // positions whose immediates may differ across lanes without the op
      // itself being lane-varying work.
      const bool strided_read = t.opcode == OP_SLICE_STRIDED && t.n_idata == 2;
      const bool indexed_read =
          ((t.opcode == OP_INDEX || t.opcode == OP_SLICE) && t.n_idata == 1) ||
          strided_read;
      if (indexed_read && t.n_in == 1 && !is_term &&
          ap.ins[0].kind == InKind::kInvariant && !idata_shared) {
        const int64_t blen = g.slots[(size_t)t.in[0]].len;
        const int64_t w = g.slots[(size_t)t.out].len;
        const int64_t step = strided_read ? t.idata[1] : 1;
        ap.width = w;
        ap.idx.reserve((size_t)(L * w));
        bool run = step == 1;
        for (int64_t l = 0; l < L; ++l) {
          const Op& o = op_at(p, l);
          const int64_t start = o.idata[0];
          if (start < 0 || step < 1 || start + (w - 1) * step >= blen ||
              (strided_read && o.idata[1] != step)) {
            ok = false;
            break;
          }
          if (start != t.idata[0] + l * w) run = false;
          for (int64_t e = 0; e < w; ++e)
            ap.idx.push_back((int)(start + e * step));
        }
        if (!ok) break;
        if (run && w == 1 && t.idata[0] == 0 && blen == L) {
          ap.emit = Emit::kElide;
        } else if (run && w == 1) {
          ap.emit = Emit::kSlice;
          ap.idx.assign(1, t.idata[0]);
        } else {
          ap.emit = Emit::kGather;
        }
        continue;
      }

      // The lane ends by writing one element of a vector nothing else reads
      // while the run is in flight (Survey_model's lp_parts accumulator).
      // Indices that march by a constant stride become one slice store.
      if (store_delim && p == k - 1) {
        const int vec = t.in[0];
        const int64_t blen = g.slots[(size_t)vec].len;
        const int64_t start = t.idata[0];
        const int64_t step = L >= 2 ? (int64_t)op_at(p, 1).idata[0] - start : 1;
        ok = ap.ins[0].kind == InKind::kInvariant &&
             (ap.ins[1].kind == InKind::kLaneLocal ||
              ap.ins[1].kind == InKind::kConstLanes) &&
             width == 1 && step >= 1 && start >= 0 &&
             start + (L - 1) * step < blen && root_set.count(vec) == 0 &&
             term_set.count(vec) == 0;
        for (int64_t l = 1; l < L && ok; ++l)
          ok = op_at(p, l).idata[0] == start + l * step;
        // The whole run moves to the first lane's position, so nothing may
        // read the vector half-written, and a writer after it would need the
        // tail renaming this pass deliberately does not do.
        if (ok) {
          const size_t reader = first_in_range_but(
              uses[(size_t)vec], first_begin, last_end, delim_at);
          const size_t other = first_in_range_but(
              writers[(size_t)vec], first_begin, last_end, delim_at);
          if (reader < n_ops || other < n_ops) {
            split_at = std::min(split_at, std::min(reader, other));
            ok = false;
          } else if (any_at_or_after(writers[(size_t)vec], last_end)) {
            ok = false;
          }
        }
        if (!ok) break;
        ap.emit = Emit::kStore;
        ap.width = 1;
        ap.idx.assign(1, (int)start);
        if (step != 1) ap.idx.push_back((int)step);
        continue;
      }

      if (all_shared && idata_shared && !is_term) {
        ap.emit = Emit::kShared;
        ap.shared = true;
        continue;
      }
      if (wide_shared) {
        ok = false;  // the kernels broadcast len-1 arguments, nothing wider
        break;
      }
      // L lanes computing the identical scalar, each one a term: the const
      // pool dedup'd even the data argument. One fused op would compute one
      // lane's value where the target owes L of them. An integer-outcome
      // density is the exception that proves it: concatenating the lanes'
      // immediates is itself the widening, so its fused form does compute L.
      if (is_term && all_shared &&
          !has_op_trait(t.opcode, op_trait::kRerollIdataDensity)) {
        ok = false;
        break;
      }
      ap.width = width;

      if (t.opcode == OP_SUM_VEC && t.n_in == 1 && !is_term &&
          ap.ins[0].kind == InKind::kLaneLocal && width >= 2) {
        ap.emit = Emit::kRowSum;
        ap.width = 1;
        continue;
      }
      if (has_op_trait(t.opcode, op_trait::kRerollAnyDensity)) {
        // A width-W outcome group is W lps per lane: the real arguments are
        // W wide too, and a lane that consumes its density rather than
        // ending at it needs them summed back per lane.
        const int64_t dw = idata_width(t);
        const int64_t w = all_shared ? dw : width;
        if (dw < 1 || (dw != 1 && dw != w)) {
          ok = false;
          break;
        }
        ap.width = 1;
        ap.rows = w;
        ap.emit = is_term ? Emit::kTermDensity : Emit::kEltDensity;
        continue;
      }
      if (has_op_trait(t.opcode, op_trait::kRerollWidenable)) {
        if (is_term && width != 1) {
          ok = false;
          break;
        }
        ap.emit = is_term ? Emit::kTermWiden : Emit::kWiden;
        continue;
      }
      ok = false;
    }
    if (!ok) {
      split_here();
      continue;
    }
    // Exactly one delimiter per lane, at its end: any other disposition
    // would leave the term list -- or the vector the lane stores into --
    // describing work that no longer happens.
    for (int p = 0; p < k && ok; ++p) {
      const Emit e = pos[(size_t)p].emit;
      const bool delim =
          e == Emit::kTermDensity || e == Emit::kTermWiden || e == Emit::kStore;
      ok = delim == (p == k - 1);
    }
    if (!ok) continue;

    // Lanes identical down to their immediates and their external slots are
    // one op once CSE runs, and the fused form evaluates all L of them. That
    // is the whole cost of fusing a bucket the const pool already collapsed.
    std::unordered_set<uint64_t> lane_hash;
    for (int64_t l = 0; l < L; ++l) {
      uint64_t h = 1469598103934665603ull;
      const auto mix = [&h](int64_t v) {
        h = (h ^ (uint64_t)v) * 1099511628211ull;
      };
      for (int p = 0; p < k; ++p) {
        const Op& o = op_at(p, l);
        const Pos& ap = pos[(size_t)p];
        for (int j = 0; j < o.n_in; ++j)
          mix(ap.ins[(size_t)j].kind == InKind::kLaneLocal ? -1 : o.in[j]);
        for (int64_t m = 0; m < o.n_idata; ++m) mix(o.idata[m]);
      }
      lane_hash.insert(h);
    }
    const int64_t distinct = (int64_t)lane_hash.size();

    int64_t added = 0, ops_out = 0, lane_elems = 0;
    for (int p = 0; p < k; ++p) {
      const Pos& ap = pos[(size_t)p];
      switch (ap.emit) {
        case Emit::kElide:
          break;
        case Emit::kSlice:
          ++ops_out;
          added += L * ap.width;  // contiguous: no index array, no scatter
          lane_elems += 2 * ap.width;
          break;
        case Emit::kGather:
          ++ops_out;
          added += 2 * L * ap.width;  // gathered forward, scattered back
          lane_elems += 2 * ap.width;
          break;
        case Emit::kTermWiden:
          ops_out += 2;
          lane_elems += 2 * ap.width;
          break;
        case Emit::kShared:
          ++ops_out;
          break;
        case Emit::kEltDensity:
          ops_out += ap.rows > 1 ? 2 : 1;
          lane_elems += ap.rows * kDensityElem;
          if (ap.rows > 1 && elt_costs_per_element(op_at(p, 0).opcode))
            added += L * ap.rows * kDensityElem;
          break;
        case Emit::kTermDensity:
          ++ops_out;
          lane_elems += ap.rows * kDensityElem;
          break;
        default:
          ++ops_out;
          lane_elems += 2 * ap.width;
          break;
      }
    }
    const auto attach_idata = [&](Op& o, std::vector<int> v) {
      g.idata_pool.push_back(std::move(v));
      o.idata = g.idata_pool.back().data();
      o.n_idata = (int64_t)g.idata_pool.back().size();
    };
    added += ops_out * kOpCost + (L - distinct) * lane_elems;
    if (distinct * (int64_t)k * kOpCost <= added + kPartitionMargin) {
      ++st.declined;
      continue;
    }

    // ---- emit ---------------------------------------------------------
    std::vector<Op> out_ops;
    out_ops.reserve((size_t)ops_out);
    // The fused lpmf's outcome vector is the lanes' immediates, laid out the
    // way the kernel unpacks them: one flat array, or group by group with a
    // [len, vals...] header each, every group widened to the fused length.
    const auto attach_outcomes = [&](Op& o, int p, int64_t w) {
      if (!has_op_trait(o.opcode, op_trait::kRerollIdataDensity)) return;
      const Op& t = op_at(p, 0);
      std::vector<int> v;
      if (!has_int_groups(o.opcode)) {
        v.reserve((size_t)(L * w));
        for (int64_t l = 0; l < L; ++l)
          for (int64_t e = 0; e < w; ++e) v.push_back(op_at(p, l).idata[e]);
      } else {
        for (int64_t m = 0; m < t.n_idata; m += group_len(t.idata + m)) {
          v.push_back((int)(L * w));
          for (int64_t l = 0; l < L; ++l)
            for (int64_t e = 0; e < w; ++e)
              v.push_back(group_elem(op_at(p, l).idata + m, e));
        }
      }
      attach_idata(o, std::move(v));
    };
    const auto swap_terms = [&](int p, int new_term) {
      std::unordered_set<int> dead;
      for (int64_t l = 0; l < L; ++l) dead.insert(op_at(p, l).out);
      std::vector<int> next;
      next.reserve(target_terms.size());
      bool placed = false;
      for (int s : target_terms) {
        if (dead.count(s) == 0) {
          next.push_back(s);
        } else if (!placed) {
          next.push_back(new_term);
          placed = true;
        }
      }
      target_terms = std::move(next);
      for (int s : dead) term_set.erase(s);
      term_set.insert(new_term);
    };

    for (int p = 0; p < k; ++p) {
      const Op& t = op_at(p, 0);
      Pos& ap = pos[(size_t)p];
      if (ap.emit == Emit::kElide) {
        ap.out = t.in[0];
        continue;
      }
      if (ap.emit == Emit::kSlice || ap.emit == Emit::kGather) {
        Op rd;
        rd.opcode = ap.emit == Emit::kSlice ? OP_SLICE : OP_GATHER;
        rd.n_in = 1;
        rd.in[0] = t.in[0];
        rd.out = g.add_slot(L * ap.width, false);
        attach_idata(rd, std::move(ap.idx));
        ap.out = rd.out;
        out_ops.push_back(rd);
        continue;
      }
      Op op = t;  // opcode, variant and immediates carry over
      for (int j = 0; j < t.n_in; ++j) {
        const PosIn& in = ap.ins[(size_t)j];
        switch (in.kind) {
          case InKind::kInvariant:
            break;
          case InKind::kLaneLocal:
            op.in[j] = pos[(size_t)in.producer].out;
            break;
          case InKind::kConstLanes: {
            const int cs = g.add_slot(L, false);
            fills.emplace_back(cs, in.values);
            op.in[j] = cs;
            break;
          }
        }
      }
      switch (ap.emit) {
        case Emit::kShared:
          ap.out = op.out;  // lane 0's own slot, written once
          break;
        case Emit::kRowSum:
          op.opcode = OP_SUM_ROWS;
          op.out = g.add_slot(L, false);
          attach_idata(
              op, std::vector<int>{(int)pos[(size_t)ap.ins[0].producer].width});
          ap.out = op.out;
          break;
        case Emit::kEltDensity:
          op.variant = (uint8_t)(op.variant | 0x40u);
          op.out = g.add_slot(L * ap.rows, false);
          attach_outcomes(op, p, ap.rows);
          ap.out = op.out;
          break;
        case Emit::kTermDensity:
          op.out = g.add_slot(1, false);
          attach_outcomes(op, p, ap.rows);
          ap.out = op.out;
          break;
        case Emit::kStore:
          op.opcode = ap.idx.size() == 1 ? OP_SET_SLICE_INPLACE
                                         : OP_SET_SLICE_STRIDED_INPLACE;
          op.out = t.in[0];
          attach_idata(op, std::move(ap.idx));
          ap.out = op.out;
          break;
        default:  // kWiden, kTermWiden
          op.out = g.add_slot(L * ap.width, false);
          ap.out = op.out;
          break;
      }
      out_ops.push_back(op);
      if (ap.emit == Emit::kEltDensity && ap.rows > 1) {
        Op rows;
        rows.opcode = OP_SUM_ROWS;
        rows.n_in = 1;
        rows.in[0] = op.out;
        rows.out = g.add_slot(L, false);
        attach_idata(rows, std::vector<int>{(int)ap.rows});
        out_ops.push_back(rows);
        ap.out = rows.out;
      } else if (ap.emit == Emit::kTermDensity) {
        swap_terms(p, op.out);
      } else if (ap.emit == Emit::kTermWiden) {
        Op sum;
        sum.opcode = OP_SUM_VEC;
        sum.n_in = 1;
        sum.in[0] = op.out;
        sum.out = g.add_slot(1, false);
        out_ops.push_back(sum);
        swap_terms(p, sum.out);
      }
    }

    for (int id : ids)
      for (size_t u = lanes[(size_t)id].begin; u < lanes[(size_t)id].end; ++u)
        dropped[u] = 1;
    emit_at[first_begin] = (int)emitted.size();
    emitted.push_back(std::move(out_ops));
    ++st.groups;
    st.lanes += (int)L;
  }

  if (st.groups == 0) return st;
  std::vector<Op> result;
  result.reserve(n_ops);
  for (size_t u = 0; u < n_ops; ++u) {
    if (emit_at[u] >= 0) {
      const std::vector<Op>& ops = emitted[(size_t)emit_at[u]];
      result.insert(result.end(), ops.begin(), ops.end());
    }
    if (!dropped[u]) result.push_back(g.ops[u]);
  }
  g.ops = std::move(result);
  return st;
}

}  // namespace stanli
