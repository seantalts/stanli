// Constant folding over the op graph.
//
// A model's log_prob graph is not all parameter-dependent. `dogs` declares
// two 25x30 matrices in `transformed parameters` and fills them entirely from
// its outcome array; `election88_full` and several of the M*_model family do
// the same. Those lower to thousands of ops that recompute the same numbers
// on every leapfrog step, and -- worse -- stand between the re-roll pass and
// the loop it would otherwise vectorize, because a lane whose operands are op
// outputs is not a lane whose operands are data.
//
// So: find the ops no parameter reaches, run them once, and replace them with
// their results.
//
// Running them means running them. There is one definition of what an op
// computes -- its kernel -- and this pass uses it, through a real Executor
// over a sub-graph holding just the constant ops and the slots they touch. A
// compile-time re-implementation would be a second definition, and the two
// would drift.
//
// The one subtlety is order. Folding replaces a slot with its FINAL value, so
// a surviving op that reads a slot some later constant op overwrites would
// see the wrong contents -- the destructive update chains write one slot end
// to end, which makes that reachable rather than theoretical. Such slots are
// refused, and the refusal cascades to everything downstream of them.
#include <stanli/constfold.hpp>
#include <stanli/optable.hpp>

#include <cstdlib>
#include <unordered_map>
#include <unordered_set>

