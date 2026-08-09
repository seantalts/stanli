// Loop re-rolling: lowering unrolls data-bound loops, so a scalar-loop
// model arrives here as N consecutive copies of a small op template whose
// only variation is (a) fresh output slots, (b) per-lane constant inputs
// from the dedup'd const pool, and (c) OP_INDEX immediates advancing
// 0,1,2,... The pass detects such regions and rewrites them into the
// vectorized ops the kernels already support, turning per-op dispatch and
// recorder overhead (~17-20ns per scalar op) into per-region cost.
//
// Template inputs classify as:
//   INVARIANT   same slot every lane -> keep; kernels broadcast len-1
//   CONST_LANES every lane a len-1 fill-backed const -> materialize a
//               constant vector from the VALUES (the pool is dedup'd, so
//               equal data values share slots; never assume slot runs)
//   LANE_LOCAL  the output of an earlier template position in the same
//               lane -> the corresponding vectorized output
// OP_INDEX positions with an invariant base either hoist (idata invariant
// across lanes) or vanish entirely (idata == lane number and the base has
// exactly lane-count elements: the vectorized consumer reads the base).
// A density whose every lane output is a target term becomes one vector
// density: the vector kernels already return the summed lp, which also
// deletes the region's share of the ADD_N reduction tree. A density whose
// lanes feed ops instead (the log_mix mixture idiom) becomes one
// elementwise op (variant bit 6: out[n] is lane n's lp); a widenable op
// whose every lane output is a target term widens and swaps the lanes'
// terms for its OP_SUM_VEC; element stores marching through one vector at
// a constant stride collapse into a single vector store -- or into the
// fused value vector itself -- with every later reference renamed.
//
// Failed classifications report the longest still-classifiable lane
// prefix and retry with it. This is what handles block-structured data
// (rats_model: obs sorted time-major, so INDEX idata restarts 0..29 every
// time block): each block classifies as its own region and the scan
// resumes at the block boundary. Anything unclassifiable bails per-region,
// never per-model: cross-lane reads (parameter recurrences), partial or
// strided INDEX progressions, outputs escaping the lane, opcodes outside
// the vocabulary.
#include <stanli/optable.hpp>
#include <stanli/reroll.hpp>

