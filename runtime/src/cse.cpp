// Common-subexpression elimination.
//
// The unrolled M*_model family evaluates one Bernoulli term per capture
// occasion per individual, and for most of them the arguments are the same
// slot: Mh_model emits 685 bit-identical BERNOULLI_LPMF ops, Mb_model 786,
// and each one is a full kernel call plus a tape entry on every leapfrog
// step. Reroll leaves them alone (they are target terms with no op consumer),
// so nothing upstream removes the recomputation.
//
// This is textbook local value numbering, made safe for a graph whose slots
// are mutable buffers rather than SSA values:
//
//   - every write bumps a per-slot version, and the key carries the version
//     of each input, so two ops separated by a store to something they read
//     are different computations,
//   - only an op whose output slot is written exactly once in the whole graph
//     can be the survivor, which is what excludes the destructive update
//     chains: a slot a later SET_*_INPLACE mutates has two writers,
//   - effectful and stateful opcodes never merge, and neither does anything
//     carrying an opaque payload the key cannot compare.
//
// Renaming is lazy: ops are in evaluation order, so resolving each op's
// inputs through the map as the single forward pass reaches it is enough,
// and chains collapse in that same pass. Rescanning the tail after each
// merge would be quadratic in the op count.
// Active outputs instead retain their slot identity and reverse callback.
// Only their primal and read-only forward scratch are shared: summing seeds
// before a nonlinear pullback would change floating-point accumulation.
// The exception is an active op whose seed is a whole number: one that only
// the target reads, which seeds each term with 1, or that reaches the target
// only through scalar additions, which pass a seed on unchanged. Adding whole
// seeds is exact, so k such copies merge into one op with one pullback, and
// a term listed k times becomes that term multiplied by k: one rounding
// where adding the copies took k - 1. The zeros of a brms hurdle model each
// add bernoulli_lpmf(1 | hu), 5,308 identical terms in one 12,000-row model,
// and each of its other rows adds bernoulli_lpmf(0 | hu) to its own term.
//
// The same model passes each zero's element of mu to a function branch that
// never reads it, and each merged copy leaves the copies that computed its
// arguments unread. Static reads and copies that nothing reads are dropped.
// No other unused op is: it might still reject.
#include <stanli/debug_switch.hpp>
#include <stanli/cse.hpp>
#include <stanli/optable.hpp>

#include "pass_util.hpp"

