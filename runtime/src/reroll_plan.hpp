// Private lane plan shared by reroll selection and emission. Not installed.
#ifndef STANLI_REROLL_PLAN_HPP
#define STANLI_REROLL_PLAN_HPP

#include "pass_util.hpp"

#include <optional>
#include <unordered_set>

namespace stanli {
namespace detail {
namespace reroll_plan {

enum class InKind { kInvariant, kConstLanes, kLaneLocal, kBad };

struct PosIn {
  InKind kind = InKind::kBad;
  int producer_pos = -1;       // LANE_LOCAL: template position of producer
  std::vector<double> values;  // CONST_LANES: `width` values per lane
  int64_t width = 1;
  bool tile_wide = false;  // INVARIANT, len == the position's own width:
                           //   tile via OP_REP_MAT instead of rejecting
};

// How L lanes of C elements sit in one L*C vector: element k of lane l is
// at flat position `lane_stride*l + elem_stride*k`. Row-major rows
// (lane_stride=C, elem_stride=1) and column-major rows (lane_stride=1,
// elem_stride=L) are the two conventions a region may commit to; every
// wide row op in one region must agree on which. A position's own read
// of the base is either an elision (the fused consumer reads the base
// directly, no op) or a slice (one OP_SLICE at `offset`, a window that
// does not cover the whole base); `kNone` means neither applies.
struct LaneLayout {
  int64_t lane_stride = 0;
  int64_t elem_stride = 1;
  enum class Kind { kNone, kElide, kSlice } kind = Kind::kNone;
  int64_t offset = 0;  // meaningful only for kSlice