#include <algorithm>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace stanli {
namespace {

constexpr int64_t kMinLanes = 4;
constexpr int kMaxPeriod = 32;
constexpr int kMaxClassifyAttempts = 6;

// Real-argument lpdfs whose vector instantiation returns the summed lp
// with per-element partials (densities.cpp bind_args shape dispatch).
bool is_density(uint16_t oc) {
  switch (oc) {
    case OP_NORMAL_LPDF:
    case OP_CAUCHY_LPDF:
    case OP_STUDENT_T_LPDF:
    case OP_GAMMA_LPDF:
    case OP_BETA_LPDF:
    case OP_LOGNORMAL_LPDF:
    case OP_UNIFORM_LPDF:
    case OP_DOUBLE_EXP_LPDF:
    case OP_EXPONENTIAL_LPDF:
    case OP_INV_GAMMA_LPDF:
    case OP_STD_NORMAL_LPDF:
      return true;
    default:
      return false;
  }
}

// lpmfs whose integer outcome rides in idata, one value per scalar lane, and
// whose vector instantiation takes the outcomes as one idata array (see
// densities.cpp: Eigen::Map<const VectorXi>(ctx.idata, n_idata)). Lanes of
// these match as a template even though their immediates differ; fusing them
// concatenates the immediates.
bool is_idata_outcome_density(uint16_t oc) {
  switch (oc) {
    case OP_BERNOULLI_LPMF:
    case OP_BERNOULLI_LOGIT_LPMF:
    case OP_POISSON_LPMF:
    case OP_POISSON_LOG_LPMF:
    case OP_NEG_BINOMIAL_2_LPMF:
      return true;
    // Everything in STANLI_INT_DENSITY_LIST has exactly this shape by
    // construction. The ordered densities deliberately do NOT appear:
    // their cutpoint vector is shared by every lane, so element n of it
    // is not lane n's, and the elementwise rewrite would be silently
    // wrong rather than merely unfused.
#define STANLI_INT_DENSITY_CASE(code, fn, nreal, t) case code:
      STANLI_INT_DENSITY_LIST(STANLI_INT_DENSITY_CASE)
#undef STANLI_INT_DENSITY_CASE
      return true;
    default:
      return false;
  }
}

// Ops whose forward and backward shape-dispatch at runtime (len-1
// broadcasts for the binaries, ctx.out.len loops with scalar/vector adjoint
// dispatch for the unaries), so widening scalar lanes to one vector op is
// the same opcode (eltwise_expr.cpp).
bool is_widenable(uint16_t oc) {
  switch (oc) {
    case OP_ADD:
    case OP_SUB:
    case OP_MUL:
    case OP_DIV:
    case OP_NEG:
    case OP_EXPV:
    case OP_LOGV:
    case OP_INV_LOGIT:
    case OP_SQRT:
    case OP_SQUARE:
    case OP_LOG1M:
    case OP_TANHV:
    case OP_LOG_INV_LOGIT:
    case OP_LOG1M_INV_LOGIT:
    // Batched since mixture.cpp grew shape dispatch: any argument len 1 or
    // N, out len N, per-element math bit-identical to the scalar op.
    case OP_LOG_MIX:
    case OP_LSE2:
      return true;
    default:
      return false;
  }
}

enum class InKind { kInvariant, kConstLanes, kLaneLocal, kBad };

struct PosIn {
  InKind kind = InKind::kBad;
  int producer_pos = -1;         // LANE_LOCAL: template position of producer
  std::vector<double> values;    // CONST_LANES: one value per lane
};

struct Pos {
  std::vector<PosIn> ins;
  bool index_elision = false;  // OP_INDEX, idata==lane, base len==lanes
  bool hoist = false;          // all inputs + idata invariant: emit once
  bool term_density = false;   // density, every lane's out a target term
  bool elt_density = false;    // density, every lane's out consumed only
                               //   inside its own lane -> variant bit 6,
                               //   out[n] = lane n's lp
  bool term_widen = false;     // widenable, every lane's out a target term
                               //   -> widen + OP_SUM_VEC, swap the terms
  int slice_start = -1;        // OP_INDEX over a contiguous window
  std::vector<int> gather_idx; // OP_INDEX with a data-driven index
  int store_vec = -1;          // element write filling a window of this
  int store_start = 0;         //   vector, starting here,
  int store_stride = 1;        //   advancing by this much per lane
  bool store_written_after = false;  // someone writes the vector later
  std::vector<int> outcome_idata;  // lpmf: per-lane integer outcomes
};

bool is_element_store(const Op& op) {
  return (op.opcode == OP_SET_INDEX || op.opcode == OP_SET_INDEX_INPLACE) &&
         op.n_in == 2 && op.n_idata == 1 && op.out == op.in[0];
}

// Structural template match; idata may differ across lanes only for
// OP_INDEX (checked as a progression during classification).
bool ops_match(const Graph& g, const Op& a, const Op& b) {
  if (a.opcode != b.opcode || a.variant != b.variant || a.n_in != b.n_in ||
      a.out2 >= 0 || b.out2 >= 0)
    return false;
  for (int j = 0; j < a.n_in; ++j)
    if (g.slots[a.in[j]].len != g.slots[b.in[j]].len) return false;
  if (g.slots[a.out].len != g.slots[b.out].len) return false;
  // Element writes carry their destination index the same way reads do, so
  // the immediate is allowed to advance across lanes for both; lpmf lanes
  // carry their integer outcome there and fuse by concatenating them.
  if (a.opcode == OP_INDEX || a.opcode == OP_SET_INDEX ||
      a.opcode == OP_SET_INDEX_INPLACE)
    return a.n_idata == 1 && b.n_idata == 1;
  if (is_idata_outcome_density(a.opcode)) return a.n_idata == b.n_idata;
  if (a.n_idata != b.n_idata) return false;
  for (int64_t k = 0; k < a.n_idata; ++k)
    if (a.idata[k] != b.idata[k]) return false;
  return true;
}

}  // namespace

