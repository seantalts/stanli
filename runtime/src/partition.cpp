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
// What a bucket's ops read is proven, not assumed: every slot the fused ops
// read from outside must be finished before the first lane starts. That one
// rule carries three of the soundness obligations at once -- no cross-lane
// recurrence, no mid-bucket writer, and no destructive in-place store between
// the lanes -- because all three appear as a writer at or after the first
// lane's position. Ops only ever move EARLIER, so an in-place store proven
// safe against a later reader stays safe.
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
};

struct Pos {
  Emit emit = Emit::kShared;
  std::vector<PosIn> ins;
  std::vector<int> idx;  // kSlice: {start}; kGather: L * width indices
  int64_t width = 1;     // output elements per lane; ignored when kShared
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
      if (!idata_is_shape(op.opcode))
        for (int64_t m = 0; m < op.n_idata; ++m) key.w.push_back(op.idata[m]);
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

  for (const std::vector<int>& ids : buckets) {
    const int64_t L = (int64_t)ids.size();
    if (L < kMinLanes) continue;
    const Lane& lane0 = lanes[(size_t)ids[0]];
    const int k = (int)(lane0.end - lane0.begin);
    const size_t first_begin = lane0.begin;
    const auto op_at = [&](int p, int64_t l) -> const Op& {
      return g.ops[lanes[(size_t)ids[(size_t)l]].begin + (size_t)p];
    };
    // Every slot the fused ops read from outside the bucket must be finished
    // before the first lane: a later writer is a cross-lane recurrence, a
    // mid-bucket store, or a destructive update, and the fused read happens
    // at the first lane's position.
    const auto settled = [&](int s) {
      return s >= 0 && (size_t)s < n_slots &&
             !any_at_or_after(writers[(size_t)s], first_begin);
    };

    pos_of.clear();
    for (int p = 0; p < k; ++p) pos_of[op_at(p, 0).out] = p;

    std::vector<Pos> pos((size_t)k);
    bool ok = true;
    for (int p = 0; p < k && ok; ++p) {
      const Op& t = op_at(p, 0);
      Pos& ap = pos[(size_t)p];
      ap.ins.resize((size_t)t.n_in);
      const bool is_term = term_set.count(t.out) != 0;
      bool all_shared = true;   // this op computes one value for every lane
      int64_t width = 0;        // per-lane elements of the varying inputs
      bool wide_shared = false; // a shared input the kernels cannot broadcast

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
      const bool indexed_read =
          (t.opcode == OP_INDEX || t.opcode == OP_SLICE) && t.n_in == 1 &&
          t.n_idata == 1 && !is_term &&
          ap.ins[0].kind == InKind::kInvariant && !idata_shared;
      if (indexed_read) {
        const int64_t blen = g.slots[(size_t)t.in[0]].len;
        const int64_t w = g.slots[(size_t)t.out].len;
        ap.width = w;
        ap.idx.reserve((size_t)(L * w));
        bool run = true;
        for (int64_t l = 0; l < L; ++l) {
          const int64_t start = op_at(p, l).idata[0];
          if (start < 0 || start + w > blen) {
            ok = false;
            break;
          }
          if (start != t.idata[0] + l * w) run = false;
          for (int64_t e = 0; e < w; ++e) ap.idx.push_back((int)(start + e));
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
        // The idata densities carrying more than one immediate (the
        // binomials' two integer groups) need the group concatenation that
        // is not in this slice yet.
        if (has_op_trait(t.opcode, op_trait::kRerollIdataDensity) &&
            t.n_idata != 1) {
          ok = false;
          break;
        }
        if (width != 1) {
          ok = false;
          break;
        }
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
    if (!ok) continue;
    // Exactly one target term per lane, at the delimiter: any other
    // disposition would leave the term list describing work that no longer
    // happens.
    for (int p = 0; p < k && ok; ++p) {
      const bool term = pos[(size_t)p].emit == Emit::kTermDensity ||
                        pos[(size_t)p].emit == Emit::kTermWiden;
      ok = term == (p == k - 1);
    }
    if (!ok) continue;

    int64_t added = 0, ops_out = 0;
    for (const Pos& ap : pos) {
      switch (ap.emit) {
        case Emit::kElide:
          break;
        case Emit::kSlice:
          ++ops_out;
          added += L * ap.width;  // contiguous: no index array, no scatter
          break;
        case Emit::kGather:
          ++ops_out;
          added += 2 * L * ap.width;  // gathered forward, scattered back
          break;
        case Emit::kTermWiden:
          ops_out += 2;
          break;
        default:
          ++ops_out;
          break;
      }
    }
    added += ops_out * kOpCost;
    if (L * (int64_t)k * kOpCost <= added + kPartitionMargin) {
      ++st.declined;
      continue;
    }

    // ---- emit ---------------------------------------------------------
    std::vector<Op> out_ops;
    out_ops.reserve((size_t)ops_out);
    const auto attach_idata = [&](Op& o, std::vector<int> v) {
      g.idata_pool.push_back(std::move(v));
      o.idata = g.idata_pool.back().data();
      o.n_idata = (int64_t)g.idata_pool.back().size();
    };
    // The fused lpmf's outcome vector is the lanes' immediates.
    const auto attach_outcomes = [&](Op& o, int p) {
      if (!has_op_trait(o.opcode, op_trait::kRerollIdataDensity)) return;
      std::vector<int> outcomes;
      outcomes.reserve((size_t)L);
      for (int64_t l = 0; l < L; ++l) outcomes.push_back(op_at(p, l).idata[0]);
      attach_idata(o, std::move(outcomes));
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
          attach_idata(op, std::vector<int>{
                               (int)pos[(size_t)ap.ins[0].producer].width});
          ap.out = op.out;
          break;
        case Emit::kEltDensity:
          op.variant = (uint8_t)(op.variant | 0x40u);
          op.out = g.add_slot(L, false);
          attach_outcomes(op, p);
          ap.out = op.out;
          break;
        case Emit::kTermDensity:
          op.out = g.add_slot(1, false);
          attach_outcomes(op, p);
          ap.out = op.out;
          break;
        default:  // kWiden, kTermWiden
          op.out = g.add_slot(L * ap.width, false);
          ap.out = op.out;
          break;
      }
      out_ops.push_back(op);
      if (ap.emit == Emit::kTermDensity) {
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