#include <cstdlib>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace stanli {
namespace {

// The shared effect blocklist, plus the opcodes holding structure this pass
// cannot compare: a reduction whose variant selects its arithmetic grouping,
// a compiled region, a solver.
bool never_merge(uint16_t oc) {
  return is_effectful_op(oc) || has_op_trait(oc, op_trait::kVariantGrouped) ||
         oc == OP_ISLAND || oc == OP_ODE || oc == OP_DAE ||
         oc == OP_ODE_ADJOINT;
}

// Whether an op whose value nothing reads can go. Lowering checked a static
// read's immediates against the base's static length, so it cannot fail at
// run time. A copy runs no forward of its own: its value, and any rejection,
// come from the op it shares a primal with, which stays.
bool droppable_if_unread(const Op& op) {
  if (op.out2 >= 0 || op.udata != nullptr) return false;
  if (op.primal_source >= 0 && op.primal_source != op.out) return true;
  switch (op.opcode) {
    case OP_INDEX:
    case OP_SLICE:
    case OP_SLICE_STRIDED:
    case OP_GATHER:
      return op.n_in == 1 && !op.dyn_lengths && op.dyn_extent_in < 0;
    default:
      return false;
  }
}

// Whether each slot's adjoint is a whole number: the slot is written once,
// and every read of it is a target term or an operand of a scalar ADD or
// ADD_N whose own adjoint is whole. Readers follow their writer, so one
// newest-first sweep settles each slot before its writer is reached.
std::vector<char> whole_seeds(const Graph& g,
                              const std::vector<int>& target_terms,
                              const std::unordered_set<int>& keep,
                              const std::vector<int>& writes) {
  std::vector<char> read(g.slots.size(), 0), other(g.slots.size(), 0);
  for (int t : target_terms) read[(size_t)t] = 1;
  std::vector<char> whole(g.slots.size(), 0);
  for (size_t i = g.ops.size(); i-- > 0;) {
    const Op& op = g.ops[i];
    if (op.out >= 0) {
      const size_t o = (size_t)op.out;
      whole[o] = read[o] && !other[o] && writes[o] == 1 && !keep.count(op.out);
    }
    const bool passes =
        (op.opcode == OP_ADD_N || (op.opcode == OP_ADD && op.variant == 0)) &&
        op.out >= 0 && op.out2 < 0 && op.udata == nullptr &&
        g.slots[(size_t)op.out].len == 1 && whole[(size_t)op.out];
    for (int j = 0; j < op.n_in; ++j) {
      if (op.in[j] < 0) continue;
      read[(size_t)op.in[j]] = 1;
      if (!passes) other[(size_t)op.in[j]] = 1;
    }
    if (op.primal_source >= 0) other[(size_t)op.primal_source] = 1;
  }
  return whole;
}

// Replaces each run of k copies of one term with a single term times k, at
// the position of its first copy.
void scale_repeated_terms(Graph& g, Fills& fills,
                          std::vector<int>& target_terms) {
  std::unordered_map<int, int> copies;
  for (int t : target_terms) ++copies[t];
  if (copies.size() == target_terms.size()) return;
  std::vector<int> scaled;
  scaled.reserve(copies.size());
  for (int t : target_terms) {
    int& k = copies[t];
    if (k == 1) scaled.push_back(t);
    if (k <= 1) continue;
    const int count = g.add_slot(1, false);
    fills.emplace_back(count, std::vector<double>{(double)k});
    const int term = g.add_slot(1, false);
    g.add_op(OP_MUL, {t, count}, term);
    scaled.push_back(term);
    k = 0;
  }
  target_terms = std::move(scaled);
}

// Drops the ops droppable_if_unread allows whose value no op, root or term
// reads, newest first so an op that only fed a dropped one goes too.
int drop_unread(Graph& g, const std::vector<int>& target_terms,
                const std::unordered_set<int>& keep) {
  std::vector<int> reads(g.slots.size(), 0), writes(g.slots.size(), 0);
  const auto for_each_read = [](const Op& op, auto&& f) {
    for (int j = 0; j < op.n_in; ++j)
      if (op.in[j] >= 0) f(op.in[j]);
    if (op.primal_source >= 0 && op.primal_source != op.out)
      f(op.primal_source);
  };
  for (const Op& op : g.ops) {
    for_each_read(op, [&](int s) { ++reads[(size_t)s]; });
    if (op.out >= 0) ++writes[(size_t)op.out];
    if (op.out2 >= 0) ++writes[(size_t)op.out2];
  }
  for (int t : target_terms) ++reads[(size_t)t];
  std::vector<char> drop(g.ops.size(), 0);
  int dropped = 0;
  for (size_t i = g.ops.size(); i-- > 0;) {
    const Op& op = g.ops[i];
    if (!droppable_if_unread(op) || op.out < 0 || reads[(size_t)op.out] != 0 ||
        writes[(size_t)op.out] != 1 || keep.count(op.out) ||
        g.slots[(size_t)op.out].is_param)
      continue;
    drop[i] = 1;
    ++dropped;
    for_each_read(op, [&](int s) { --reads[(size_t)s]; });
    g.slots[(size_t)op.out].len = 0;
  }
  if (dropped == 0) return 0;
  std::vector<Op> kept;
  kept.reserve(g.ops.size() - (size_t)dropped);
  for (size_t i = 0; i < g.ops.size(); ++i)
    if (!drop[i]) kept.push_back(g.ops[i]);
  g.ops = std::move(kept);
  return dropped;
}

}  // namespace

