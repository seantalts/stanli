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
// A lane may also be C elements wide. LaneLayout names the region's packing
// convention (row-major or column-major, whichever its first wide row op
// commits to): a SLICE or SLICE_STRIDED reading row `lane` of an invariant
// base, lanes covering every row, elides to the base the same way a plain
// OP_INDEX does; a row that does not cover the base, or an OP_GATHER whose
// lanes' indices are together one affine run, packs through one OP_SLICE or
// OP_GATHER instead; constants and lpmf outcomes pack in the region's
// convention; an invariant operand as wide as the row tiles via OP_REP_MAT;
// a density whose row-wide lp feeds another op folds back with one
// OP_SUM_ROWS per lane (repacked first when the convention is column-major,
// since OP_SUM_ROWS needs each lane's elements contiguous); and row stores
// covering every row make the fused value the container.
//
// Failed classifications report the longest still-classifiable lane
// prefix and retry with it. This is what handles block-structured data
// (rats_model: obs sorted time-major, so INDEX idata restarts 0..29 every
// time block): each block classifies as its own region and the scan
// resumes at the block boundary. Anything unclassifiable bails per-region,
// never per-model: cross-lane reads (parameter recurrences), partial or
// strided INDEX progressions, outputs escaping the lane, opcodes outside
// the vocabulary.
#include <stanli/builtin_registry.hpp>
#include <stanli/optable.hpp>
#include <stanli/reroll.hpp>

