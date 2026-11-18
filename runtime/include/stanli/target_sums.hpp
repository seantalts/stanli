#ifndef STANLI_TARGET_SUMS_HPP
#define STANLI_TARGET_SUMS_HPP

#include <stanli/graph.hpp>

#include <utility>
#include <vector>

namespace stanli {

// Replaces a target term that is a chain of scalar additions, such as a
// local `lp += f(n)` returned from a reduce_sum partial sum, with the
// chain's operands as separate target terms. Re-roll and partition can
// then vectorize the per-element terms. Only additions whose results
// nothing else reads are removed, and a +0.0 constant operand is dropped.
// Returns the number of additions removed.
int split_target_sums(
    Graph& g, const std::vector<std::pair<int, std::vector<double>>>& fills,
    std::vector<int>& target_terms, const std::vector<int>& roots);

}  // namespace stanli
#endif