namespace stanli {
namespace {

// True for each op whose inputs are all data, constants, or the outputs of
// earlier constant ops. Parallel to `g.ops`.
std::vector<char> mark_constant_ops(const Graph& g) {
  std::vector<char> no_fold(g.slots.size(), 0);
  std::vector<char> is_const(g.ops.size(), 0);

  for (int round = 0;; ++round) {
    // A slot carries a parameter's influence if it is a parameter or if some
    // surviving op writes it. Ops are in evaluation order, so one pass does.
    std::vector<char> live(g.slots.size(), 0);
    for (size_t s = 0; s < g.slots.size(); ++s)
      if (g.slots[s].is_param) live[s] = 1;

    for (size_t i = 0; i < g.ops.size(); ++i) {
      const Op& op = g.ops[i];
      bool c = op.n_in > 0;
      for (int k = 0; k < op.n_in; ++k)
        if (op.in[k] >= 0 && live[(size_t)op.in[k]]) c = false;
      if (c && op.out >= 0 && no_fold[(size_t)op.out]) c = false;
      if (c && op.out2 >= 0 && no_fold[(size_t)op.out2]) c = false;
      is_const[i] = c ? 1 : 0;
      if (!c) {
        if (op.out >= 0) live[(size_t)op.out] = 1;
        if (op.out2 >= 0) live[(size_t)op.out2] = 1;
      }
    }

    // Refuse any slot a surviving op reads before a constant op overwrites
    // it: that op needs the intermediate contents, and folding keeps only
    // the final ones.
    std::vector<int> last_write(g.slots.size(), -1);
    for (size_t i = 0; i < g.ops.size(); ++i) {
      if (!is_const[i]) continue;
      const Op& op = g.ops[i];
      if (op.out >= 0) last_write[(size_t)op.out] = (int)i;
      if (op.out2 >= 0) last_write[(size_t)op.out2] = (int)i;
    }
    bool changed = false;
    for (size_t i = 0; i < g.ops.size(); ++i) {
      if (is_const[i]) continue;
      const Op& op = g.ops[i];
      for (int k = 0; k < op.n_in; ++k) {
        const int s = op.in[k];
        if (s >= 0 && last_write[(size_t)s] > (int)i && !no_fold[(size_t)s]) {
          no_fold[(size_t)s] = 1;
          changed = true;
        }
      }
      // And refuse any slot a surviving READ-MODIFY-WRITE op updates in
      // place, when a constant op is what produces its initial contents.
      //
      // make_inplace_updates refuses a fill-backed base for exactly this
      // reason -- "mutating one would let state leak from one gradient
      // evaluation into the next" -- but it decides BEFORE this pass runs,
      // and folding is what turns an op-written slot into a fill. So the
      // precondition it checked can stop being true afterwards.
      //
      //   vector[N] mu = rep_vector(0, N);
      //   for (n in 1:N) mu[n] += f(theta, n);
      //
      // rep_vector's arguments are constants, so it folds; mu becomes a
      // bind-time fill; and the in-place element writes then accumulate
      // across evaluations. The same point evaluated four times gave four
      // different log densities, and nothing structural showed it --
      // the corpus rig evaluates one point per process, so only a sampler
      // (or a second call) can see it.
      //
      // out == one of in is the read-modify-write signature; an op that
      // merely overwrites its output is safe to fold behind, since it
      // recomputes the whole slot every evaluation.
      if (op.out >= 0 && last_write[(size_t)op.out] >= 0 &&
          !no_fold[(size_t)op.out]) {
        bool reads_own_output = false;
        for (int k = 0; k < op.n_in; ++k)
          reads_own_output = reads_own_output || op.in[k] == op.out;
        if (reads_own_output) {
          no_fold[(size_t)op.out] = 1;
          changed = true;
        }
      }
    }
    if (!changed || round > 16) return is_const;
  }
}

}  // namespace

ConstFoldStats const_fold(
    Graph& g, std::vector<std::pair<int, std::vector<double>>>& fills,
    const std::vector<int>& roots, std::vector<int>* folded) {
  ConstFoldStats st;
  if (const char* e = std::getenv("STANLI_NO_CONSTFOLD"))
    if (e[0] == '1') return st;

  const std::vector<char> is_const = mark_constant_ops(g);
  size_t n_const = 0;
  for (char c : is_const) n_const += (size_t)c;
  if (n_const == 0) return st;

  // Slots the folded ops touch, remapped into a sub-graph so the temporary
  // arena is proportional to the constant part rather than to the model.
  std::unordered_map<int, int> remap;
  Graph sub;
  auto map_slot = [&](int s) {
    auto it = remap.find(s);
    if (it != remap.end()) return it->second;
    const int ns = sub.add_slot(g.slots[(size_t)s].len, false);
    remap.emplace(s, ns);
    return ns;
  };
  for (size_t i = 0; i < g.ops.size(); ++i) {
    if (!is_const[i]) continue;
    const Op& op = g.ops[i];
    Op sop = op;  // idata / udata point into g's pools, which outlive this
    for (int k = 0; k < op.n_in; ++k)
      if (op.in[k] >= 0) sop.in[k] = map_slot(op.in[k]);
    if (op.out >= 0) sop.out = map_slot(op.out);
    if (op.out2 >= 0) sop.out2 = map_slot(op.out2);
    sub.ops.push_back(sop);
  }
  sub.result_slot = 0;

  // Const outputs anything still needs: a surviving op's input, or a root.
  // The rest was scaffolding and is simply gone.
  std::unordered_set<int> needed(roots.begin(), roots.end());
  for (size_t i = 0; i < g.ops.size(); ++i) {
    if (is_const[i]) continue;
    const Op& op = g.ops[i];
    for (int k = 0; k < op.n_in; ++k)
      if (op.in[k] >= 0) needed.insert(op.in[k]);
  }
  if (g.result_slot >= 0) needed.insert(g.result_slot);

  std::vector<std::pair<int, std::vector<double>>> new_fills;
  try {
    Executor ex(std::move(sub));
    for (const auto& f : fills) {
      auto it = remap.find(f.first);
      if (it == remap.end()) continue;
      double* p = ex.value_ptr(it->second);
      for (size_t j = 0; j < f.second.size(); ++j) p[j] = f.second[j];
    }
    ex.run_forward_only();
    std::unordered_set<int> emitted;
    for (size_t i = 0; i < g.ops.size(); ++i) {
      if (!is_const[i]) continue;
      const Op& op = g.ops[i];
      for (int out : {op.out, op.out2}) {
        if (out < 0 || !needed.count(out) || !emitted.insert(out).second)
          continue;
        const int64_t len = g.slots[(size_t)out].len;
        const double* p = ex.value_ptr(remap.at(out));
        new_fills.emplace_back(out, std::vector<double>(p, p + len));
        if (folded) folded->push_back(out);
      }
    }
    st.slots_folded = (int)emitted.size();
  } catch (const std::exception&) {
    // The constant part does not evaluate (a density rejecting its own data,
    // an unregistered opcode). Leave the graph exactly as it was; the normal
    // sweep will hit the same thing and report it in context.
    return st;
  }

  // A folded slot may already have a fill -- the zeroed vector a destructive
  // update chain started from. The computed value supersedes it, and fills
  // are applied in order, so appending is enough; dropping the superseded
  // one just saves writing a buffer twice at bind.
  {
    std::unordered_set<int> superseded;
    for (const auto& f : new_fills) superseded.insert(f.first);
    std::vector<std::pair<int, std::vector<double>>> kept;
    kept.reserve(fills.size() + new_fills.size());
    for (auto& f : fills)
      if (!superseded.count(f.first)) kept.push_back(std::move(f));
    for (auto& f : new_fills) kept.push_back(std::move(f));
    fills = std::move(kept);
  }

  std::vector<Op> kept;
  kept.reserve(g.ops.size() - n_const);
  for (size_t i = 0; i < g.ops.size(); ++i)
    if (!is_const[i]) kept.push_back(g.ops[i]);
  st.ops_removed = (int)(g.ops.size() - kept.size());
  g.ops = std::move(kept);
  return st;
}

}  // namespace stanli