RerollStats reroll(Graph& g,
                   std::vector<std::pair<int, std::vector<double>>>& fills,
                   std::vector<int>& target_terms,
                   const std::vector<int>& extra_roots) {
  RerollStats st;
  st.ops_before = static_cast<int64_t>(g.ops.size());
  st.ops_after = st.ops_before;
  if (std::getenv("STANLI_NO_REROLL")) return st;

  // The dedup'd constant pool: slot -> value, for len-1 fills.
  std::unordered_map<int, double> const_val;
  for (const auto& f : fills)
    if (f.second.size() == 1) const_val.emplace(f.first, f.second[0]);

  // Consumers of each slot, by original op index. Ops only read slots
  // produced earlier, so indices stay valid as the scan rewrites disjoint
  // regions left to right.
  std::unordered_map<int, std::vector<size_t>> uses;
  for (size_t u = 0; u < g.ops.size(); ++u)
    for (int j = 0; j < g.ops[u].n_in; ++j)
      uses[g.ops[u].in[j]].push_back(u);

  // Producers of each slot. The write-fusion rewrite below needs to know
  // that nothing else ever writes the vector it is about to take over.
  std::unordered_map<int, std::vector<size_t>> writers;
  for (size_t u = 0; u < g.ops.size(); ++u) {
    if (g.ops[u].out >= 0) writers[g.ops[u].out].push_back(u);
    if (g.ops[u].out2 >= 0) writers[g.ops[u].out2].push_back(u);
  }

  // Both lists are built by walking u upwards, so each one is sorted, and
  // every question asked of them below is a range question: is anything
  // in [lo, hi), is anything at or past hi. Answering those by scanning
  // the whole list is what made this pass quadratic in TIME long after
  // the lazy renaming below made it linear in space -- ldaK5 refills one
  // shared gamma vector from every one of its N iterations, so that
  // slot's lists are N long and every one of the N regions walked all of
  // both. Measured at N=32,000: 11.3 billion list steps, against 3.6
  // million for everything else in the pass put together.
  //
  // Binary search asks the same questions of the same lists and gets the
  // same answers; it just does not read the entries that cannot matter.
  // Entries actually inside a region are still visited, but regions are
  // disjoint, so that total is bounded by the list sizes: O(n log n).
  //
  // Every entry read is counted into st.list_steps, probes included, and
  // that count is what the scaling test asserts on: it is an exact
  // integer for a given graph, so it says the same thing on a laptop and
  // on a shared CI runner, which a wall-clock reading does not.
  const auto first_at_or_after = [&](const std::vector<size_t>& v, size_t x) {
    size_t lo = 0, hi = v.size();
    while (lo < hi) {
      const size_t mid = lo + (hi - lo) / 2;
      ++st.list_steps;
      if (v[mid] < x) lo = mid + 1;
      else hi = mid;
    }
    return v.begin() + (ptrdiff_t)lo;
  };
  // Is any entry of `v` in [lo, hi) not accepted by `ours`? `ours` names
  // the region's own ops, which are allowed to touch the slot.
  const auto any_in_range_but = [&](const std::vector<size_t>* v, size_t lo,
                                    size_t hi, auto&& ours) {
    if (v == nullptr) return false;
    for (auto it = first_at_or_after(*v, lo); it != v->end() && *it < hi;
         ++it) {
      ++st.list_steps;
      if (!ours(*it)) return true;
    }
    return false;
  };
  // Is any entry of `v` at or after `x`?
  const auto any_at_or_after = [&](const std::vector<size_t>* v, size_t x) {
    return v != nullptr && first_at_or_after(*v, x) != v->end();
  };
  // The list for a slot, or null when it has none.
  const auto list_for = [](const std::unordered_map<int,
                                                    std::vector<size_t>>& m,
                           int slot) -> const std::vector<size_t>* {
    const auto it = m.find(slot);
    return it == m.end() ? nullptr : &it->second;
  };

  std::unordered_set<int> term_set(target_terms.begin(), target_terms.end());

  // Write-fusion renaming, done lazily. When a store region is fused, every
  // later reference to its vector means the fused value instead. Rewriting
  // the tail eagerly is quadratic when one vector chains through many
  // regions (ldaK5 refills a single 5-slot gamma 32,877 times: the eager
  // scan cost 52 s and 49 GB of `uses` bookkeeping, OOM-killing every CI
  // runner). Instead the original ops are left untouched -- classification
  // only ever compares slot ids ACROSS lanes, and stale names are stale
  // uniformly -- and the current name is applied at emission time. Values
  // are never chained: remapping the ORIGINAL name to the newest
  // replacement each time keeps lookups single-step.
  std::unordered_map<int, int> renamed;
  const auto resolve = [&renamed](int s) {
    const auto it = renamed.find(s);
    return it == renamed.end() ? s : it->second;
  };

  // Slots read from outside the op graph (jacobian terms, constrained
  // parameter views). They have no consuming op, so `uses` cannot see
  // them; folding a lane that writes one would leave it unwritten.
  std::unordered_set<int> root_set(extra_roots.begin(), extra_roots.end());

  // Scan-cost control: after a hard classification failure (prefix 0,
  // lane-independent evidence) the run gets one more attempt one lane in,
  // then is skipped wholesale for that period. Soft failures (positive
  // prefix) always re-attempt at the reported boundary. Without this,
  // graphs made of enormous near-periodic runs go quadratic (ldaK5:
  // 1.03M ops in 33k-lane log_sum_exp lanes hung the pass; its runs are
  // now pruned by the density pre-check before lane counting).
  std::vector<size_t> retry_at((size_t)kMaxPeriod + 1, 0);
  std::vector<size_t> fail_end((size_t)kMaxPeriod + 1, 0);
  std::vector<bool> hard_failed((size_t)kMaxPeriod + 1, false);

  std::vector<Op> result;
  result.reserve(g.ops.size());
  size_t i = 0;
  while (i < g.ops.size()) {
    bool rewrote = false;
    for (int P = 1; P <= kMaxPeriod && i + 2 * (size_t)P <= g.ops.size();
         ++P) {
      if (i < retry_at[(size_t)P]) continue;
      // Cheap pre-check before any lane counting: a profitable region must
      // contain an allowlisted density (term or elementwise), a per-lane
      // element write (which fuses into one vector store), or a widenable
      // op whose out is a target term (log_mix lanes over already-vector
      // lps: the region is INDEX/INDEX/LOG_MIX with no density at all).
      bool candidate = false;
      for (int p = 0; p < P && !candidate; ++p) {
        const Op& t = g.ops[i + p];
        candidate = is_density(t.opcode) ||
                    is_idata_outcome_density(t.opcode) ||
                    is_element_store(t) ||
                    (is_widenable(t.opcode) && term_set.count(t.out) != 0);
      }
      if (!candidate) continue;

      // Count template-matching lanes.
      int64_t L = 1;
      while (i + ((size_t)L + 1) * P <= g.ops.size()) {
        bool match = true;
        for (int p = 0; p < P && match; ++p)
          match = ops_match(g, g.ops[i + p], g.ops[i + (size_t)L * P + p]);
        if (!match) break;
        ++L;
      }
      if (L < kMinLanes) continue;

      const auto op_at = [&](int p, int64_t l) -> const Op& {
        return g.ops[i + (size_t)l * P + p];
      };

      // ---- classify, shrinking to the reported prefix on failure ----
      std::vector<Pos> pos;
      int64_t Luse = L;
      bool classified = false;
      for (int attempt = 0;
           attempt < kMaxClassifyAttempts && Luse >= kMinLanes; ++attempt) {
        int64_t prefix = Luse;
        pos.assign((size_t)P, Pos{});
        std::unordered_map<int, int> lane0_producer;
        bool ok = true;
        bool any_term_density = false;
        bool any_store = false;
        bool any_elt_density = false;
        bool any_term_widen = false;
        const size_t region_end = i + (size_t)P * (size_t)Luse;
        for (int p = 0; p < P; ++p) {
          const Op& t = op_at(p, 0);
          Pos& ap = pos[p];
          ap.ins.resize(t.n_in);
          bool all_inputs_invariant = true;
          for (int j = 0; j < t.n_in; ++j) {
            // Longest lane prefix under each interpretation; pick the
            // interpretation valid for all Luse lanes, else bound prefix.
            int64_t br_inv = Luse;
            for (int64_t l = 1; l < Luse; ++l)
              if (op_at(p, l).in[j] != t.in[j]) {
                br_inv = l;
                break;
              }
            if (br_inv == Luse) {
              ap.ins[j].kind = InKind::kInvariant;
              continue;
            }
            all_inputs_invariant = false;
            int64_t br_local = 0;
            auto pit = lane0_producer.find(t.in[j]);
            if (pit != lane0_producer.end()) {
              br_local = Luse;
              for (int64_t l = 1; l < Luse; ++l)
                if (op_at(p, l).in[j] != op_at(pit->second, l).out) {
                  br_local = l;
                  break;
                }
              if (br_local == Luse) {
                ap.ins[j].kind = InKind::kLaneLocal;
                ap.ins[j].producer_pos = pit->second;
                continue;
              }
            }
            int64_t br_const = Luse;
            std::vector<double> vals;
            vals.reserve((size_t)Luse);
            for (int64_t l = 0; l < Luse; ++l) {
              auto cit = const_val.find(op_at(p, l).in[j]);
              if (cit == const_val.end()) {
                br_const = l;
                break;
              }
              vals.push_back(cit->second);
            }
            if (br_const == Luse) {
              ap.ins[j].kind = InKind::kConstLanes;
              ap.ins[j].values = std::move(vals);
              continue;
            }
            ok = false;
            prefix =
                std::min(prefix, std::max({br_inv, br_local, br_const}));
          }

          // Output discipline prefixes. A lane's out may be consumed only
          // by later ops of its own lane instance; density outs may
          // instead be target terms (with no op consumers at all).
          int64_t br_term = Luse;     // lanes whose out IS a term
          int64_t br_nonterm = Luse;  // lanes whose out is NOT a term
          int64_t br_internal = Luse; // lanes whose out does not escape
          for (int64_t l = 0; l < Luse; ++l) {
            const int o = op_at(p, l).out;
            const bool is_term = term_set.count(o) != 0;
            if (!is_term && br_term == Luse) br_term = l;
            if (is_term && br_nonterm == Luse) br_nonterm = l;
            if (br_internal == Luse && root_set.count(o) != 0) br_internal = l;
            if (br_internal == Luse) {
              auto uit = uses.find(o);
              if (uit != uses.end())
                for (size_t u : uit->second) {
                  const bool inside = u > i + (size_t)l * P + p &&
                                      u < i + ((size_t)l + 1) * P;
                  if (!inside) {
                    br_internal = l;
                    break;
                  }
                }
            }
          }

          // Position-level classification.
          if (t.opcode == OP_INDEX) {
            int64_t br_prog = Luse, br_iinv = Luse;
            for (int64_t l = 0; l < Luse; ++l) {
              const int v = op_at(p, l).idata[0];
              if (v != l && br_prog == Luse) br_prog = l;
              if (v != t.idata[0] && br_iinv == Luse) br_iinv = l;
            }
            const int64_t blen = g.slots[t.in[0]].len;
            const int64_t io_ok = std::min(br_internal, br_nonterm);
            // Contiguous ascending run (idata[l] == idata[0] + l) and
            // in-range indices: the two cheaper rewrites below.
            int64_t br_run = Luse;
            bool in_range = true;
            for (int64_t l = 0; l < Luse; ++l) {
              const int v = op_at(p, l).idata[0];
              if (v != t.idata[0] + l && br_run == Luse) br_run = l;
              if (v < 0 || v >= blen) in_range = false;
            }
            if (ap.ins[0].kind != InKind::kInvariant || io_ok < Luse) {
              ok = false;
              prefix = std::min(prefix, io_ok);
            } else if (br_prog == Luse && blen == Luse) {
              ap.index_elision = true;  // reads the whole base, in order
            } else if (br_iinv == Luse) {
              ap.hoist = true;  // same element every lane
            } else if (br_run == Luse && t.idata[0] + Luse <= blen) {
              ap.slice_start = t.idata[0];  // contiguous window -> OP_SLICE
            } else if (in_range) {
              // Arbitrary data-driven index (`alpha[county_idx[n]]`, the
              // hierarchical idiom) -> one OP_GATHER over the lane indices.
              ap.gather_idx.reserve((size_t)Luse);
              for (int64_t l = 0; l < Luse; ++l)
                ap.gather_idx.push_back(op_at(p, l).idata[0]);
            } else {
              ok = false;
              prefix = 0;  // out-of-range index: not ours to rewrite
            }
          } else if (is_element_store(t)) {
            // `mu[n] = ...` under an unrolled loop, after the destructive
            // rewrite: N writes into one vector, each at its own index. When
            // those indices march by a constant stride the whole run
            // collapses into a single vector store -- or into nothing at
            // all, when a contiguous run covers the vector and the
            // vectorized values can simply BE it.
            //
            // The output escaping the lane is the point here, so the usual
            // escape test does not apply. What does: the vector must be the
            // same one every lane, and no one else may read it while it is
            // half-written. Writes AFTER the run are fine -- every later
            // reference, read or write, is renamed to the store's output, so
            // interleaved runs (dogs fills a matrix in 30 column-comb
            // blocks) chain through fresh slots block by block -- but they
            // do rule out the store-free form, whose "output" is a slot
            // nobody may touch again.
            const int vec = t.in[0];
            const int64_t blen = g.slots[vec].len;
            // Indices must march by a constant positive stride: 1 is the
            // vector case, larger is a column-major matrix filled along its
            // minor axis (dogs writes p[j, t] with t inner, so the flat
            // index advances by the row count).
            const int64_t stride =
                Luse >= 2 ? op_at(p, 1).idata[0] - t.idata[0] : 1;
            int64_t br_run = stride >= 1 ? Luse : 0;
            for (int64_t l = 0; l < br_run; ++l)
              if (op_at(p, l).idata[0] != t.idata[0] + l * stride) {
                br_run = l;
                break;
              }
            bool clean = ap.ins[0].kind == InKind::kInvariant &&
                         (ap.ins[1].kind == InKind::kLaneLocal ||
                          ap.ins[1].kind == InKind::kConstLanes) &&
                         t.idata[0] >= 0 && br_run > 0 &&
                         t.idata[0] + (br_run - 1) * stride < blen &&
                         !root_set.count(vec) && !term_set.count(vec);
            bool written_after = false;
            if (clean && ap.ins[1].kind == InKind::kLaneLocal) {
              // When the value comes from an elided index the fused value IS
              // that op's base -- a pre-existing slot rather than a fresh
              // one. Redirecting the vector's readers to it is only sound
              // while nobody writes it after the run, so a later writer
              // forces the store form.
              const Pos& prod = pos[(size_t)ap.ins[1].producer_pos];
              if (prod.index_elision) {
                const int base = op_at(ap.ins[1].producer_pos, 0).in[0];
                if (any_at_or_after(list_for(writers, base), region_end))
                  written_after = true;
              }
            }
            if (clean) {
              // Everything touching `vec` INSIDE the region must be one of
              // this region's own writes; anything after it gets renamed.
              auto in_region_write = [&](size_t u) {
                return u >= i && u < region_end &&
                       (u - i) % (size_t)P == (size_t)p;
              };
              const std::vector<size_t>* vec_uses = list_for(uses, vec);
              const std::vector<size_t>* vec_writers = list_for(writers, vec);
              if (any_in_range_but(vec_uses, i, region_end, in_region_write) ||
                  any_in_range_but(vec_writers, i, region_end, in_region_write))
                clean = false;
              if (any_at_or_after(vec_writers, region_end))
                written_after = true;
            }
            if (!clean || br_run < Luse) {
              ok = false;
              prefix = std::min(prefix, clean ? br_run : (int64_t)0);
            } else {
              ap.store_vec = vec;
              ap.store_start = (int)t.idata[0];
              ap.store_stride = (int)stride;
              ap.store_written_after = written_after;
              any_store = true;
            }
          } else if (is_density(t.opcode) ||
                     is_idata_outcome_density(t.opcode)) {
            // Two fusable dispositions: every lane's out IS a target term
            // (one summed vector density), or NO lane's out is a term and
            // each is consumed only inside its own lane (one elementwise
            // density, variant bit 6 -- the log_mix/log_sum_exp mixture
            // idiom). Mixed lanes or escaping outputs bound the prefix.
            const bool all_terms = br_term == Luse;
            const bool no_terms = br_nonterm == Luse;
            if (br_internal < Luse || (!all_terms && !no_terms)) {
              ok = false;
              prefix = std::min(
                  prefix, std::min(br_internal, std::max(br_term, br_nonterm)));
            } else if (is_idata_outcome_density(t.opcode) && t.n_idata != 1) {
              ok = false;
              prefix = 0;  // already a vector op; nothing to fuse
            } else if (all_terms) {
              auto uit = uses.find(t.out);
              if (uit != uses.end() && !uit->second.empty()) {
                ok = false;
                prefix = 0;  // a term that is also an op input
              } else if (all_inputs_invariant &&
                         !is_idata_outcome_density(t.opcode)) {
                // L identical lanes (the const pool dedup'd even the data
                // argument): the "fused" density would compute one lane's
                // lp where the target owes L of them.
                ok = false;
                prefix = 0;
              } else {
                ap.term_density = true;
                any_term_density = true;
                if (is_idata_outcome_density(t.opcode)) {
                  ap.outcome_idata.reserve((size_t)Luse);
                  for (int64_t l = 0; l < Luse; ++l)
                    ap.outcome_idata.push_back(op_at(p, l).idata[0]);
                }
              }
            } else if (all_inputs_invariant &&
                       !is_idata_outcome_density(t.opcode)) {
              // Every lane computes the same scalar lp: keep ONE scalar op
              // and let the lanes' consumers broadcast it. Widening scalar
              // inputs into a len-N out is the losscurve hazard.
              ap.hoist = true;
            } else {
              ap.elt_density = true;
              any_elt_density = true;
              if (is_idata_outcome_density(t.opcode)) {
                ap.outcome_idata.reserve((size_t)Luse);
                for (int64_t l = 0; l < Luse; ++l)
                  ap.outcome_idata.push_back(op_at(p, l).idata[0]);
              }
            }
          } else if (all_inputs_invariant) {
            const int64_t io_ok = std::min(br_internal, br_nonterm);
            if (io_ok == Luse) {
              ap.hoist = true;
            } else {
              ok = false;
              prefix = std::min(prefix, io_ok);
            }
          } else if (is_widenable(t.opcode)) {
            if (br_term == Luse && br_internal == Luse) {
              // Every lane's out is a target term (log_mix under
              // `target +=`): widen the op, SUM_VEC the lanes, and swap
              // the N terms for the sum.
              auto uit = uses.find(t.out);
              if (uit != uses.end() && !uit->second.empty()) {
                ok = false;
                prefix = 0;  // a term that is also an op input
              } else {
                ap.term_widen = true;
                any_term_widen = true;
              }
            } else {
              const int64_t io_ok =
                  std::min(br_internal, std::max(br_term, br_nonterm));
              if (io_ok < Luse || br_nonterm < Luse) {
                ok = false;
                prefix = std::min(prefix, io_ok);
              }
            }
          } else {
            ok = false;
            prefix = 0;  // opcode outside the vocabulary: no prefix helps
          }
          lane0_producer[t.out] = p;
        }
        if (ok && (any_term_density || any_store || any_elt_density ||
                   any_term_widen)) {
          classified = true;
          break;
        }
        if (ok) prefix = 0;  // classifiable but useless
        if (prefix >= Luse) prefix = Luse - 1;    // guarantee progress
        Luse = prefix;
      }

      if (!classified) {
        // Bookkeeping. Soft failures (positive prefix) re-attempt at the
        // reported boundary; hard failures (prefix 0) get one second
        // chance one lane in, then the whole run is skipped.
        const size_t run_end = i + (size_t)P * (size_t)L;
        if (Luse > 0) {
          retry_at[(size_t)P] = i + (size_t)P * (size_t)Luse;
          hard_failed[(size_t)P] = false;
        } else if (hard_failed[(size_t)P] && i < fail_end[(size_t)P]) {
          retry_at[(size_t)P] = fail_end[(size_t)P];
        } else {
          fail_end[(size_t)P] = run_end;
          hard_failed[(size_t)P] = true;
          retry_at[(size_t)P] = i + (size_t)P;
        }
        continue;
      }

      // ---- rewrite the classified prefix [i, i + P*Luse) ----
      std::vector<int> pos_out((size_t)P, -1);
      for (int p = 0; p < P; ++p) {
        const Op& t = op_at(p, 0);
        Pos& ap = pos[(size_t)p];
        if (ap.index_elision) {
          pos_out[(size_t)p] = resolve(t.in[0]);
          continue;
        }
        if (ap.hoist) {
          Op h = t;
          for (int j = 0; j < h.n_in; ++j) h.in[j] = resolve(h.in[j]);
          result.push_back(h);
          pos_out[(size_t)p] = h.out;
          continue;
        }
        if (ap.store_vec >= 0) {
          // The lanes' values, as one vector.
          int W = -1;
          if (ap.ins[1].kind == InKind::kLaneLocal) {
            W = pos_out[(size_t)ap.ins[1].producer_pos];
          } else {
            W = g.add_slot(Luse, false);
            fills.emplace_back(W, ap.ins[1].values);
          }
          // Every lane writing the same scalar (the value chain stayed
          // scalar because all its inputs were lane-invariant): the store
          // wants a vector, so broadcast it into one.
          if (g.slots[W].len == 1 && Luse > 1) {
            Op rv;
            rv.opcode = OP_REP_VEC;
            rv.n_in = 1;
            rv.in[0] = W;
            rv.out = g.add_slot(Luse, false);
            result.push_back(rv);
            W = rv.out;
          }
          const int vec = ap.store_vec;
          int replacement = W;
          if (ap.store_written_after || ap.store_stride != 1 ||
              ap.store_start != 0 || Luse != g.slots[vec].len) {
            // A window (or a comb) rather than the whole vector: the
            // untouched elements still have to come from somewhere, so this
            // is a real store.
            Op sv;
            sv.opcode =
                ap.store_stride == 1 ? OP_SET_SLICE : OP_SET_SLICE_STRIDED;
            sv.n_in = 2;
            sv.in[0] = resolve(vec);
            sv.in[1] = W;
            sv.out = g.add_slot(g.slots[vec].len, false);
            std::vector<int> sidata{ap.store_start};
            if (ap.store_stride != 1) sidata.push_back(ap.store_stride);
            g.idata_pool.push_back(std::move(sidata));
            sv.idata = g.idata_pool.back().data();
            sv.n_idata = (int64_t)g.idata_pool.back().size();
            result.push_back(sv);
            replacement = sv.out;
          }
          // Every later reference to the vector -- read or write -- now
          // means the fused value: recorded here, applied when those ops
          // are emitted. Renaming the writes too is what lets interleaved
          // runs chain: the next block's element writes still NAME the
          // original vector, resolve to this block's store output when
          // that block fuses in its turn, and repeat the process on a
          // fresh slot. Ops before the region were emitted already; they
          // saw the pre-write contents and still do.
          renamed[vec] = replacement;
          pos_out[(size_t)p] = replacement;
          continue;
        }
        if (ap.slice_start >= 0 || !ap.gather_idx.empty()) {
          // One vector read replaces the lanes' scalar reads. Both kernels
          // scatter their adjoints back into the base, gather in ascending
          // lane order so repeated indices accumulate like the var path.
          Op rd;
          rd.opcode = ap.slice_start >= 0 ? OP_SLICE : OP_GATHER;
          rd.n_in = 1;
          rd.in[0] = resolve(t.in[0]);
          rd.out = g.add_slot(Luse, false);
          std::vector<int> idata;
          if (ap.slice_start >= 0)
            idata.push_back(ap.slice_start);
          else
            idata = std::move(ap.gather_idx);
          g.idata_pool.push_back(std::move(idata));
          rd.idata = g.idata_pool.back().data();
          rd.n_idata = (int64_t)g.idata_pool.back().size();
          pos_out[(size_t)p] = rd.out;
          result.push_back(rd);
          continue;
        }
        Op op = t;  // opcode, variant, idata carry over
        bool all_scalar = true;
        for (int j = 0; j < t.n_in; ++j) {
          switch (ap.ins[j].kind) {
            case InKind::kInvariant:
              op.in[j] = resolve(t.in[j]);
              if (g.slots[t.in[j]].len != 1) all_scalar = false;
              break;
            case InKind::kLaneLocal:
              op.in[j] = pos_out[(size_t)ap.ins[j].producer_pos];
              if (g.slots[op.in[j]].len != 1) all_scalar = false;
              break;
            case InKind::kConstLanes: {
              const int cs = g.add_slot(Luse, false);
              fills.emplace_back(cs, ap.ins[j].values);
              op.in[j] = cs;
              all_scalar = false;
              break;
            }
            case InKind::kBad:
              break;  // unreachable: classification succeeded
          }
        }
        // Swap the Luse lane terms for one replacement, at the first
        // lane's position (term_density and term_widen both end here).
        const auto swap_terms = [&](int new_term) {
          std::unordered_set<int> dead;
          for (int64_t l = 0; l < Luse; ++l) dead.insert(op_at(p, l).out);
          std::vector<int> next_terms;
          next_terms.reserve(target_terms.size());
          bool placed = false;
          for (int s : target_terms) {
            if (dead.count(s)) {
              if (!placed) {
                next_terms.push_back(new_term);
                placed = true;
              }
            } else {
              next_terms.push_back(s);
            }
          }
          target_terms = std::move(next_terms);
          for (int s : dead) term_set.erase(s);
          term_set.insert(new_term);
        };
        if (ap.elt_density) {
          // One density op with variant bit 6: out[n] is lane n's lp, read
          // by the lanes' (widened) consumers. An all-scalar real-arg
          // density classified as hoist instead, so a vector input or a
          // per-lane outcome exists here and the out is genuinely len-N.
          op.variant = (uint8_t)(op.variant | 0x40u);
          op.out = g.add_slot(Luse, false);
          if (!ap.outcome_idata.empty()) {
            g.idata_pool.push_back(std::move(ap.outcome_idata));
            op.idata = g.idata_pool.back().data();
            op.n_idata = (int64_t)g.idata_pool.back().size();
          }
          pos_out[(size_t)p] = op.out;
          result.push_back(op);
          continue;
        }
        if (ap.term_widen) {
          int term_slot;
          if (all_scalar) {
            // Every lane's term is the same scalar: one op, times L.
            result.push_back(op);  // scalar op, out = t.out
            Op mul;
            mul.opcode = OP_MUL;
            mul.n_in = 2;
            mul.in[0] = op.out;
            const int lc = g.add_slot(1, false);
            fills.emplace_back(lc, std::vector<double>{(double)Luse});
            mul.in[1] = lc;
            mul.out = g.add_slot(1, false);
            result.push_back(mul);
            term_slot = mul.out;
            pos_out[(size_t)p] = op.out;
          } else {
            op.out = g.add_slot(Luse, false);
            result.push_back(op);
            Op sum;
            sum.opcode = OP_SUM_VEC;
            sum.n_in = 1;
            sum.in[0] = op.out;
            sum.out = g.add_slot(1, false);
            result.push_back(sum);
            term_slot = sum.out;
            pos_out[(size_t)p] = op.out;
          }
          swap_terms(term_slot);
          continue;
        }
        // A widened op whose mapped inputs are all len-1 computes the same
        // value in every lane -- its lane-varying inputs came from HOISTED
        // producers, which are scalars. Widening it anyway hands the kernels
        // scalar inputs with a vector output, and their scalar-x-scalar
        // paths write element 0 only (found by the corpus A/B on
        // losscurve_sislob: a cohort's lm window filled with arena zeros).
        // Keep it scalar; consumers broadcast, and the store materializes.
        if (!ap.term_density && all_scalar) {
          pos_out[(size_t)p] = op.out;  // t's own (lane 0) output slot
          result.push_back(op);
          continue;
        }
        if (ap.term_density) {
          if (all_scalar && ap.outcome_idata.empty()) {
            // Every lane's density is the same scalar (lane-varying inputs
            // all came from hoisted producers): the target owes L copies of
            // it, and L times one lane is that, exactly -- the density
            // backward sees out_adj = L and scales its partials to match.
            result.push_back(op);  // scalar density, out = t.out
            Op mul;
            mul.opcode = OP_MUL;
            mul.n_in = 2;
            mul.in[0] = op.out;
            const int lc = g.add_slot(1, false);
            fills.emplace_back(lc, std::vector<double>{(double)Luse});
            mul.in[1] = lc;
            mul.out = g.add_slot(1, false);
            op = mul;  // the term-swap below installs mul.out as the term
          } else {
            op.out = g.add_slot(1, false);
          }
          if (!ap.outcome_idata.empty()) {
            // The fused lpmf's outcome vector is the lanes' immediates.
            g.idata_pool.push_back(std::move(ap.outcome_idata));
            op.idata = g.idata_pool.back().data();
            op.n_idata = (int64_t)g.idata_pool.back().size();
          }
          swap_terms(op.out);
        } else {
          op.out = g.add_slot(Luse, false);
        }
        pos_out[(size_t)p] = op.out;
        result.push_back(op);
      }
      i += (size_t)P * (size_t)Luse;
      ++st.regions;
      rewrote = true;
      break;
    }
    if (!rewrote) {
      Op op = g.ops[i];
      if (!renamed.empty()) {
        for (int j = 0; j < op.n_in; ++j) op.in[j] = resolve(op.in[j]);
        op.out = resolve(op.out);
      }
      result.push_back(op);
      ++i;
    }
  }
  g.ops = std::move(result);
  st.ops_after = static_cast<int64_t>(g.ops.size());
  return st;
}

}  // namespace stanli