#include "pass_util.hpp"
#include "reroll_profile.hpp"
#include "reroll_plan.hpp"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace stanli {
namespace {

constexpr int64_t kMinLanes = detail::kMinLanes;
constexpr int kMaxPeriod = detail::kMaxPeriod;
constexpr int kMaxClassifyAttempts = 6;

using detail::reroll_plan::InKind;
using detail::reroll_plan::LaneLayout;
using detail::reroll_plan::Pos;
using detail::reroll_plan::PosIn;

bool is_row_read(const Op& op) {
  return op.n_in == 1 && ((op.opcode == OP_SLICE && op.n_idata == 1) ||
                          (op.opcode == OP_SLICE_STRIDED && op.n_idata == 2));
}

bool is_row_store(const Op& op) {
  if (op.n_in != 2) return false;
  switch (op.opcode) {
    case OP_SET_SLICE:
    case OP_SET_SLICE_INPLACE:
      return op.n_idata == 1;
    case OP_SET_SLICE_STRIDED:
    case OP_SET_SLICE_STRIDED_INPLACE:
      return op.n_idata == 2;
    default:
      return false;
  }
}

uint16_t inplace_form(uint16_t opcode) {
  if (opcode == OP_SET_SLICE) return OP_SET_SLICE_INPLACE;
  if (opcode == OP_SET_SLICE_STRIDED) return OP_SET_SLICE_STRIDED_INPLACE;
  return opcode;
}

bool is_strided(uint16_t opcode) {
  return opcode == OP_SLICE_STRIDED || opcode == OP_SET_SLICE_STRIDED ||
         opcode == OP_SET_SLICE_STRIDED_INPLACE;
}

bool two_int_groups(uint16_t opcode) {
  return opcode == OP_BINOMIAL_LPMF || opcode == OP_BINOMIAL_LOGIT_LPMF ||
         opcode == OP_BETA_BINOMIAL_LPMF;
}

// Structural template match; idata may differ across lanes only for
// OP_INDEX (checked as a progression during classification). A row store
// run starts with the functional write of the declaration and continues in
// place.
bool ops_match(const Graph& g, const Op& a, const Op& b,
               int64_t lane_distance) {
  if (a.opcode != b.opcode) {
    const bool maybe_row_store =
        (a.opcode == OP_SET_SLICE && b.opcode == OP_SET_SLICE_INPLACE) ||
        (a.opcode == OP_SET_SLICE_STRIDED &&
         b.opcode == OP_SET_SLICE_STRIDED_INPLACE);
    if (!maybe_row_store || !is_row_store(a)) return false;
  }
  if (a.variant != b.variant || a.n_in != b.n_in || a.out2 >= 0 ||
      b.out2 >= 0 || is_effectful_op(a.opcode) ||
      has_op_trait(a.opcode, op_trait::kVariantGrouped))
    return false;
  for (int j = 0; j < a.n_in; ++j)
    if (g.slots[a.in[j]].len != g.slots[b.in[j]].len) return false;
  if (g.slots[a.out].len != g.slots[b.out].len) return false;
  const bool a_row_store = is_row_store(a);
  if (a_row_store && a.out != b.out) return false;
  const bool a_row_read = a_row_store ? false : is_row_read(a);
  if (a_row_read && a.in[0] != b.in[0]) return false;
  // Element writes carry their destination index the same way reads do, so
  // the immediate is allowed to advance across lanes for both; lpmf lanes
  // carry their integer outcome there and fuse by concatenating them.
  if (a.opcode == OP_INDEX || a.opcode == OP_SET_INDEX ||
      a.opcode == OP_SET_INDEX_INPLACE)
    return a.n_idata == 1 && b.n_idata == 1;
  if (a_row_read) {
    if (a.n_idata != b.n_idata) return false;
    bool same = true;
    for (int64_t k = 0; k < a.n_idata; ++k)
      same = same && a.idata[k] == b.idata[k];
    if (same) return true;
    if (is_strided(a.opcode))
      return b.idata[0] == a.idata[0] + lane_distance &&
             b.idata[1] == a.idata[1];
    return b.idata[0] == a.idata[0] + lane_distance * g.slots[a.out].len;
  }
  if (a_row_store) {
    if (a.n_idata != b.n_idata) return false;
    if (is_strided(a.opcode))
      return b.idata[0] == a.idata[0] + lane_distance &&
             b.idata[1] == a.idata[1];
    return b.idata[0] == a.idata[0] + lane_distance * g.slots[a.in[1]].len;
  }
  // A gather's idata is an arbitrary index list, not a progression: any two
  // gathers of the same width off the same base are the same template, and
  // classification decides separately whether the lanes' concatenated
  // indices are a shape it can pack.
  if (a.opcode == OP_GATHER)
    return a.in[0] == b.in[0] && a.n_idata == b.n_idata;
  if (has_op_trait(a.opcode, op_trait::kRerollIdataDensity))
    return a.n_idata == b.n_idata;
  if (a.n_idata != b.n_idata) return false;
  for (int64_t k = 0; k < a.n_idata; ++k)
    if (a.idata[k] != b.idata[k]) return false;
  return true;
}

uint64_t mix_u64(uint64_t h, uint64_t v) {
  v += 0x9e3779b97f4a7c15ULL;
  v ^= v >> 30;
  v *= 0xbf58476d1ce4e5b9ULL;
  v ^= v >> 27;
  v *= 0x94d049bb133111ebULL;
  v ^= v >> 31;
  return (h ^ v) * 1099511628211ULL;
}

// A necessary condition for ops_match(g, a, b, L) at any lane distance L:
// every field ops_match requires identical between a and b, folded into one
// integer. Fields ops_match lets advance with the lane -- OP_INDEX and
// element-write idata, the row-read/row-store idata offset -- are left out.
uint64_t op_signature(const Graph& g, const Op& a) {
  uint64_t h = 1469598103934665603ULL;
  h = mix_u64(h, (uint64_t)inplace_form(a.opcode));
  h = mix_u64(h, (uint64_t)a.variant);
  h = mix_u64(h, (uint64_t)a.n_in);
  for (int j = 0; j < a.n_in; ++j)
    h = mix_u64(h, a.in[j] >= 0 ? (uint64_t)g.slots[a.in[j]].len : ~0ULL);
  h = mix_u64(h, a.out >= 0 ? (uint64_t)g.slots[a.out].len : ~0ULL);
  const bool row_store = is_row_store(a);
  const bool row_read = row_store ? false : is_row_read(a);
  const bool idx_family = a.opcode == OP_INDEX || a.opcode == OP_SET_INDEX ||
                          a.opcode == OP_SET_INDEX_INPLACE;
  if (row_store) {
    h = mix_u64(h, (uint64_t)a.out);
    h = mix_u64(h, (uint64_t)a.n_idata);
  } else if (row_read) {
    h = mix_u64(h, (uint64_t)a.in[0]);
    h = mix_u64(h, (uint64_t)a.n_idata);
  } else if (a.opcode == OP_GATHER) {
    h = mix_u64(h, (uint64_t)a.in[0]);
    h = mix_u64(h, (uint64_t)a.n_idata);
  } else if (!idx_family) {
    h = mix_u64(h, (uint64_t)a.n_idata);
    if (!has_op_trait(a.opcode, op_trait::kRerollIdataDensity))
      for (int64_t k = 0; k < a.n_idata; ++k)
        h = mix_u64(h, (uint64_t)(uint32_t)a.idata[k]);
  }
  return h;
}

// LDA's likelihood is a small inner loop nested in a much larger document
// loop:
//
//   gamma[k] = log(a[index_a[n,k]]) + log(b[index_b[n,k]]);
//   target += log_sum_exp(gamma);
//
// K=2 is below the generic reroller's four-lane threshold, and for K=5 the
// scalar LOG_SUM_EXP between rows prevents the outer loop from looking
// periodic. Recognize this one exact scalar grammar before ordinary rerolling
// and flatten the complete rows into two gathers, vector arithmetic, and one
// packed row reduction. This is deliberately not a second symbolic loop
// vectorizer: every removed producer, consumer, store index, and target term is
// proven here against the already concrete graph.
int fuse_log_sum_exp_rows(Graph& g, std::vector<int>& target_terms,
                          const std::vector<int>& extra_roots,
                          int64_t& row_steps) {
  if (g.ops.empty() || (int64_t)target_terms.size() < kMinLanes) return 0;

  // Most graphs have no row-store/LSE boundary at all. Keep their cost to one
  // cache-friendly scan and, in particular, do not defeat the generic
  // candidate fast path by building target sets or allocating dense ownership
  // arrays first. Target membership is proven by the exact parser below.
  bool maybe_rows = false;
  for (size_t u = 1; u < g.ops.size(); ++u) {
    ++row_steps;
    const Op& lse = g.ops[u];
    const Op& store = g.ops[u - 1];
    if (lse.opcode == OP_LOG_SUM_EXP && lse.n_in == 1 &&
        (store.opcode == OP_SET_INDEX ||
         store.opcode == OP_SET_INDEX_INPLACE) &&
        store.out == lse.in[0]) {
      maybe_rows = true;
      break;
    }
  }
  if (!maybe_rows) return 0;

  std::unordered_set<int> roots(extra_roots.begin(), extra_roots.end());
  if (g.result_slot >= 0) roots.insert(g.result_slot);
  std::unordered_set<int> term_set(target_terms.begin(), target_terms.end());

  std::unordered_map<int, int> term_count;
  for (int s : target_terms) ++term_count[s];

  // Exact ownership for slots the rewrite deletes. Reader/writer counts and
  // their last positions make reused/in-place hand-built graphs fail closed
  // too.
  std::vector<int64_t> read_count(g.slots.size(), 0);
  std::vector<int64_t> write_count(g.slots.size(), 0);
  std::vector<int64_t> last_reader(g.slots.size(), -1);
  std::vector<int64_t> last_writer(g.slots.size(), -1);
  for (size_t u = 0; u < g.ops.size(); ++u) {
    ++row_steps;
    const Op& op = g.ops[u];
    for (int j = 0; j < op.n_in; ++j) {
      if (op.in[j] < 0) continue;
      ++read_count[(size_t)op.in[j]];
      last_reader[(size_t)op.in[j]] = (int64_t)u;
    }
    const auto wrote = [&](int s) {
      if (s < 0) return;
      ++write_count[(size_t)s];
      last_writer[(size_t)s] = (int64_t)u;
    };
    wrote(op.out);
    wrote(op.out2);
  }

  struct Row {
    size_t begin = 0;
    size_t end = 0;
    int K = 0;
    int a_base = -1;
    int b_base = -1;
    int gamma_in = -1;
    int gamma_out = -1;
    int lse_out = -1;
    bool functional_start = false;
    std::vector<int> a_index;
    std::vector<int> b_index;
    std::vector<int> gamma_slots;
    std::vector<int> deleted_slots;
  };

  const auto valid_slot = [&](int s) {
    return s >= 0 && (size_t)s < g.slots.size();
  };
  const auto plain = [](const Op& op, uint16_t opcode, int n_in) {
    return op.opcode == opcode && op.variant == 0 && op.n_in == n_in &&
           op.n_idata == 0 && op.out2 < 0 && op.udata == nullptr;
  };
  const auto deleted_scalar = [&](int slot, size_t writer, size_t reader) {
    return valid_slot(slot) && g.slots[(size_t)slot].len == 1 &&
           !g.slots[(size_t)slot].is_param && !roots.count(slot) &&
           !term_set.count(slot) && write_count[(size_t)slot] == 1 &&
           last_writer[(size_t)slot] == (int64_t)writer &&
           read_count[(size_t)slot] == 1 &&
           last_reader[(size_t)slot] == (int64_t)reader;
  };

  const auto parse_row = [&](size_t start, int want_k, int want_a, int want_b,
                             Row& row) {
    ++row_steps;
    if (start + 7 > g.ops.size()) return false;
    // The first store reveals gamma's width, hence the number of six-op scalar
    // lanes before the row's LOG_SUM_EXP.
    const Op& first_store = g.ops[start + 5];
    if ((first_store.opcode != OP_SET_INDEX &&
         first_store.opcode != OP_SET_INDEX_INPLACE) ||
        first_store.n_in != 2 || first_store.n_idata != 1 ||
        first_store.out2 >= 0 || first_store.udata != nullptr ||
        !valid_slot(first_store.in[0]))
      return false;
    const int64_t k64 = g.slots[(size_t)first_store.in[0]].len;
    if (k64 <= 0 || k64 > std::numeric_limits<int>::max()) return false;
    const int K = (int)k64;
    if ((want_k > 0 && K != want_k) ||
        start + (size_t)6 * (size_t)K + 1 > g.ops.size())
      return false;

    row = Row{};
    row.begin = start;
    row.K = K;
    row.a_index.reserve((size_t)K);
    row.b_index.reserve((size_t)K);
    int gamma = first_store.in[0];
    row.gamma_in = gamma;
    row.functional_start = first_store.opcode == OP_SET_INDEX &&
                           first_store.out != first_store.in[0];
    row.gamma_slots.push_back(gamma);

    for (int k = 0; k < K; ++k) {
      row_steps += 6;
      const size_t p = start + (size_t)6 * (size_t)k;
      const Op& ai = g.ops[p];
      const Op& al = g.ops[p + 1];
      const Op& bi = g.ops[p + 2];
      const Op& bl = g.ops[p + 3];
      const Op& add = g.ops[p + 4];
      const Op& store = g.ops[p + 5];
      if (!plain(al, OP_LOGV, 1) || !plain(bl, OP_LOGV, 1) ||
          !plain(add, OP_ADD, 2) || ai.opcode != OP_INDEX ||
          bi.opcode != OP_INDEX || ai.variant != 0 || bi.variant != 0 ||
          ai.n_in != 1 || bi.n_in != 1 || ai.n_idata != 1 || bi.n_idata != 1 ||
          ai.out2 >= 0 || bi.out2 >= 0 || ai.udata != nullptr ||
          bi.udata != nullptr || !valid_slot(ai.in[0]) ||
          !valid_slot(bi.in[0]) || ai.idata[0] < 0 || bi.idata[0] < 0 ||
          ai.idata[0] >= g.slots[(size_t)ai.in[0]].len ||
          bi.idata[0] >= g.slots[(size_t)bi.in[0]].len || al.in[0] != ai.out ||
          bl.in[0] != bi.out || add.in[0] != al.out || add.in[1] != bl.out ||
          (store.opcode != OP_SET_INDEX &&
           store.opcode != OP_SET_INDEX_INPLACE) ||
          store.variant != 0 || store.n_in != 2 || store.n_idata != 1 ||
          store.out2 >= 0 || store.udata != nullptr || store.in[0] != gamma ||
          store.in[1] != add.out || store.idata[0] != k ||
          !valid_slot(store.out) || g.slots[(size_t)store.out].len != K ||
          (store.opcode == OP_SET_INDEX_INPLACE && store.out != gamma) ||
          !deleted_scalar(ai.out, p, p + 1) ||
          !deleted_scalar(al.out, p + 1, p + 4) ||
          !deleted_scalar(bi.out, p + 2, p + 3) ||
          !deleted_scalar(bl.out, p + 3, p + 4) ||
          !deleted_scalar(add.out, p + 4, p + 5))
        return false;

      if (k == 0) {
        row.a_base = ai.in[0];
        row.b_base = bi.in[0];
        if ((want_a >= 0 && row.a_base != want_a) ||
            (want_b >= 0 && row.b_base != want_b))
          return false;
      } else if (ai.in[0] != row.a_base || bi.in[0] != row.b_base) {
        return false;
      }
      row.a_index.push_back(ai.idata[0]);
      row.b_index.push_back(bi.idata[0]);
      row.deleted_slots.insert(row.deleted_slots.end(),
                               {ai.out, al.out, bi.out, bl.out, add.out});
      gamma = store.out;
      row.gamma_slots.push_back(gamma);
    }

    const size_t lp_pos = start + (size_t)6 * (size_t)K;
    ++row_steps;
    const Op& lse = g.ops[lp_pos];
    if (!plain(lse, OP_LOG_SUM_EXP, 1) || lse.in[0] != gamma ||
        !valid_slot(lse.out) || g.slots[(size_t)lse.out].len != 1 ||
        g.slots[(size_t)lse.out].is_param || roots.count(lse.out) ||
        !term_set.count(lse.out) || term_count[lse.out] != 1 ||
        write_count[(size_t)lse.out] != 1 ||
        last_writer[(size_t)lse.out] != (int64_t)lp_pos ||
        read_count[(size_t)lse.out] != 0)
      return false;
    row.gamma_out = gamma;
    row.lse_out = lse.out;
    row.deleted_slots.push_back(lse.out);
    row.end = lp_pos + 1;
    return true;
  };

  std::vector<Op> result;
  result.reserve(g.ops.size());
  int regions = 0;
  size_t i = 0;
  while (i < g.ops.size()) {
    Row first;
    if (!parse_row(i, -1, -1, -1, first)) {
      result.push_back(g.ops[i++]);
      continue;
    }

    std::vector<Row> rows;
    rows.push_back(std::move(first));
    std::unordered_set<int> run_gamma(rows[0].gamma_slots.begin(),
                                      rows[0].gamma_slots.end());
    while (true) {
      Row next;
      const Row& prev = rows.back();
      if (!parse_row(prev.end, prev.K, prev.a_base, prev.b_base, next)) break;

      // Lowering has two safe row-to-row forms. A declaration reused across
      // the outer loop continues the previous row's gamma chain, normally
      // entirely in place. An unrolled declaration starts each row from a
      // distinct scratch slot with a functional first write; because indices
      // 0..K-1 are then all overwritten before LOG_SUM_EXP, its old contents
      // are immaterial. Do not accept a different in-place buffer or a fresh
      // chain that aliases an earlier row: those are neither of the proven
      // forms and can hide cross-row state.
      const bool shared = next.gamma_in == prev.gamma_out;
      bool compatible = shared || next.functional_start;
      for (int s : next.gamma_slots)
        if (run_gamma.count(s) && (!shared || s != prev.gamma_out)) {
          compatible = false;
          break;
        }
      if (!compatible) break;

      run_gamma.insert(next.gamma_slots.begin(), next.gamma_slots.end());
      rows.push_back(std::move(next));
    }
    if ((int64_t)rows.size() < kMinLanes) {
      result.push_back(g.ops[i++]);
      continue;
    }

    const size_t batch_end = rows.back().end;
    std::unordered_set<int> gamma_slots;
    std::unordered_set<int> deleted_slots;
    for (const Row& row : rows) {
      gamma_slots.insert(row.gamma_slots.begin(), row.gamma_slots.end());
      deleted_slots.insert(row.deleted_slots.begin(), row.deleted_slots.end());
    }
    bool safe = true;
    for (int s : gamma_slots) {
      if (!valid_slot(s) || g.slots[(size_t)s].is_param || roots.count(s) ||
          term_set.count(s) || last_reader[(size_t)s] >= (int64_t)batch_end ||
          last_writer[(size_t)s] >= (int64_t)batch_end) {
        safe = false;
        break;
      }
    }
    // Hoisting all scalar reads into two leading gathers is only valid when
    // neither base is part of the mutable/deleted row state.
    if (gamma_slots.count(rows[0].a_base) ||
        gamma_slots.count(rows[0].b_base) ||
        deleted_slots.count(rows[0].a_base) ||
        deleted_slots.count(rows[0].b_base))
      safe = false;

    size_t term_at = target_terms.size();
    for (size_t t = 0; t < target_terms.size(); ++t)
      if (target_terms[t] == rows[0].lse_out) {
        term_at = t;
        break;
      }
    if (term_at + rows.size() > target_terms.size()) safe = false;
    for (size_t r = 0; safe && r < rows.size(); ++r)
      if (target_terms[term_at + r] != rows[r].lse_out) safe = false;

    if (!safe) {
      // A late safety refusal (an escaped gamma or interleaved target term)
      // applies to this whole packed-row run. Copy it once rather than
      // reparsing every suffix, which would turn a fail-closed path quadratic.
      result.insert(result.end(), g.ops.begin() + (ptrdiff_t)i,
                    g.ops.begin() + (ptrdiff_t)batch_end);
      i = batch_end;
      continue;
    }

    const int64_t total = (int64_t)rows.size() * rows[0].K;
    std::vector<int> a_index;
    std::vector<int> b_index;
    a_index.reserve((size_t)total);
    b_index.reserve((size_t)total);
    for (const Row& row : rows) {
      a_index.insert(a_index.end(), row.a_index.begin(), row.a_index.end());
      b_index.insert(b_index.end(), row.b_index.begin(), row.b_index.end());
    }
    const auto attach_idata = [&](Op& op, std::vector<int> idata) {
      g.idata_pool.push_back(std::move(idata));
      op.idata = g.idata_pool.back().data();
      op.n_idata = (int64_t)g.idata_pool.back().size();
    };
    const auto unary = [&](uint16_t opcode, int in, int64_t len) {
      Op op;
      op.opcode = opcode;
      op.n_in = 1;
      op.in[0] = in;
      op.out = g.add_slot(len, false);
      result.push_back(op);
      return op.out;
    };
    Op gather_a;
    gather_a.opcode = OP_GATHER;
    gather_a.n_in = 1;
    gather_a.in[0] = rows[0].a_base;
    gather_a.out = g.add_slot(total, false);
    attach_idata(gather_a, std::move(a_index));
    result.push_back(gather_a);
    const int log_a = unary(OP_LOGV, gather_a.out, total);

    Op gather_b;
    gather_b.opcode = OP_GATHER;
    gather_b.n_in = 1;
    gather_b.in[0] = rows[0].b_base;
    gather_b.out = g.add_slot(total, false);
    attach_idata(gather_b, std::move(b_index));
    result.push_back(gather_b);
    const int log_b = unary(OP_LOGV, gather_b.out, total);

    Op add;
    add.opcode = OP_ADD;
    add.n_in = 2;
    add.in[0] = log_a;
    add.in[1] = log_b;
    add.out = g.add_slot(total, false);
    result.push_back(add);

    Op lse;
    lse.opcode = OP_LOG_SUM_EXP_ROWS;
    lse.n_in = 1;
    lse.in[0] = add.out;
    lse.out = g.add_slot((int64_t)rows.size(), false);
    attach_idata(lse, std::vector<int>{rows[0].K});
    result.push_back(lse);

    Op sum;
    sum.opcode = OP_SUM_VEC;
    sum.n_in = 1;
    sum.in[0] = lse.out;
    sum.out = g.add_slot(1, false);
    result.push_back(sum);

    std::vector<int> next_terms;
    next_terms.reserve(target_terms.size() - rows.size() + 1);
    next_terms.insert(next_terms.end(), target_terms.begin(),
                      target_terms.begin() + (ptrdiff_t)term_at);
    next_terms.push_back(sum.out);
    next_terms.insert(next_terms.end(),
                      target_terms.begin() + (ptrdiff_t)(term_at + rows.size()),
                      target_terms.end());
    target_terms = std::move(next_terms);
    term_set.insert(sum.out);
    i = batch_end;
    ++regions;
  }
  g.ops = std::move(result);
  return regions;
}

}  // namespace

