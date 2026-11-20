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
// Lowering has already evaluated many of these values with the MIR
// interpreter, the same evaluator that computes transformed data, and bound
// them for later data-only reads. An op whose value lowering recorded takes
// that value, so a variable has one value in the interpreter and the graph,
// and an expensive call is not evaluated a second time. The rest run through
// a real Executor over a sub-graph holding just those ops and the slots they
// touch.
//
// The one subtlety is order. Folding replaces a slot with its FINAL value, so
// a surviving op that reads a slot some later constant op overwrites would
// see the wrong contents -- the destructive update chains write one slot end
// to end, which makes that reachable rather than theoretical. Such slots are
// refused, and the refusal cascades to everything downstream of them.
#include <stanli/algebra.hpp>
#include <stanli/constfold.hpp>
#include <stanli/dae.hpp>
#include <stanli/debug_switch.hpp>
#include <stanli/island.hpp>
#include <stanli/ode.hpp>
#include <stanli/ode_adjoint.hpp>
#include <stanli/optable.hpp>
#include <stanli/quadrature.hpp>

#include <algorithm>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>

namespace stanli {
namespace {

// The user function a solver op calls back into, or null for other ops.
const RetainedCallback* solver_callback(const Op& op) {
  switch (op.opcode) {
    case OP_ODE:
      return static_cast<const OdeSpec*>(op.udata);
    case OP_ODE_ADJOINT:
      return static_cast<const OdeAdjointSpec*>(op.udata);
    case OP_DAE:
      return static_cast<const DaeSpec*>(op.udata);
    case OP_ALGEBRA_SOLVER:
      return static_cast<const AlgebraSpec*>(op.udata);
    case OP_QUADRATURE:
      return static_cast<const QuadratureSpec*>(op.udata);
    default:
      return nullptr;
  }
}

// Whether the program that an op's payload carries can print or reject. A
// callback that did not compile runs in the interpreter, whose effects are
// not known here, so it counts as one that can.
bool payload_has_effect(const Op& op) {
  if (op.udata == nullptr) return false;
  if (op.opcode == OP_ISLAND)
    return island_has_effect(*static_cast<const IslandProg*>(op.udata));
  if (const RetainedCallback* callback = solver_callback(op))
    return !callback->prog.ok || island_has_effect(callback->prog);
  return false;
}

// True for each op whose inputs are all data, constants, or the outputs of
// earlier constant ops, and that is not in `refused`. Parallel to `g.ops`.
std::vector<char> mark_constant_ops(const Graph& g,
                                    const std::vector<char>& refused) {
  std::vector<char> no_fold(g.slots.size(), 0);
  std::vector<char> is_const(g.ops.size(), 0);

  // Terminates without a round cap: every `changed` sets a no_fold entry
  // that was clear, no_fold lives outside the loop and is never cleared,
  // so the number of rounds is bounded by g.slots.size().
  for (;;) {
    // A slot carries a parameter's influence if it is a parameter or if some
    // surviving op writes it. Ops are in evaluation order, so one pass does.
    std::vector<char> live(g.slots.size(), 0);
    for (size_t s = 0; s < g.slots.size(); ++s)
      if (g.slots[s].is_param) live[s] = 1;

    for (size_t i = 0; i < g.ops.size(); ++i) {
      const Op& op = g.ops[i];
      // Effects are graph semantics, even when all their values are data.
      // Executing them in this compile-time pass would erase them from every
      // subsequent evaluation (and print/reject while compiling instead).
      // A compiled region or a solver's callback carries its effects inside
      // its own program, so the opcode alone does not say: ask the payload.
      // A region over literals alone, such as an inlined loop with constant
      // bounds, has no inputs and is constant all the same.
      bool c = (op.n_in > 0 || op.opcode == OP_ISLAND) &&
               !is_effectful_op(op.opcode) && !payload_has_effect(op) &&
               !refused[i];
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
    if (!changed) return is_const;
  }
}

bool same_op(const Op& a, const Op& b) {
  return a.opcode == b.opcode && a.variant == b.variant && a.out == b.out &&
         a.out2 == b.out2 && a.n_in == b.n_in &&
         std::equal(a.in, a.in + a.n_in, b.in) && a.idata == b.idata &&
         a.n_idata == b.n_idata && a.udata == b.udata &&
         a.dyn_capacity == b.dyn_capacity &&
         a.dyn_extent_in == b.dyn_extent_in && a.dyn_lengths == b.dyn_lengths &&
         a.primal_source == b.primal_source;
}

// The recorded value of each op in `known` that still writes its slot alone,
// as it did when lowering evaluated it. Parallel to `g.ops`.
std::vector<const std::vector<double>*> known_values(
    const Graph& g,
    const std::vector<std::pair<int, std::vector<double>>>& fills,
    const std::vector<KnownOutput>& known) {
  std::vector<const std::vector<double>*> values(g.ops.size(), nullptr);
  if (known.empty()) return values;
  std::unordered_map<int, const KnownOutput*> by_out;
  for (const KnownOutput& k : known) by_out[k.op.out] = &k;
  std::vector<int> writers(g.slots.size(), 0);
  for (const Op& op : g.ops) {
    if (op.out >= 0) ++writers[(size_t)op.out];
    if (op.out2 >= 0) ++writers[(size_t)op.out2];
  }
  for (const auto& f : fills) ++writers[(size_t)f.first];
  for (size_t i = 0; i < g.ops.size(); ++i) {
    const Op& op = g.ops[i];
    if (op.out < 0 || op.out2 >= 0 || writers[(size_t)op.out] != 1) continue;
    const auto it = by_out.find(op.out);
    if (it == by_out.end() || !same_op(it->second->op, op) ||
        (int64_t)it->second->value.size() != g.slots[(size_t)op.out].len)
      continue;
    values[i] = &it->second->value;
  }
  return values;
}

}  // namespace

ConstFoldStats const_fold(
    Graph& g, std::vector<std::pair<int, std::vector<double>>>& fills,
    const std::vector<int>& roots, const std::vector<KnownOutput>& known) {
  ConstFoldStats st;
  if (const char* e = debug_env("STANLI_NO_CONSTFOLD"))
    if (e[0] == '1') return st;

  const std::vector<const std::vector<double>*> known_value =
      known_values(g, fills, known);
  std::vector<char> refused(g.ops.size(), 0);
  std::vector<char> is_const;
  std::vector<std::pair<int, std::vector<double>>> new_fills;
  // Each round folds the constant ops, or finds the first one that throws
  // (a density rejecting its own data, an unregistered opcode). That op is
  // not folded, which makes every op that reads it non-constant too, and
  // the next round tries the rest. The normal sweep reports the error in
  // context. Every round refuses one more op, so the loop ends.
  for (;;) {
    is_const = mark_constant_ops(g, refused);
    std::vector<size_t> run;  // constant ops that have to be evaluated
    size_t n_const = 0;
    for (size_t i = 0; i < g.ops.size(); ++i) {
      n_const += (size_t)is_const[i];
      if (is_const[i] && !known_value[i]) run.push_back(i);
    }
    if (n_const == 0) return st;
    // Evaluates the first `count` ops of `run`, and with `collect` keeps
    // the outputs that anything still needs: a surviving op's input, or a
    // root. The rest was scaffolding and is simply gone.
    const auto evaluate = [&](size_t count, bool collect) {
      // Slots the folded ops touch, remapped into a sub-graph so the
      // temporary arena is proportional to the constant part rather than
      // to the model.
      std::unordered_map<int, int> remap;
      Graph sub;
      auto map_slot = [&](int s) {
        auto it = remap.find(s);
        if (it != remap.end()) return it->second;
        const int ns = sub.add_slot(g.slots[(size_t)s].len, false);
        remap.emplace(s, ns);
        return ns;
      };
      for (size_t k = 0; k < count; ++k) {
        const Op& op = g.ops[run[k]];
        Op sop = op;  // idata / udata point into g's pools, which outlive this
        for (int j = 0; j < op.n_in; ++j)
          if (op.in[j] >= 0) sop.in[j] = map_slot(op.in[j]);
        if (op.out >= 0) sop.out = map_slot(op.out);
        if (op.out2 >= 0) sop.out2 = map_slot(op.out2);
        sub.ops.push_back(sop);
      }
      sub.result_slot = sub.slots.empty() ? -1 : 0;
      try {
        Executor ex(std::move(sub));
        for (const auto& f : fills) {
          auto it = remap.find(f.first);
          if (it == remap.end()) continue;
          double* p = ex.value_ptr(it->second);
          for (size_t j = 0; j < f.second.size(); ++j) p[j] = f.second[j];
        }
        for (size_t i = 0; i < g.ops.size(); ++i) {
          if (!is_const[i] || !known_value[i]) continue;
          auto it = remap.find(g.ops[i].out);
          if (it == remap.end()) continue;
          std::copy(known_value[i]->begin(), known_value[i]->end(),
                    ex.value_ptr(it->second));
        }
        ex.run_forward_only();
        if (!collect) return true;
        std::unordered_set<int> needed(roots.begin(), roots.end());
        for (size_t i = 0; i < g.ops.size(); ++i) {
          if (is_const[i]) continue;
          const Op& op = g.ops[i];
          for (int j = 0; j < op.n_in; ++j)
            if (op.in[j] >= 0) needed.insert(op.in[j]);
        }
        if (g.result_slot >= 0) needed.insert(g.result_slot);
        std::unordered_set<int> emitted;
        for (size_t i = 0; i < g.ops.size(); ++i) {
          if (!is_const[i]) continue;
          const Op& op = g.ops[i];
          for (int out : {op.out, op.out2}) {
            if (out < 0 || !needed.count(out) || !emitted.insert(out).second)
              continue;
            if (known_value[i]) {
              new_fills.emplace_back(out, *known_value[i]);
              continue;
            }
            const int64_t len = g.slots[(size_t)out].len;
            const double* p = std::as_const(ex).value_ptr(remap.at(out));
            new_fills.emplace_back(out, std::vector<double>(p, p + len));
          }
        }
        st.slots_folded = (int)emitted.size();
        return true;
      } catch (const std::exception&) {
        return false;
      }
    };
    if (evaluate(run.size(), true)) break;
    if (run.empty()) return st;
    // The first op to throw is the first whose prefix of `run` throws.
    size_t ok = 0, fails = run.size();
    while (fails - ok > 1) {
      const size_t mid = ok + (fails - ok) / 2;
      (evaluate(mid, false) ? ok : fails) = mid;
    }
    refused[run[fails - 1]] = 1;
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
  kept.reserve(g.ops.size());
  for (size_t i = 0; i < g.ops.size(); ++i)
    if (!is_const[i]) kept.push_back(g.ops[i]);
  st.ops_removed = (int)(g.ops.size() - kept.size());
  g.ops = std::move(kept);
  return st;
}

}  // namespace stanli