CseStats cse(Graph& g, std::vector<std::pair<int, std::vector<double>>>& fills,
             std::vector<int>& target_terms,
             const std::vector<int>& extra_roots, bool merge_active) {
  CseStats st;
  if (debug_env("STANLI_NO_CSE")) return st;

  const size_t n_slots = g.slots.size();
  std::vector<int> writes(n_slots, 0);
  for (const Op& op : g.ops) {
    if (op.out >= 0) ++writes[(size_t)op.out];
    if (op.out2 >= 0) ++writes[(size_t)op.out2];
  }

  std::unordered_set<int> keep(extra_roots.begin(), extra_roots.end());
  if (g.result_slot >= 0) keep.insert(g.result_slot);
  // A fill writes its slot at bind time from a buffer sized by the slot
  // length, so a filled slot must keep that length even once dead.
  for (const auto& f : fills) keep.insert(f.first);
  const std::vector<char> whole =
      merge_active ? std::vector<char>()
                   : whole_seeds(g, target_terms, keep, writes);

  std::vector<int64_t> version(n_slots, 0);
  std::vector<char> active(n_slots, 0);
  for (size_t s = 0; s < n_slots; ++s) active[s] = g.slots[s].is_param;
  std::unordered_map<Key, size_t, KeyHash> table;
  std::unordered_map<Key, int, KeyHash> whole_table;  // key -> surviving term
  std::unordered_map<int, int> rename;  // removed output -> surviving output
  std::unordered_map<int, int> primal;  // same value, independent adjoint
  const auto resolve = [&](int s) {
    auto it = rename.find(s);
    return it == rename.end() ? s : it->second;
  };

  std::vector<char> drop(g.ops.size(), 0);
  Key key;
  for (size_t i = 0; i < g.ops.size(); ++i) {
    Op& op = g.ops[i];
    for (int j = 0; j < op.n_in; ++j) op.in[j] = resolve(op.in[j]);
    bool any_active = false;
    for (int j = 0; j < op.n_in; ++j)
      if (op.in[j] >= 0) any_active |= active[(size_t)op.in[j]];
    if (op.out >= 0) active[(size_t)op.out] |= any_active;
    if (op.out2 >= 0) active[(size_t)op.out2] |= any_active;

    bool candidate = op.out >= 0 && op.out2 < 0 && op.udata == nullptr &&
                     op.primal_source < 0 && !op.dyn_lengths &&
                     !never_merge(op.opcode) && writes[(size_t)op.out] == 1 &&
                     !keep.count(op.out) && !g.slots[(size_t)op.out].is_param;
    for (int j = 0; candidate && j < op.n_in; ++j)
      candidate = op.in[j] != op.out;

    if (candidate) {
      const Kernel* implementation = find_kernel(op.opcode);
      if (!implementation || implementation->make_state) candidate = false;
    }
    if (candidate) {
      key.w.clear();
      key.w.push_back(op.opcode);
      key.w.push_back(op.variant);
      key.w.push_back(g.slots[(size_t)op.out].len);
      key.w.push_back(op.n_in);
      for (int j = 0; j < op.n_in; ++j) {
        const auto shared = primal.find(op.in[j]);
        const int input = shared == primal.end() ? op.in[j] : shared->second;
        key.w.push_back(input);
        key.w.push_back(input >= 0 ? version[(size_t)input] : 0);
      }
      key.w.push_back(op.n_idata);
      for (int64_t k = 0; k < op.n_idata; ++k) key.w.push_back(op.idata[k]);
      // An active value with a whole-number seed merges into an earlier one
      // with a whole-number seed.
      if (any_active && !merge_active && whole[(size_t)op.out]) {
        auto term = whole_table.emplace(key, op.out);
        if (!term.second) {
          rename.emplace(op.out, term.first->second);
          g.slots[(size_t)op.out].len = 0;
          drop[i] = 1;
          ++st.ops_removed;
          continue;
        }
      }
      auto ins = table.emplace(key, i);
      if (!ins.second) {
        Op& survivor = g.ops[ins.first->second];
        if (any_active && !merge_active) {
          // Combining output adjoints before the pullback reassociates
          // arithmetic. Share only the primal, keeping source reverse order.
          op.primal_source = survivor.primal_source = survivor.out;
          primal.emplace(op.out, survivor.out);
          ++st.primals_shared;
        } else {
          rename.emplace(op.out, survivor.out);
          g.slots[(size_t)op.out].len = 0;
          drop[i] = 1;
          ++st.ops_removed;
          continue;
        }
      }
    }
    if (op.out >= 0) ++version[(size_t)op.out];
    if (op.out2 >= 0) ++version[(size_t)op.out2];
  }

  if (st.ops_removed > 0) {
    for (int& t : target_terms) t = resolve(t);
    std::vector<Op> kept;
    kept.reserve(g.ops.size() - (size_t)st.ops_removed);
    for (size_t i = 0; i < g.ops.size(); ++i)
      if (!drop[i]) kept.push_back(g.ops[i]);
    g.ops = std::move(kept);
  }
  scale_repeated_terms(g, fills, target_terms);
  st.unread_dropped = drop_unread(g, target_terms, keep);
  return st;
}

}  // namespace stanli