static RerollStats reroll_impl(
    Graph& g, std::vector<std::pair<int, std::vector<double>>>& fills,
    std::vector<int>& target_terms, const std::vector<int>& extra_roots,
    detail::RerollDispositionStats* dispositions) {
  RerollStats st;
  if (std::getenv("STANLI_NO_REROLL")) return st;

  const int packed_rows =
      fuse_log_sum_exp_rows(g, target_terms, extra_roots, st.row_steps);
  st.regions += packed_rows;
  if (dispositions) dispositions->packed_rows += packed_rows;

  std::unordered_set<int> term_set(target_terms.begin(), target_terms.end());
  const auto is_candidate_op = [&](const Op& t) {
    const uint8_t traits = op_traits(t.opcode);
    return (traits & op_trait::kRerollAnyDensity) != 0 || is_element_store(t) ||
           is_row_store(t) ||
           ((traits & op_trait::kRerollWidenable) != 0 &&
            term_set.count(t.out) != 0);
  };

  // Every profitable region must contain one of these ops in its first period.
  // Index the next one at or after every position, making the [i, i + P)
  // question below O(1) instead of rescanning up to P ops for every P. The
  // target set can only lose original outputs from a region the scan then skips
  // over, and gains only new result slots that are not outputs in g.ops, so the
  // index stays valid as rewrites advance through the original graph.
  const size_t no_candidate = g.ops.size();
  std::vector<size_t> next_candidate(no_candidate + 1, no_candidate);
  for (size_t u = g.ops.size(); u-- > 0;) {
    ++st.candidate_steps;
    next_candidate[u] = is_candidate_op(g.ops[u]) ? u : next_candidate[u + 1];
  }
  // If the whole graph contains none, no period can classify. Return before
  // allocating dense reader/writer lists.
  if (next_candidate[0] == no_candidate) return st;

  const std::unordered_map<int, double> const_val = scalar_constants(fills);

  // Consumers of each slot, by original op index. Ops only read slots
  // produced earlier, so indices stay valid as the scan rewrites disjoint
  // regions left to right. Indexed by slot id: every question below is
  // asked about a slot taken from an unrewritten g.ops entry, i.e. one
  // that already existed when the scan started, so the dense form is
  // enough and no lookup can miss.
  std::vector<std::vector<size_t>> uses(g.slots.size());
  for (size_t u = 0; u < g.ops.size(); ++u)
    for (int j = 0; j < g.ops[u].n_in; ++j)
      if (g.ops[u].in[j] >= 0) uses[(size_t)g.ops[u].in[j]].push_back(u);

  // Producers of each slot. The write-fusion rewrite below needs to know
  // that nothing else ever writes the vector it is about to take over.
  std::vector<std::vector<size_t>> writers(g.slots.size());
  for (size_t u = 0; u < g.ops.size(); ++u) {
    if (g.ops[u].out >= 0) writers[(size_t)g.ops[u].out].push_back(u);
    if (g.ops[u].out2 >= 0) writers[(size_t)g.ops[u].out2].push_back(u);
  }

  // Vector constants, by index into `fills`.
  std::unordered_map<int, size_t> vec_const;
  for (size_t f = 0; f < fills.size(); ++f) {
    const int s = fills[f].first;
    if (fills[f].second.size() > 1 && s >= 0 && (size_t)s < writers.size() &&
        writers[(size_t)s].empty())
      vec_const[s] = f;
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
  // Is any entry of `v` in [lo, hi) not accepted by `ours`? `ours` names
  // the region's own ops, which are allowed to touch the slot.
  const auto any_in_range_but = [&](const std::vector<size_t>& v, size_t lo,
                                    size_t hi, auto&& ours) {
    for (auto it = first_at_or_after(v, lo, st.list_steps);
         it != v.end() && *it < hi; ++it) {
      ++st.list_steps;
      if (!ours(*it)) return true;
    }
    return false;
  };

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
    return detail::reroll_plan::resolve_slot(renamed, s);
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

  // sig[u] is op_signature(g.ops[u]); period_fwd(P)[k] is the largest L
  // such that sig[k] == sig[k + P] == ... == sig[k + L * P], built once
  // per P and cached.
  const bool prefilter_off =
      std::getenv("STANLI_NO_REROLL_PREFILTER") != nullptr;
  std::vector<uint64_t> sig;
  if (!prefilter_off) {
    sig.resize(g.ops.size());
    for (size_t u = 0; u < g.ops.size(); ++u)
      sig[u] = op_signature(g, g.ops[u]);
  }
  std::vector<std::vector<int32_t>> period_fwd_cache((size_t)kMaxPeriod + 1);
  const auto period_fwd = [&](int P) -> const std::vector<int32_t>& {
    std::vector<int32_t>& fwd = period_fwd_cache[(size_t)P];
    if (fwd.empty() && !g.ops.empty()) {
      fwd.assign(g.ops.size(), 0);
      for (size_t k = g.ops.size(); k-- > 0;) {
        const size_t next = k + (size_t)P;
        fwd[k] =
            (next < g.ops.size() && sig[k] == sig[next]) ? fwd[next] + 1 : 0;
      }
    }
    return fwd;
  };

  std::vector<Op> result;
  result.reserve(g.ops.size());
  size_t i = 0;
  while (i < g.ops.size()) {
    bool rewrote = false;
    for (int P = 1; P <= kMaxPeriod && i + 2 * (size_t)P <= g.ops.size(); ++P) {
      if (i < retry_at[(size_t)P]) continue;
      // Cheap pre-check before any lane counting: a profitable region must
      // contain an allowlisted density (term or elementwise), a per-lane
      // element write (which fuses into one vector store), or a widenable
      // op whose out is a target term (log_mix lanes over already-vector
      // lps: the region is INDEX/INDEX/LOG_MIX with no density at all).
      ++st.candidate_steps;
      if (next_candidate[i] >= i + (size_t)P) continue;

      // Count template-matching lanes.
      int64_t Lcap = std::numeric_limits<int64_t>::max();
      if (!prefilter_off) {
        const std::vector<int32_t>& fwd = period_fwd(P);
        Lcap = fwd[i];
        for (int p = 1; p < P && Lcap + 1 >= kMinLanes; ++p)
          Lcap = std::min(Lcap, (int64_t)fwd[i + (size_t)p]);
        if (Lcap + 1 < kMinLanes) continue;
        Lcap += 1;
      }
      int64_t L = 1;
      while (L <= Lcap && i + ((size_t)L + 1) * P <= g.ops.size()) {
        bool match = true;
        for (int p = 0; p < P && match; ++p)
          match = ops_match(g, g.ops[i + p], g.ops[i + (size_t)L * P + p], L);
        if (!match) break;
        ++L;
      }
      if (L < kMinLanes) continue;

      const auto op_at = [&](int p, int64_t l) -> const Op& {
        return g.ops[i + (size_t)l * P + p];
      };

      const auto rigid_diverges = [&](int64_t base) {
        for (int p = 0; p < P; ++p) {
          const Op& t0 = op_at(p, base);
          if (t0.opcode == OP_INDEX || is_element_store(t0) ||
              is_row_store(t0) || is_row_read(t0) ||
              has_op_trait(t0.opcode, op_trait::kRerollAnyDensity) ||
              has_op_trait(t0.opcode, op_trait::kRerollWidenable))
            continue;
          const Op& t1 = op_at(p, base + 1);
          for (int j = 0; j < t0.n_in; ++j)
            if (t0.in[j] != t1.in[j]) return true;
        }
        return false;
      };
      const bool doomed = rigid_diverges(0);
      const bool doomed_next = doomed && L > kMinLanes && rigid_diverges(1);

      // ---- classify, shrinking to the reported prefix on failure ----
      detail::reroll_plan::CandidatePlan plan;
      std::vector<Pos>& pos = plan.positions;
      bool layout_set = false;  // has the region committed to a convention
      bool& layout_cols = plan.column_major;  // column-major, once committed
      int64_t Luse = doomed ? 0 : L;
      using SelectedPlan =
          detail::reroll_plan::SelectedPlan<detail::reroll_plan::GraphSource>;
      std::optional<SelectedPlan> selected;
      for (int attempt = 0;
           !doomed && attempt < kMaxClassifyAttempts && Luse >= kMinLanes;
           ++attempt) {
        int64_t prefix = Luse;
        pos.assign((size_t)P, Pos{});
        std::unordered_map<int, int> lane0_producer;
        bool ok = true;
        bool any_term_density = false;
        bool any_store = false;
        bool any_elt_density = false;
        bool any_term_widen = false;
        layout_set = false;
        const size_t region_end = i + (size_t)P * (size_t)Luse;
        const auto row_operands_ok = [&](Pos& ap, const Op& t) {
          for (int j = 0; j < t.n_in; ++j) {
            PosIn& in = ap.ins[j];
            switch (in.kind) {
              case InKind::kInvariant:
                if (g.slots[t.in[j]].len == 1) break;
                if (g.slots[t.in[j]].len == ap.width) {
                  in.tile_wide = true;
                  break;
                }
                return false;
              case InKind::kLaneLocal: {
                const Pos& prod = pos[(size_t)in.producer_pos];
                if (prod.hoist) {
                  const int64_t len =
                      g.slots[op_at(in.producer_pos, 0).out].len;
                  if (len == 1) break;
                  if (len == ap.width) {
                    in.tile_wide = true;
                    break;
                  }
                  return false;
                } else if (prod.width != ap.width) {
                  return false;
                }
                break;
              }
              case InKind::kConstLanes:
                if (in.width != 1 && in.width != ap.width) return false;
                break;
              case InKind::kBad:
                return false;
            }
          }
          return true;
        };
        for (int p = 0; p < P; ++p) {
          const Op& t = op_at(p, 0);
          Pos& ap = pos[p];
          ap.ins.resize(t.n_in);
          bool all_inputs_invariant = true;
          for (int j = 0; j < t.n_in; ++j) {
            if (j == 0 && is_row_store(t)) {
              ap.ins[j].kind = InKind::kInvariant;
              continue;
            }
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
            const int64_t cw = g.slots[t.in[j]].len;
            std::vector<double> vals;
            vals.reserve((size_t)(Luse * cw));
            for (int64_t l = 0; l < Luse; ++l) {
              const int s = op_at(p, l).in[j];
              if (cw == 1) {
                auto cit = const_val.find(s);
                if (cit == const_val.end()) {
                  br_const = l;
                  break;
                }
                vals.push_back(cit->second);
              } else {
                auto vit = vec_const.find(s);
                if (vit == vec_const.end() ||
                    (int64_t)fills[vit->second].second.size() != cw) {
                  br_const = l;
                  break;
                }
                const std::vector<double>& row = fills[vit->second].second;
                vals.insert(vals.end(), row.begin(), row.end());
              }
            }
            if (br_const == Luse) {
              ap.ins[j].kind = InKind::kConstLanes;
              ap.ins[j].values = std::move(vals);
              ap.ins[j].width = cw;
              continue;
            }
            ok = false;
            prefix = std::min(prefix, std::max({br_inv, br_local, br_const}));
          }
          for (int j = 0; j < t.n_in; ++j) {
            const PosIn& in = ap.ins[j];
            if (in.kind == InKind::kConstLanes)
              ap.width = std::max(ap.width, in.width);
            else if (in.kind == InKind::kLaneLocal &&
                     !pos[(size_t)in.producer_pos].hoist)
              ap.width = std::max(ap.width, pos[(size_t)in.producer_pos].width);
          }

          // Output discipline prefixes. A lane's out may be consumed only
          // by later ops of its own lane instance; density outs may
          // instead be target terms (with no op consumers at all).
          int64_t br_term = Luse;      // lanes whose out IS a term
          int64_t br_nonterm = Luse;   // lanes whose out is NOT a term
          int64_t br_internal = Luse;  // lanes whose out does not escape
          for (int64_t l = 0; l < Luse; ++l) {
            const int o = op_at(p, l).out;
            const bool is_term = term_set.count(o) != 0;
            if (!is_term && br_term == Luse) br_term = l;
            if (is_term && br_nonterm == Luse) br_nonterm = l;
            if (br_internal == Luse && root_set.count(o) != 0) br_internal = l;
            // A use before the lane's own position would mean an op reading
            // a slot written after it, which the emission order forbids for
            // every position whose br_internal is read: the one exception,
            // an element store's repeatedly written vector, is classified by
            // the is_element_store arm, which never looks at br_internal.
            // So "escapes the lane" is exactly "used at or past the lane
            // end", which is one binary search instead of a whole scan.
            if (br_internal == Luse &&
                any_at_or_after(uses[(size_t)o], i + ((size_t)l + 1) * P,
                                st.list_steps))
              br_internal = l;
          }

          const bool idata_density =
              has_op_trait(t.opcode, op_trait::kRerollIdataDensity);
          bool row_idata_varies = false;
          if (is_row_read(t))
            for (int64_t l = 1; l < Luse && !row_idata_varies; ++l)
              for (int64_t k = 0; k < t.n_idata; ++k)
                if (op_at(p, l).idata[k] != t.idata[k]) {
                  row_idata_varies = true;
                  break;
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
              ap.read.kind = LaneLayout::Kind::kElide;  // whole base, in order
            } else if (br_iinv == Luse) {
              ap.hoist = true;  // same element every lane
            } else if (br_run == Luse && t.idata[0] + Luse <= blen) {
              ap.read.kind = LaneLayout::Kind::kSlice;  // contiguous window
              ap.read.offset = t.idata[0];
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
              if (prod.read.kind == LaneLayout::Kind::kElide) {
                const int base = op_at(ap.ins[1].producer_pos, 0).in[0];
                if (any_at_or_after(writers[(size_t)base], region_end,
                                    st.list_steps))
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
              const std::vector<size_t>& vec_uses = uses[(size_t)vec];
              const std::vector<size_t>& vec_writers = writers[(size_t)vec];
              if (any_in_range_but(vec_uses, i, region_end, in_region_write) ||
                  any_in_range_but(vec_writers, i, region_end, in_region_write))
                clean = false;
              if (any_at_or_after(vec_writers, region_end, st.list_steps))
                written_after = true;
            }
            if (!clean || br_run < Luse) {
              ok = false;
              prefix = std::min(prefix, clean ? br_run : (int64_t)0);
            } else {
              ap.store_vec = vec;
              ap.store_src = vec;
              ap.store_start = (int)t.idata[0];
              ap.store_stride = (int)stride;
              ap.store_written_after = written_after;
              any_store = true;
            }
          } else if (is_row_store(t)) {
            // `p[j, :] = ...` under an unrolled loop: lane 0 writes the
            // declaration functionally, later lanes in place.
            const int vec = t.out;
            const bool strided = is_strided(t.opcode);
            const int64_t w = g.slots[t.in[1]].len;
            const int64_t blen = g.slots[vec].len;
            const int64_t rows = strided ? t.idata[1] : Luse;
            int64_t br_row = Luse;
            bool shape = true;
            for (int64_t l = 0; l < Luse; ++l) {
              const Op& o = op_at(p, l);
              if (o.out != vec || (l > 0 && o.in[0] != vec)) shape = false;
              const bool row_l = strided ? o.idata[0] == l && o.idata[1] == rows
                                         : o.idata[0] == l * w;
              if (!row_l && br_row == Luse) br_row = l;
            }
            const bool covering =
                br_row == Luse && Luse == rows && blen == rows * w;
            bool clean = shape && covering && !root_set.count(vec) &&
                         !term_set.count(vec);
            if (clean && w != 1) {
              if (layout_set)
                clean = layout_cols == strided;
              else {
                layout_set = true;
                layout_cols = strided;
              }
            }
            const PosIn& val = ap.ins[1];
            if (val.kind == InKind::kLaneLocal) {
              const Pos& prod = pos[(size_t)val.producer_pos];
              if (prod.hoist || prod.width != w) clean = false;
            } else if (val.kind != InKind::kConstLanes || val.width != w) {
              clean = false;
            }
            bool written_after = false;
            if (clean && val.kind == InKind::kLaneLocal) {
              const Pos& prod = pos[(size_t)val.producer_pos];
              if (prod.read.kind == LaneLayout::Kind::kElide) {
                const int base = op_at(val.producer_pos, 0).in[0];
                if (any_at_or_after(writers[(size_t)base], region_end,
                                    st.list_steps))
                  written_after = true;
              }
            }
            if (clean) {
              auto in_region_write = [&](size_t u) {
                return u >= i && u < region_end &&
                       (u - i) % (size_t)P == (size_t)p;
              };
              const std::vector<size_t>& vec_uses = uses[(size_t)vec];
              const std::vector<size_t>& vec_writers = writers[(size_t)vec];
              if (any_in_range_but(vec_uses, i, region_end, in_region_write) ||
                  any_in_range_but(vec_writers, i, region_end, in_region_write))
                clean = false;
              if (any_at_or_after(vec_writers, region_end, st.list_steps))
                written_after = true;
            }
            if (!clean) {
              ok = false;
              prefix = std::min(prefix,
                                shape && br_row < Luse ? br_row : (int64_t)0);
            } else {
              ap.store_vec = vec;
              ap.store_src = t.in[0];
              ap.row_store = true;
              ap.store_written_after = written_after;
              any_store = true;
            }
          } else if (row_idata_varies) {
            // Row `lane` of an invariant base, lanes covering every row:
            // the fused consumer reads the base in place.
            const bool strided = t.opcode == OP_SLICE_STRIDED;
            const int64_t w = g.slots[t.out].len;
            const int64_t blen = g.slots[t.in[0]].len;
            const int64_t rows = strided ? t.idata[1] : Luse;
            const int64_t io_ok = std::min(br_internal, br_nonterm);
            int64_t br_row = Luse;
            for (int64_t l = 0; l < Luse; ++l) {
              const Op& o = op_at(p, l);
              const bool row_l = strided ? o.idata[0] == l && o.idata[1] == rows
                                         : o.idata[0] == l * w;
              if (!row_l) {
                br_row = l;
                break;
              }
            }
            if (ap.ins[0].kind != InKind::kInvariant || io_ok < Luse) {
              ok = false;
              prefix = std::min(prefix, io_ok);
            } else if (br_row == Luse && Luse == rows && blen == rows * w) {
              bool layout_ok = w == 1;
              if (!layout_ok) {
                if (layout_set) {
                  layout_ok = layout_cols == strided;
                } else {
                  layout_set = true;
                  layout_cols = strided;
                  layout_ok = true;
                }
              }
              if (layout_ok) {
                ap.read.kind = LaneLayout::Kind::kElide;
                ap.width = w;
              } else {
                ok = false;
                prefix = std::min(prefix, br_row < Luse ? br_row : (int64_t)0);
              }
            } else {
              ok = false;
              prefix = std::min(prefix, br_row < Luse ? br_row : (int64_t)0);
            }
          } else if (t.opcode == OP_GATHER && t.n_in == 1) {
            // A per-lane gather of an invariant base: lowering's read-side
            // selector ladder does not cover a fixed index alongside a
            // range on another axis (`m[i, 2:K]`), so a partial row falls
            // to a plain per-lane gather instead of a strided window. Safe
            // to pack only when every lane's gathered elements together are
            // an exact permutation of one contiguous span of the base:
            // sort the concatenation and classify it with the same
            // primitive lowering's own selector classifier uses. A repeated
            // or non-contiguous union is not this disposition; nothing else
            // here combines OP_GATHER lanes into one op yet.
            const int64_t io_ok = std::min(br_internal, br_nonterm);
            if (ap.ins[0].kind != InKind::kInvariant || io_ok < Luse) {
              ok = false;
              prefix = std::min(prefix, io_ok);
            } else {
              std::vector<int64_t> offsets;
              offsets.reserve((size_t)(Luse * t.n_idata));
              for (int64_t l = 0; l < Luse; ++l) {
                const Op& o = op_at(p, l);
                for (int64_t k = 0; k < o.n_idata; ++k)
                  offsets.push_back(o.idata[k]);
              }
              std::sort(offsets.begin(), offsets.end());
              const FlatOffsetRun run = classify_flat_offsets(offsets);
              if (run.kind == BuiltinSliceMap::Kind::Contiguous) {
                ap.read.kind = LaneLayout::Kind::kSlice;
                ap.read.offset = run.offset;
                ap.width = t.n_idata;
              } else {
                ok = false;
                prefix = 0;  // not a contiguous permutation: not ours to pack
              }
            }
          } else if (has_op_trait(t.opcode, op_trait::kRerollAnyDensity)) {
            // Two fusable dispositions: every lane's out IS a target term
            // (one summed vector density), or NO lane's out is a term and
            // each is consumed only inside its own lane (one elementwise
            // density, variant bit 6 -- the log_mix/log_sum_exp mixture
            // idiom). Mixed lanes or escaping outputs bound the prefix.
            const bool all_terms = br_term == Luse;
            const bool no_terms = br_nonterm == Luse;
            const bool row_outcomes = idata_density && ap.width > 1 &&
                                      t.n_idata == ap.width &&
                                      !two_int_groups(t.opcode);
            if (br_internal < Luse || (!all_terms && !no_terms)) {
              ok = false;
              prefix = std::min(
                  prefix, std::min(br_internal, std::max(br_term, br_nonterm)));
            } else if (idata_density && t.n_idata != 1 && !row_outcomes) {
              // More than one immediate: either already a vector op, or one
              // of the binomials, whose two integer groups only partition.cpp
              // concatenates. One lane still stands for all of them when the
              // real inputs and the immediates alike are lane-invariant.
              int64_t br_iinv = Luse;
              for (int64_t l = 1; l < Luse && br_iinv == Luse; ++l)
                for (int64_t k = 0; k < t.n_idata; ++k)
                  if (op_at(p, l).idata[k] != t.idata[k]) {
                    br_iinv = l;
                    break;
                  }
              if (!all_terms && all_inputs_invariant && br_iinv == Luse) {
                ap.hoist = true;
              } else {
                ok = false;
                prefix = 0;
              }
            } else if (ap.width > 1 && ((idata_density && !row_outcomes) ||
                                        !row_operands_ok(ap, t))) {
              ok = false;
              prefix = 0;
            } else if (all_terms) {
              if (!uses[(size_t)t.out].empty()) {
                ok = false;
                prefix = 0;  // a term that is also an op input
              } else if (all_inputs_invariant &&
                         !has_op_trait(t.opcode,
                                       op_trait::kRerollIdataDensity)) {
                // L identical lanes (the const pool dedup'd even the data
                // argument): the "fused" density would compute one lane's
                // lp where the target owes L of them.
                ok = false;
                prefix = 0;
              } else {
                ap.term_density = true;
                any_term_density = true;
              }
            } else if (all_inputs_invariant &&
                       !has_op_trait(t.opcode, op_trait::kRerollIdataDensity)) {
              // Every lane computes the same scalar lp: keep ONE scalar op
              // and let the lanes' consumers broadcast it. Widening scalar
              // inputs into a len-N out is the losscurve hazard.
              ap.hoist = true;
            } else {
              ap.elt_density = true;
              any_elt_density = true;
              if (ap.width > 1) {
                ap.rows = ap.width;
                ap.width = 1;
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
          } else if (has_op_trait(t.opcode, op_trait::kRerollWidenable)) {
            if (ap.width > 1 &&
                (g.slots[t.out].len != ap.width || !row_operands_ok(ap, t))) {
              ok = false;
              prefix = 0;
            } else if (br_term == Luse && br_internal == Luse) {
              // Every lane's out is a target term (log_mix under
              // `target +=`): widen the op, SUM_VEC the lanes, and swap
              // the N terms for the sum.
              if (!uses[(size_t)t.out].empty()) {
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
          // The fused lpmf's outcome vector is the lanes' immediates. Only
          // the two fusing density dispositions need it; the hoist arm
          // above excludes idata-outcome densities.
          if ((ap.term_density || ap.elt_density) && idata_density) {
            ap.outcome_idata.reserve((size_t)(Luse * t.n_idata));
            for (int64_t l = 0; l < Luse; ++l)
              for (int64_t k = 0; k < t.n_idata; ++k)
                ap.outcome_idata.push_back(op_at(p, l).idata[k]);
          }
          lane0_producer[t.out] = p;
        }
        if (ok && (any_term_density || any_store || any_elt_density ||
                   any_term_widen)) {
          // Price the fused form against staying scalar, in partition.cpp's
          // currencies: kLaneOpCost per graph-op dispatch this region would
          // eliminate, against kLaneOpCost per op it introduces plus
          // kLaneDensityElem-scale per-element cost wherever a position
          // actually copies or scatters elements (a slice, a gather, or a
          // density with no native elementwise form), plus a charge for
          // lanes CSE would already have merged into fewer than Luse ops.
          // Elision, hoisting and a plain vector op or density call are not
          // charged beyond their one dispatch: the scalar path would pay
          // the same per-element work in Luse separate calls, so only the
          // eliminated dispatches and the genuine copies are the region's
          // net win.
          plan.lanes = Luse;
          selected = SelectedPlan::select(
              g, plan, detail::reroll_plan::GraphSource{g.ops.data() + i, P});
          if (selected) break;
          ok = false;
        }
        if (ok) prefix = 0;                     // classifiable but useless
        if (prefix >= Luse) prefix = Luse - 1;  // guarantee progress
        Luse = prefix;
      }

      if (!selected) {
        // Bookkeeping. Soft failures (positive prefix) re-attempt at the
        // reported boundary; hard failures (prefix 0) get one second
        // chance one lane in, then the whole run is skipped.
        const size_t run_end = i + (size_t)P * (size_t)L;
        if (Luse > 0) {
          retry_at[(size_t)P] = i + (size_t)P * (size_t)Luse;
          hard_failed[(size_t)P] = false;
        } else if (hard_failed[(size_t)P] && i < fail_end[(size_t)P]) {
          retry_at[(size_t)P] = fail_end[(size_t)P];
        } else if (doomed_next) {
          fail_end[(size_t)P] = run_end;
          hard_failed[(size_t)P] = true;
          retry_at[(size_t)P] = run_end;
        } else {
          fail_end[(size_t)P] = run_end;
          hard_failed[(size_t)P] = true;
          retry_at[(size_t)P] = i + (size_t)P;
        }
        continue;
      }

      // Emit only the accepted plan; classification and pricing above have
      // not changed graph storage, targets or lazy renaming.
      std::move(*selected).emit(g, fills, target_terms, term_set, result,
                                renamed);
      i += (size_t)P * (size_t)Luse;
      ++st.regions;
      if (dispositions) {
        for (const Pos& committed : selected->description().positions) {
          dispositions->term_density += committed.term_density;
          dispositions->element_density += committed.elt_density;
          dispositions->term_widen += committed.term_widen;
          dispositions->element_store += committed.store_vec >= 0;
        }
      }
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
  return st;
}

RerollStats reroll(Graph& g,
                   std::vector<std::pair<int, std::vector<double>>>& fills,
                   std::vector<int>& target_terms,
                   const std::vector<int>& extra_roots) {
  return reroll_impl(g, fills, target_terms, extra_roots, nullptr);
}

namespace detail {

ProfiledRerollStats reroll_profiled(
    Graph& g, std::vector<std::pair<int, std::vector<double>>>& fills,
    std::vector<int>& target_terms, const std::vector<int>& extra_roots) {
  ProfiledRerollStats result;
  result.work =
      reroll_impl(g, fills, target_terms, extra_roots, &result.dispositions);
  return result;
}

SignatureCheckResult check_signature_soundness(const Graph& g, int64_t window) {
  SignatureCheckResult result;
  std::vector<uint64_t> sig(g.ops.size());
  for (size_t u = 0; u < g.ops.size(); ++u) sig[u] = op_signature(g, g.ops[u]);
  for (size_t a = 0; a < g.ops.size(); ++a) {
    const size_t hi = std::min(g.ops.size(), a + (size_t)window + 1);
    for (size_t b = a + 1; b < hi; ++b) {
      const int64_t direct = (int64_t)(b - a);
      for (int64_t dist : {direct, (int64_t)1, kMinLanes}) {
        ++result.pairs_checked;
        if (sig[a] != sig[b] && ops_match(g, g.ops[a], g.ops[b], dist))
          ++result.violations;
      }
    }
  }
  return result;
}

}  // namespace detail

}  // namespace stanli