  int64_t flat_at(int64_t l, int64_t k) const {
    return lane_stride * l + elem_stride * k;
  }
};

struct Pos {
  std::vector<PosIn> ins;
  int64_t width = 1;            // elements per lane through this position
  LaneLayout read;              // OP_INDEX/row elision or slice of the base
  bool row_store = false;       // SET_SLICE of row `lane`, lanes cover it
  int store_src = -1;           // the store's base at lane 0
  bool hoist = false;           // all inputs + idata invariant: emit once
  bool term_density = false;    // density, every lane's out a target term
  bool elt_density = false;     // density, every lane's out consumed only
                                //   inside its own lane -> variant bit 6,
                                //   out[n] = lane n's lp
  int64_t rows = 1;             // elt_density with ap.width > 1: outcome
                                //   elements reduced back per lane by
                                //   OP_SUM_ROWS; ap.width becomes 1 after
  bool term_widen = false;      // widenable, every lane's out a target term
                                //   -> widen + OP_SUM_VEC, swap the terms
  std::vector<int> gather_idx;  // OP_INDEX with a data-driven index
  int store_vec = -1;           // element write filling a window of this
  int store_start = 0;          //   vector, starting here,
  int store_stride = 1;         //   advancing by this much per lane
  bool store_written_after = false;  // someone writes the vector later
  std::vector<int> outcome_idata;    // lpmf: per-lane integer outcomes
};

// Classification fills positions without publishing graph mutations. Pricing
// reads the same plan. After acceptance emit consumes the packing payloads;
// the flags remain available for preparation diagnostics. This is a one-use
// preparation object, not an executor/AD record or an eligibility proof.
struct CandidatePlan {
  int64_t lanes = 0;
  bool column_major = false;
  std::vector<Pos> positions;
};

// Adapter over the immutable input op sequence. Graph slot/idata allocations
// during emission cannot invalidate it: the driver publishes g.ops only after
// the entire scan. Target replacement preserves the original first-leaf
// position, including surrounding terms and lazy region-to-region renaming.
struct GraphSource {
  static constexpr bool kDistinctLaneHashes = false;
  const Op* ops;
  int period;
  const Op& at(int p, int64_t lane) const {
    return ops[(size_t)lane * (size_t)period + (size_t)p];
  }
  void replace_terms(int p, int64_t Luse, int new_term,
                     std::vector<int>& target_terms,
                     std::unordered_set<int>& term_set) const {
    std::unordered_set<int> dead;
    for (int64_t l = 0; l < Luse; ++l) dead.insert(at(p, l).out);
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
  }
};

// Existing cost model, including CSE's distinct-input-lane discount. Geometry
// alone must not select a vector plan: choosing this form changes reductions
// and broadcast-adjoint accumulation order. Source::at must describe every
// lane; a few observed iterations are not an affine/control proof.
template <class Source>
bool profitable(const Graph& g, const CandidatePlan& plan,
                const Source& source) {
  const auto& pos = plan.positions;
  const int P = static_cast<int>(pos.size());
  const int64_t Luse = plan.lanes;
  const bool layout_cols = plan.column_major;
  int64_t ops_out = 0, added = 0, lane_elems = 0;
  for (int p = 0; p < P; ++p) {
    const Pos& ap = pos[(size_t)p];
    const int64_t tile_width = ap.rows > 1 ? ap.rows : ap.width;
    for (const PosIn& in : ap.ins)
      if (in.tile_wide) {
        ++ops_out;
        added += Luse * tile_width;
      }
    if (ap.read.kind == LaneLayout::Kind::kElide) {
      continue;
    } else if (ap.hoist) {
      ++ops_out;
    } else if (ap.read.kind == LaneLayout::Kind::kSlice) {
      ++ops_out;
      added += Luse * ap.width;
      lane_elems += 2 * ap.width;
    } else if (!ap.gather_idx.empty()) {
      ++ops_out;
      added += 2 * Luse * ap.width;
      lane_elems += 2 * ap.width;
    } else if (ap.term_density) {
      ++ops_out;
      lane_elems += ap.width * kLaneDensityElem;
    } else if (ap.elt_density) {
      const uint16_t opcode = source.at(p, 0).opcode;
      ops_out += ap.rows > 1 ? 2 : 1;
      lane_elems += ap.rows * kLaneDensityElem;
      if (ap.rows > 1 && lane_elt_costs_per_element(opcode))
        added += Luse * ap.rows * kLaneDensityElem;
      if (ap.rows > 1 && layout_cols) {
        // The column-major repack ahead of OP_SUM_ROWS: one gather,
        // priced like any other.
        ++ops_out;
        added += 2 * Luse * ap.rows;
        lane_elems += 2 * ap.rows;
      }
    } else if (ap.term_widen) {
      ops_out += 2;
      lane_elems += 2 * ap.width;
    } else if (ap.store_vec >= 0) {
      const bool whole =
          ap.row_store || (ap.store_stride == 1 && ap.store_start == 0 &&
                           Luse == g.slots[(size_t)ap.store_vec].len);
      if (!whole || ap.store_written_after) {
        ++ops_out;
        lane_elems += 2 * ap.width;
      }
    } else {
      ++ops_out;
      lane_elems += 2 * ap.width;
    }
  }
  // A source may prove these exact hashes distinct without constructing
  // them. This is stronger than distinct data values or distinct input tuples:
  // multiple varying fields can collide. Arbitrary graph sources still hash.
  // Retain the graph path's original container lifetime through the return;
  // the proved specialization leaves it empty and the compiler removes it.
  std::unordered_set<uint64_t> lane_hash;
  int64_t distinct = Luse;
  if constexpr (!Source::kDistinctLaneHashes) {
    for (int64_t l = 0; l < Luse; ++l) {
      uint64_t h = 1469598103934665603ull;
      const auto mix = [&h](int64_t v) {
        h = (h ^ (uint64_t)v) * 1099511628211ull;
      };
      for (int p = 0; p < P; ++p) {
        const Op& o = source.at(p, l);
        const Pos& ap = pos[(size_t)p];
        for (int j = 0; j < o.n_in; ++j)
          mix(ap.ins[(size_t)j].kind == InKind::kLaneLocal ? -1 : o.in[j]);
        for (int64_t m = 0; m < o.n_idata; ++m) mix(o.idata[m]);
      }
      lane_hash.insert(h);
    }
    distinct = (int64_t)lane_hash.size();
  }
  added += ops_out * kLaneOpCost + (Luse - distinct) * lane_elems;
  return distinct * (int64_t)P * kLaneOpCost > added + kLanePartitionMargin;
}

// The classifier supplies the structural proof. This move-only boundary
// additionally enforces the unchanged cost gate and binds the source used
// for pricing to the source used for emission. A rejection leaves candidate
// storage intact for the driver's prefix retry; acceptance transfers it once.
// Source must have immutable value semantics (or be a stable borrowed view):
// copying it must preserve all lane identities and target-publication facts.
// Call emit once; its rvalue qualification expresses the consumption contract,
// but does not by itself prevent a caller from reusing a moved-from object.
template <class Source>
class SelectedPlan {
 public:
  SelectedPlan(const SelectedPlan&) = delete;
  SelectedPlan& operator=(const SelectedPlan&) = delete;
  SelectedPlan(SelectedPlan&&) = default;
  SelectedPlan& operator=(SelectedPlan&&) = default;

