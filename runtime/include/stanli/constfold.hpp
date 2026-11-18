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

// An op whose output lowering already computed, and that output's value in
// graph order.
struct KnownOutput {
  Op op;
  std::vector<double> value;
};

// Evaluate every constant op once and delete it, appending a fill for each
// folded slot a surviving op (or a root) still reads. A constant op that
// matches one in `known` is not run again: its recorded value is used.
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
    const std::vector<int>& roots, const std::vector<KnownOutput>& known = {});

}  // namespace stanli

#endif
