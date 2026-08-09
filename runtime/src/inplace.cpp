// Destructive functional updates.
//
// `mu[n] = ...` inside a data-bound loop unrolls into a chain of
// OP_SET_INDEX ops. Each one copies its whole input vector into a fresh
// slot and then pokes one element, so N writes cost O(N^2) time and O(N^2)
// arena. Measured on radon_county_intercept (N=12,573): 90.5 ms per
// gradient and 2.58 GB peak RSS, against CmdStan's 438 us.
//
// A write can mutate its input vector directly when nothing later observes
// that vector's pre-write contents. The condition is LAST USE, not single
// use: the read-back `INDEX(mu_n, n)` that immediately follows a write in
// the same lane is an earlier use of the same slot, so a single-use rule
// would refuse exactly the chains that matter. A write qualifies when
//
//   - its input is produced by an op. Fill-backed slots (declared vectors,
//     constants) are written once at bind time, so mutating one would let
//     state leak from one gradient evaluation into the next. The first
//     write of each chain therefore keeps its copy -- one O(N) copy per
//     evaluation, which is the price of the whole chain now.
//   - this op is the last op reading that slot,
//   - neither the input nor the output is a root (read straight from the
//     arena, invisible in the use map) or a parameter, and
//   - no EARLIER reader of that vector needs its values during the reverse
//     sweep. Forward order makes the values those readers see correct, but
//     a backward pass runs after every write has landed: `legacy_bwd_vec_in`
//     (log_sum_exp, softmax, ...) rebuilds its var tape from
//     `ctx.in[0].data` at reverse time, so a destroyed buffer feeds it the
//     wrong numbers. Only ops whose backward moves adjoints and never reads
//     input values may precede a destructive write. Found by the corpus
//     A/B: without this, 8 HMM/LDA/mixture models were wrong by up to
//     1.7e+05 relative while every op count looked untouched.
//
// The rewrite makes the op's output slot BE its input slot, so `bind_()`
// gives them one offset with no executor change, and later references to
// the dead output slot are renamed to the input.
#include <stanli/inplace.hpp>
#include <stanli/optable.hpp>

#include <algorithm>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace stanli {

// Declared in the header. tests/test_pass_safety.cpp checks every opcode
// this returns true for, by poisoning inputs between forward and backward
// and requiring the adjoints to be unchanged.
bool backward_ignores_input_values(uint16_t oc) {
  switch (oc) {
    case OP_INDEX:
    case OP_SLICE:
    case OP_SLICE_STRIDED:
    case OP_GATHER:
    case OP_SET_INDEX:
    case OP_SET_INDEX_INPLACE:
    // Since runtime/kernels/mixture.cpp made these native, their partials
    // live in scratch (which destructive writes never touch) rather than
    // being recomputed from the inputs. That is what lets the HMM forward
    // algorithm -- fill `acc` element by element, read it whole -- take
    // the destructive path.
    case OP_LOG_SUM_EXP:
    case OP_LSE2:
    case OP_LOG_MIX:
      return true;
    default:
      return false;
  }
}

