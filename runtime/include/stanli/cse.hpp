// Common-subexpression elimination over the op graph. Unrolled models emit
// the same computation many times. Inactive duplicates become references to
// the first. Active duplicates share only the forward value and scratch;
// each keeps its own adjoint and its original place in the reverse sweep,
// unless their seeds are whole numbers (see cse()).
#ifndef STANLI_CSE_HPP
#define STANLI_CSE_HPP

#include <stanli/graph.hpp>

#include <utility>
#include <vector>

namespace stanli {

struct CseStats {
  int ops_removed = 0;
  int primals_shared = 0;
  int unread_dropped = 0;
};

// In place. Inactive target terms are renamed. An active value keeps its
// source identity, so combining seeds cannot reassociate a VJP, unless its
// seed is a whole number: it is read only as a target term or through
// scalar additions into one. Then its copies merge, since adding whole
// seeds is exact. A
// term listed k times afterwards becomes one term times k, which adds a
// constant to `fills`. Static reads (OP_INDEX, OP_SLICE, ...) and
// primal-sharing copies whose value nothing reads are dropped.
//
// `extra_roots` must list every slot something outside the op graph reads
// (jacobian terms, constrained-parameter views), as reroll's must: those are
// never renamed away. `fills` is read to keep bind-time-filled slots alive,
// since their buffers are sized from the slot length.
// STANLI_NO_CSE=1 disables the pass.
//
// `merge_active` (fast mode) also merges duplicates whose inputs depend on
// parameters, summing their seeds before one pullback.
CseStats cse(Graph& g, std::vector<std::pair<int, std::vector<double>>>& fills,
             std::vector<int>& target_terms,
             const std::vector<int>& extra_roots, bool merge_active = false);

}  // namespace stanli

#endif
