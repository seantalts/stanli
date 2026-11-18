#include <stanli/optable.hpp>
#include <stanli/target_sums.hpp>

#include <cmath>
#include <unordered_set>

#include "pass_util.hpp"
#include "reroll_profile.hpp"

namespace stanli {

int split_target_sums(Graph& g, const Fills& fills,
                      std::vector<int>& target_terms,
                      const std::vector<int>& roots) {
  const auto scalar = [&](int s) {
    return s >= 0 && (size_t)s < g.slots.size() && g.slots[(size_t)s].len == 1;
  };
  const auto plain_add = [&](const Op& op) {
    return op.opcode == OP_ADD && op.variant == 0 && op.n_in == 2 &&
           op.n_idata == 0 && op.out2 < 0 && op.udata == nullptr &&
           op.dyn_capacity == 0 && op.primal_source < 0 && scalar(op.out) &&
           scalar(op.in[0]) && scalar(op.in[1]) && op.out != op.in[0] &&
           op.out != op.in[1];
  };
  bool any = false;
  for (const Op& op : g.ops) any |= plain_add(op);
  if (!any) return 0;

  // An addition is removable when it is the only writer of its result and
  // its one reader is the next addition up the chain, or the target.
  std::vector<int> uses(g.slots.size(), 0), writer(g.slots.size(), -1);
  for (size_t i = 0; i < g.ops.size(); ++i) {
    const Op& op = g.ops[i];
    for (int k = 0; k < op.n_in; ++k)
      if (op.in[k] >= 0) ++uses[(size_t)op.in[k]];
    for (int s : {op.out, op.out2})
      if (s >= 0) writer[(size_t)s] = writer[(size_t)s] == -1 ? (int)i : -2;
  }
  for (int s : roots)
    if (s >= 0) ++uses[(size_t)s];
  if (g.result_slot >= 0) ++uses[(size_t)g.result_slot];
  for (int s : target_terms) ++uses[(size_t)s];
  const auto removable = [&](int s) {
    return uses[(size_t)s] == 1 && writer[(size_t)s] >= 0 &&
           !g.slots[(size_t)s].is_param &&
           plain_add(g.ops[(size_t)writer[(size_t)s]]);
  };
  const auto constants = scalar_constants(fills);
  const auto positive_zero = [&](int s) {
    const auto it = constants.find(s);
    return it != constants.end() && writer[(size_t)s] == -1 &&
           it->second == 0.0 && !std::signbit(it->second);
  };

  std::vector<char> dead(g.ops.size(), 0);
  std::vector<int> terms;
  int removed = 0;
  for (int t : target_terms) {
    if (!removable(t)) {
      terms.push_back(t);
      continue;
    }
    // Walk down the accumulator: each addition's first operand is the
    // running sum and its second the term that statement added.
    std::vector<int> chain, added;
    int s = t;
    do {
      const Op& op = g.ops[(size_t)writer[(size_t)s]];
      chain.push_back(writer[(size_t)s]);
      added.push_back(op.in[1]);
      s = op.in[0];
    } while (removable(s));
    // A chain shorter than re-roll's narrowest vector gains nothing, and
    // splitting the parts of one term would leave lanes that no longer match.
    if ((int64_t)chain.size() < detail::kMinLanes) {
      terms.push_back(t);
      continue;
    }
    std::vector<int> leaves{s};
    leaves.insert(leaves.end(), added.rbegin(), added.rend());
    std::unordered_set<int> seen;
    bool distinct = true;
    for (int leaf : leaves) distinct &= seen.insert(leaf).second;
    if (!distinct) {
      terms.push_back(t);
      continue;
    }
    for (int leaf : leaves)
      if (!positive_zero(leaf)) terms.push_back(leaf);
    for (int i : chain) {
      dead[(size_t)i] = 1;
      g.slots[(size_t)g.ops[(size_t)i].out].len = 0;
    }
    removed += (int)chain.size();
  }
  if (!removed) return 0;
  target_terms = std::move(terms);
  std::vector<Op> kept;
  kept.reserve(g.ops.size() - (size_t)removed);
  for (size_t i = 0; i < g.ops.size(); ++i)
    if (!dead[i]) kept.push_back(g.ops[i]);
  g.ops = std::move(kept);
  return removed;
}

}  // namespace stanli