int make_inplace_updates(Graph& g, const std::vector<int>& roots) {
  if (std::getenv("STANLI_NO_INPLACE")) return 0;

  const int n_slots = static_cast<int>(g.slots.size());
  // Last op index that reads each slot, and whether an op writes it.
  std::vector<int> last_use(n_slots, -1);
  std::vector<char> written(n_slots, 0);
  for (size_t i = 0; i < g.ops.size(); ++i) {
    for (int j = 0; j < g.ops[i].n_in; ++j)
      last_use[g.ops[i].in[j]] = static_cast<int>(i);
    written[g.ops[i].out] = 1;
    if (g.ops[i].out2 >= 0) written[g.ops[i].out2] = 1;
  }
  std::unordered_set<int> root_set(roots.begin(), roots.end());
  if (g.result_slot >= 0) root_set.insert(g.result_slot);

  std::unordered_map<int, int> rename;  // dead output slot -> live slot
  const auto resolve = [&](int s) {
    auto it = rename.find(s);
    return it == rename.end() ? s : it->second;
  };

  // Set once some op that will re-read a slot's values during the reverse
  // sweep has consumed it; from then on that slot may not be destroyed.
  std::vector<char> value_reader(n_slots, 0);

  int n_rewritten = 0;
  for (size_t i = 0; i < g.ops.size(); ++i) {
    Op& op = g.ops[i];
    for (int j = 0; j < op.n_in; ++j) op.in[j] = resolve(op.in[j]);
    const bool routes_only = backward_ignores_input_values(op.opcode);

    if (op.opcode != OP_SET_INDEX) {
      if (!routes_only)
        for (int j = 0; j < op.n_in; ++j) value_reader[op.in[j]] = 1;
      continue;
    }

    const int vec = op.in[0];
    // The renamed slot inherits the original's last use: a read of any
    // link in the chain is a read of the one buffer they now share.
    if (!written[vec] || g.slots[vec].is_param) continue;
    if (root_set.count(vec) || root_set.count(op.out)) continue;
    if (last_use[vec] != static_cast<int>(i)) continue;
    if (value_reader[vec]) continue;

    rename[op.out] = vec;
    // A later read of the old output is a read of the shared buffer, so
    // the surviving slot inherits it: the next write in the chain must not
    // treat itself as the last use while that read is still pending.
    last_use[vec] = std::max(last_use[vec], last_use[op.out]);
    // The old output slot is now unreachable: no op writes or reads it,
    // and it was neither a root nor fill-backed (checked above), so it
    // needs no arena. Without this the chain still costs O(N^2) memory --
    // bind_() sizes the arenas from slot lengths, not from op references.
    g.slots[op.out].len = 0;
    op.opcode = OP_SET_INDEX_INPLACE;
    op.out = vec;  // out and in[0] are now one slot: one buffer, one adjoint
    ++n_rewritten;
  }
  return n_rewritten;
}

int forward_stores_to_loads(Graph& g, const std::vector<int>& roots) {
  if (std::getenv("STANLI_NO_INPLACE")) return 0;

  std::unordered_set<int> root_set(roots.begin(), roots.end());
  if (g.result_slot >= 0) root_set.insert(g.result_slot);

  // Most recent write to each vector, as (op index, element, value slot).
  struct Store {
    size_t op;
    int elem;
    int value;
  };
  std::unordered_map<int, Store> last_store;
  std::unordered_map<int, int> rename;  // load output -> stored value slot
  const auto resolve = [&](int s) {
    auto it = rename.find(s);
    return it == rename.end() ? s : it->second;
  };

  std::vector<char> drop(g.ops.size(), 0);
  int removed = 0;
  for (size_t i = 0; i < g.ops.size(); ++i) {
    Op& op = g.ops[i];
    for (int j = 0; j < op.n_in; ++j) op.in[j] = resolve(op.in[j]);

    if (op.opcode == OP_SET_INDEX || op.opcode == OP_SET_INDEX_INPLACE) {
      // The written vector is op.out (both forms), and the element written
      // is idata[0]. A later write to the same vector supersedes this one.
      last_store[op.out] = Store{i, op.idata[0], op.in[1]};
      continue;
    }
    if (op.opcode == OP_INDEX && op.n_idata == 1) {
      auto it = last_store.find(op.in[0]);
      if (it != last_store.end() && it->second.elem == op.idata[0] &&
          !root_set.count(op.out)) {
        // Reading back exactly what the last write put there.
        rename[op.out] = it->second.value;
        drop[i] = 1;
        ++removed;
        continue;
      }
    }
    // Any other read of a vector leaves its stores observable; a write to
    // some *other* vector cannot invalidate this one, so nothing else
    // needs clearing here -- last_store is keyed by vector.
  }

  // Sweep dead writes: a store whose vector nothing reads afterwards (and
  // which is not a root) computes nothing. Walk backwards so a chain dies
  // from its tail.
  std::vector<char> read_after(g.slots.size(), 0);
  for (size_t k = g.ops.size(); k-- > 0;) {
    if (drop[k]) continue;
    Op& op = g.ops[k];
    const bool is_store =
        op.opcode == OP_SET_INDEX || op.opcode == OP_SET_INDEX_INPLACE;
    if (is_store && !read_after[op.out] && !root_set.count(op.out)) {
      drop[k] = 1;
      ++removed;
      continue;
    }
    for (int j = 0; j < op.n_in; ++j) read_after[op.in[j]] = 1;
  }

  if (removed) {
    std::vector<Op> kept;
    kept.reserve(g.ops.size() - (size_t)removed);
    for (size_t i = 0; i < g.ops.size(); ++i)
      if (!drop[i]) kept.push_back(g.ops[i]);
    g.ops = std::move(kept);
  }
  return removed;
}

}  // namespace stanli
