// Common-subexpression elimination over the op graph. Unrolled models emit
// the same computation many times (685 bit-identical BERNOULLI_LPMF ops in
// Mh_model); the second and later copies become references to the first.
#ifndef STANLI_CSE_HPP
#define STANLI_CSE_HPP

#include <stanli/graph.hpp>

#include <utility>
#include <vector>

namespace stanli {

struct CseStats {
  int ops_removed = 0;
};

// In place. `target_terms` entries naming a removed op's output are rewritten
// to the surviving output; duplicates in that list are fine, since the term
// reduction sums the same slot twice for the two terms it stands for.
//
// `extra_roots` must list every slot something outside the op graph reads
// (jacobian terms, constrained-parameter views), as reroll's must: those are
// never renamed away. `fills` is read to keep bind-time-filled slots alive,
// since their buffers are sized from the slot length.
// STANLI_NO_CSE=1 disables the pass.
CseStats cse(Graph& g,
             const std::vector<std::pair<int, std::vector<double>>>& fills,
             std::vector<int>& target_terms,
             const std::vector<int>& extra_roots);

}  // namespace stanli

#endif
