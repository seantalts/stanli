// Ops that do not depend on any parameter compute the same values on every
// gradient evaluation. Run them once, at lowering time, and turn their
// outputs into data.
#ifndef STANLI_CONSTFOLD_HPP
#define STANLI_CONSTFOLD_HPP

#include <stanli/graph.hpp>

#include <utility>
#include <vector>

namespace stanli {

struct ConstFoldStats {
  int ops_removed = 0;
  int slots_folded = 0;  // const outputs some surviving op still reads
};

// Evaluate every constant op once and delete it, appending a fill for each
// folded slot a surviving op (or a root) still reads. `folded` receives those
// slot ids so the caller can mark them data-like for the passes that follow.
//
// Why lowering time rather than a skip flag in the executor: the point is not
// only to stop re-evaluating them, it is to get them out of the graph, so the
// re-roll pass sees a lane whose data-derived operands are plain data and can
// vectorize it. Transformed parameters that turn out to be pure functions of
// data (`dogs` builds two 25x30 matrices from its outcome array) are the case
// that motivates it.
//
// Runs the ops through a real Executor over a sub-graph of just those ops, so
// there is one implementation of what an op means. If that evaluation throws
// -- a density rejecting its own data, say -- the graph is left untouched.
// STANLI_NO_CONSTFOLD=1 disables the pass.
ConstFoldStats const_fold(
    Graph& g, std::vector<std::pair<int, std::vector<double>>>& fills,
    const std::vector<int>& roots, std::vector<int>* folded = nullptr);

}  // namespace stanli

#endif
