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
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace stanli {
namespace {

constexpr int64_t kMinLanes = 4;
constexpr int kMaxPeriod = 32;
constexpr int kMaxClassifyAttempts = 6;

enum class InKind { kInvariant, kConstLanes, kLaneLocal, kBad };

struct PosIn {
  InKind kind = InKind::kBad;
  int producer_pos = -1;       // LANE_LOCAL: template position of producer
  std::vector<double> values;  // CONST_LANES: one value per lane
};

struct Pos {
  std::vector<PosIn> ins;
  bool index_elision = false;   // OP_INDEX, idata==lane, base len==lanes
  bool hoist = false;           // all inputs + idata invariant: emit once
  bool term_density = false;    // density, every lane's out a target term
  bool elt_density = false;     // density, every lane's out consumed only
                                //   inside its own lane -> variant bit 6,
                                //   out[n] = lane n's lp
  bool term_widen = false;      // widenable, every lane's out a target term
                                //   -> widen + OP_SUM_VEC, swap the terms
  int slice_start = -1;         // OP_INDEX over a contiguous window
  std::vector<int> gather_idx;  // OP_INDEX with a data-driven index
  int store_vec = -1;           // element write filling a window of this
  int store_start = 0;          //   vector, starting here,
  int store_stride = 1;         //   advancing by this much per lane
  bool store_written_after = false;  // someone writes the vector later
  std::vector<int> outcome_idata;    // lpmf: per-lane integer outcomes
};

bool is_element_store(const Op& op) {
  return (op.opcode == OP_SET_INDEX || op.opcode == OP_SET_INDEX_INPLACE) &&
         op.n_in == 2 && op.n_idata == 1 && op.out == op.in[0];
}

// Structural template match; idata may differ across lanes only for
// OP_INDEX (checked as a progression during classification).
bool ops_match(const Graph& g, const Op& a, const Op& b) {
  if (a.opcode != b.opcode || a.variant != b.variant || a.n_in != b.n_in ||
      a.out2 >= 0 || b.out2 >= 0 || a.opcode == OP_CHECK_STRUCTURED ||
      a.opcode == OP_CHECK_MATCHING_DIMS || a.opcode == OP_CHECK_LOWER ||
      a.opcode == OP_CHECK_UPPER || a.opcode == OP_CATEGORICAL ||
      a.opcode == OP_RNG || a.opcode == OP_PROD_VEC)
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
  if (has_op_trait(a.opcode, op_trait::kRerollIdataDensity))
    return a.n_idata == b.n_idata;
  if (a.n_idata != b.n_idata) return false;
  for (int64_t k = 0; k < a.n_idata; ++k)
    if (a.idata[k] != b.idata[k]) return false;
  return true;
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

RerollStats reroll(Graph& g,
                   std::vector<std::pair<int, std::vector<double>>>& fills,
                   std::vector<int>& target_terms,
                   const std::vector<int>& extra_roots) {
  RerollStats st;
  if (std::getenv("STANLI_NO_REROLL")) return st;

  st.regions +=
      fuse_log_sum_exp_rows(g, target_terms, extra_roots, st.row_steps);

  std::unordered_set<int> term_set(target_terms.begin(), target_terms.end());
  const auto is_candidate_op = [&](const Op& t) {
    const uint8_t traits = op_traits(t.opcode);
    return (traits & op_trait::kRerollAnyDensity) != 0 || is_element_store(t) ||
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

  // The dedup'd constant pool: slot -> value, for len-1 fills.
  std::unordered_map<int, double> const_val;
  for (const auto& f : fills)
    if (f.second.size() == 1) const_val.emplace(f.first, f.second[0]);

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
      if (v[mid] < x)
        lo = mid + 1;
      else
        hi = mid;
    }
    return v.begin() + (ptrdiff_t)lo;
  };
  // Is any entry of `v` in [lo, hi) not accepted by `ours`? `ours` names
  // the region's own ops, which are allowed to touch the slot.
  const auto any_in_range_but = [&](const std::vector<size_t>& v, size_t lo,
                                    size_t hi, auto&& ours) {
    for (auto it = first_at_or_after(v, lo); it != v.end() && *it < hi; ++it) {
      ++st.list_steps;
      if (!ours(*it)) return true;
    }
    return false;
  };
  // Is any entry of `v` at or after `x`?
  const auto any_at_or_after = [&](const std::vector<size_t>& v, size_t x) {
    return first_at_or_after(v, x) != v.end();
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
      for (int attempt = 0; attempt < kMaxClassifyAttempts && Luse >= kMinLanes;
           ++attempt) {
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
            prefix = std::min(prefix, std::max({br_inv, br_local, br_const}));
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
                any_at_or_after(uses[(size_t)o], i + ((size_t)l + 1) * P))
              br_internal = l;
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
                if (any_at_or_after(writers[(size_t)base], region_end))
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
          } else if (has_op_trait(t.opcode, op_trait::kRerollAnyDensity)) {
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
            } else if (has_op_trait(t.opcode, op_trait::kRerollIdataDensity) &&
                       t.n_idata != 1) {
              ok = false;
              prefix = 0;  // already a vector op; nothing to fuse
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
            if (br_term == Luse && br_internal == Luse) {
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
          if ((ap.term_density || ap.elt_density) &&
              has_op_trait(t.opcode, op_trait::kRerollIdataDensity)) {
            ap.outcome_idata.reserve((size_t)Luse);
            for (int64_t l = 0; l < Luse; ++l)
              ap.outcome_idata.push_back(op_at(p, l).idata[0]);
          }
          lane0_producer[t.out] = p;
        }
        if (ok && (any_term_density || any_store || any_elt_density ||
                   any_term_widen)) {
          classified = true;
          break;
        }
        if (ok) prefix = 0;                     // classifiable but useless
        if (prefix >= Luse) prefix = Luse - 1;  // guarantee progress
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
          // One density op with variant bit 6: out[n] is lane n's lp, read
          // by the lanes' (widened) consumers. An all-scalar real-arg
          // density classified as hoist instead, so a vector input or a
          // per-lane outcome exists here and the out is genuinely len-N.
          op.variant = (uint8_t)(op.variant | 0x40u);
          op.out = g.add_slot(Luse, false);
          attach_idata(op, std::move(ap.outcome_idata));
          pos_out[(size_t)p] = op.out;
          result.push_back(op);
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
  return st;
}

}  // namespace stanli