  static std::optional<SelectedPlan> select(const Graph& g,
                                            CandidatePlan& candidate,
                                            const Source& source) {
    if (!profitable(g, candidate, source)) return std::nullopt;
    return SelectedPlan(std::move(candidate), source);
  }
  const CandidatePlan& description() const { return plan_; }

  // Consumes packing payloads, emitting existing kernels in their original
  // order. Target/rename state stays live across regions; it is not captured
  // in the plan. The caller publishes the completed op sequence afterward.
  void emit(Graph& g, Fills& fills, std::vector<int>& target_terms,
            std::unordered_set<int>& term_set, std::vector<Op>& result,
            std::unordered_map<int, int>& renamed) &&;

 private:
  SelectedPlan(CandidatePlan&& plan, const Source& source)
      : plan_(std::move(plan)), source_(source) {}
  CandidatePlan plan_;
  Source source_;
};

inline int resolve_slot(const std::unordered_map<int, int>& renamed, int slot) {
  const auto it = renamed.find(slot);
  return it == renamed.end() ? slot : it->second;
}

template <class Source>
void SelectedPlan<Source>::emit(Graph& g, Fills& fills,
                                std::vector<int>& target_terms,
                                std::unordered_set<int>& term_set,
                                std::vector<Op>& result,
                                std::unordered_map<int, int>& renamed) && {
  CandidatePlan& plan = plan_;
  const Source& source = source_;
  const auto resolve = [&](int slot) { return resolve_slot(renamed, slot); };
  auto& pos = plan.positions;
  const int P = static_cast<int>(pos.size());
  const int64_t Luse = plan.lanes;
  const bool layout_cols = plan.column_major;
  std::vector<int> pos_out((size_t)P, -1);
  const auto packed_const = [&](const PosIn& in, int64_t w) {
    // Element k of lane l in a vector of `w`-wide lanes, per the
    // region's committed convention.
    const LaneLayout lay = layout_cols ? LaneLayout{1, Luse} : LaneLayout{w, 1};
    std::vector<double> packed((size_t)(Luse * w));
    for (int64_t l = 0; l < Luse; ++l)
      for (int64_t k = 0; k < w; ++k)
        packed[(size_t)lay.flat_at(l, k)] =
            in.values[(size_t)(l * in.width + (in.width == 1 ? 0 : k))];
    const int cs = g.add_slot(Luse * w, false);
    fills.emplace_back(cs, std::move(packed));
    return cs;
  };
  for (int p = 0; p < P; ++p) {
    const Op& t = source.at(p, 0);
    Pos& ap = pos[(size_t)p];
    if (ap.read.kind == LaneLayout::Kind::kElide) {
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
        W = packed_const(ap.ins[1], ap.width);
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
      const bool whole =
          ap.row_store || (ap.store_stride == 1 && ap.store_start == 0 &&
                           Luse == g.slots[vec].len);
      if (ap.store_written_after || !whole) {
        // A window (or a comb) rather than the whole vector: the
        // untouched elements still have to come from somewhere, so this
        // is a real store.
        Op sv;
        sv.opcode = ap.store_stride == 1 ? OP_SET_SLICE : OP_SET_SLICE_STRIDED;
        sv.n_in = 2;
        sv.in[0] = resolve(ap.store_src);
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
    const bool is_slice = ap.read.kind == LaneLayout::Kind::kSlice;
    if (is_slice || !ap.gather_idx.empty()) {
      // One vector read replaces the lanes' scalar reads. Both kernels
      // scatter their adjoints back into the base, gather in ascending
      // lane order so repeated indices accumulate like the var path.
      Op rd;
      rd.opcode = is_slice ? OP_SLICE : OP_GATHER;
      rd.n_in = 1;
      rd.in[0] = resolve(t.in[0]);
      rd.out = g.add_slot(Luse * ap.width, false);
      std::vector<int> idata;
      if (is_slice)
        idata.push_back((int)ap.read.offset);
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
    // elt_density with ap.rows > 1 sets ap.width to 1 for downstream
    // consumers (its own output is one lp per lane, after OP_SUM_ROWS);
    // its own operands still pack at the row width.
    const int64_t eff_width = ap.rows > 1 ? ap.rows : ap.width;
    if (eff_width > 1 && !ap.outcome_idata.empty()) {
      const LaneLayout lay =
          layout_cols ? LaneLayout{1, Luse} : LaneLayout{eff_width, 1};
      std::vector<int> packed(ap.outcome_idata.size());
      for (int64_t l = 0; l < Luse; ++l)
        for (int64_t k = 0; k < eff_width; ++k)
          packed[(size_t)lay.flat_at(l, k)] =
              ap.outcome_idata[(size_t)(l * eff_width + k)];
      ap.outcome_idata = std::move(packed);
    }
    // A wide shared operand tiled to the region's packing order: mode
    // {width, count, 1} tail-to-tail (row-major) or {count, width, 2}
    // each element repeated (column-major), matching the same
    // LaneLayout convention.
    const auto tile_wide_op = [&](int base) {
      Op rep;
      rep.opcode = OP_REP_MAT;
      rep.n_in = 1;
      rep.in[0] = base;
      rep.out = g.add_slot(Luse * eff_width, false);
      std::vector<int> ridata =
          layout_cols ? std::vector<int>{(int)Luse, (int)eff_width, 2}
                      : std::vector<int>{(int)eff_width, (int)Luse, 1};
      g.idata_pool.push_back(std::move(ridata));
      rep.idata = g.idata_pool.back().data();
      rep.n_idata = (int64_t)g.idata_pool.back().size();
      result.push_back(rep);
      return rep.out;
    };
    bool all_scalar = true;
    for (int j = 0; j < t.n_in; ++j) {
      switch (ap.ins[j].kind) {
        case InKind::kInvariant:
          op.in[j] = ap.ins[j].tile_wide ? tile_wide_op(resolve(t.in[j]))
                                         : resolve(t.in[j]);
          if (g.slots[t.in[j]].len != 1) all_scalar = false;
          break;
        case InKind::kLaneLocal:
          op.in[j] = pos_out[(size_t)ap.ins[j].producer_pos];
          if (ap.ins[j].tile_wide) op.in[j] = tile_wide_op(op.in[j]);
          if (g.slots[op.in[j]].len != 1) all_scalar = false;
          break;
        case InKind::kConstLanes:
          op.in[j] = packed_const(ap.ins[j], eff_width);
          all_scalar = false;
          break;
        case InKind::kBad:
          break;  // unreachable: classification succeeded
      }
    }
    // Swap the Luse lane terms for one replacement, at the first
    // lane's position (term_density and term_widen both end here).
    const auto swap_terms = [&](int new_term) {
      source.replace_terms(p, Luse, new_term, target_terms, term_set);
    };
    // The fused lpmf's outcome vector is the lanes' immediates.
    const auto attach_idata = [&](Op& o, std::vector<int> outcome) {
      if (outcome.empty()) return;
      g.idata_pool.push_back(std::move(outcome));
      o.idata = g.idata_pool.back().data();
      o.n_idata = (int64_t)g.idata_pool.back().size();
    };
    // Every lane computes the same scalar: emit it once and multiply by
    // L. Both callers are all-scalar term dispositions, whose positions
    // have no op consumers at all (the classifier refuses otherwise), so
    // pos_out here is bookkeeping nobody reads. Returns the new term.
    const auto scalar_times_lanes = [&](const Op& scalar) {
      result.push_back(scalar);  // scalar op, out = t.out
      Op mul;
      mul.opcode = OP_MUL;
      mul.n_in = 2;
      mul.in[0] = scalar.out;
      const int lc = g.add_slot(1, false);
      fills.emplace_back(lc, std::vector<double>{(double)Luse});
      mul.in[1] = lc;
      mul.out = g.add_slot(1, false);
      result.push_back(mul);
      pos_out[(size_t)p] = scalar.out;
      return mul.out;
    };
    if (ap.elt_density) {
      // One density op with variant bit 6: out[n] is lane n's lp (or,
      // when ap.rows > 1, out holds Luse*rows per-element lps that
      // OP_SUM_ROWS folds back to one lp per lane), read by the lanes'
      // (widened) consumers. An all-scalar real-arg density classified
      // as hoist instead, so a vector input or a per-lane outcome
      // exists here.
      op.variant = (uint8_t)(op.variant | 0x40u);
      op.out = g.add_slot(Luse * ap.rows, false);
      attach_idata(op, std::move(ap.outcome_idata));
      int result_slot = op.out;
      result.push_back(op);
      if (ap.rows > 1) {
        // The density's real args pack in the region's committed order.
        // OP_SUM_ROWS needs lane l's `rows` elements contiguous
        // (l*rows+k); row-major packing already has that, but
        // column-major (l + Luse*k) does not, so repack it first.
        if (layout_cols) {
          Op perm;
          perm.opcode = OP_GATHER;
          perm.n_in = 1;
          perm.in[0] = result_slot;
          perm.out = g.add_slot(Luse * ap.rows, false);
          std::vector<int> idx;
          idx.reserve((size_t)(Luse * ap.rows));
          for (int64_t l = 0; l < Luse; ++l)
            for (int64_t k = 0; k < ap.rows; ++k)
              idx.push_back((int)(l + Luse * k));
          g.idata_pool.push_back(std::move(idx));
          perm.idata = g.idata_pool.back().data();
          perm.n_idata = (int64_t)g.idata_pool.back().size();
          result.push_back(perm);
          result_slot = perm.out;
        }
        Op rows;
        rows.opcode = OP_SUM_ROWS;
        rows.n_in = 1;
        rows.in[0] = result_slot;
        rows.out = g.add_slot(Luse, false);
        attach_idata(rows, std::vector<int>{(int)ap.rows});
        result.push_back(rows);
        result_slot = rows.out;
      }
      pos_out[(size_t)p] = result_slot;
      continue;
    }
    if (ap.term_widen) {
      int term_slot;
      if (all_scalar) {
        // Every lane's term is the same scalar: one op, times L.
        term_slot = scalar_times_lanes(op);
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
        swap_terms(scalar_times_lanes(op));
        continue;
      }
      op.out = g.add_slot(1, false);
      attach_idata(op, std::move(ap.outcome_idata));
      swap_terms(op.out);
    } else {
      op.out = g.add_slot(Luse * ap.width, false);
    }
    pos_out[(size_t)p] = op.out;
    result.push_back(op);
  }
}

}  // namespace reroll_plan
}  // namespace detail
}  // namespace stanli
#endif
