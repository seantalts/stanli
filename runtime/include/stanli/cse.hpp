// Common-subexpression elimination over the op graph. Unrolled models emit
// the same computation many times. Inactive duplicates become references to
// the first. Active duplicates share only the forward value and scratch;
// each keeps its own adjoint and its original place in the reverse sweep.
#ifndef STANLI_CSE_HPP
#define STANLI_CSE_HPP

#include <stanli/graph.hpp>

#include <utility>
#include <vector>

namespace stanli {

struct CseStats {
  int ops_removed = 0;
  int primals_shared = 0;
};

// In place. Only inactive target terms are renamed. Active terms retain
// their source identity so combining their seeds cannot reassociate a VJP.
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
