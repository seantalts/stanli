// Re-roll pass: rewrites unrolled-loop regions (periodic op templates over
// consecutive lanes) into vectorized ops. Runs after lowering, before the
// target-term reduction, so N scalar density terms can become one summed
// vector-density term.
#ifndef STANLI_REROLL_HPP
#define STANLI_REROLL_HPP

#include <stanli/graph.hpp>

#include <cstdint>
#include <utility>
#include <vector>

namespace stanli {

struct RerollStats {
  int regions = 0;
  // Entries of the per-slot use and writer lists the pass read. This is
  // the term that was once quadratic in the op count, and it is what
  // tests/test_reroll.cpp asserts near-linearity on: an exact integer,
  // unlike a wall-clock reading, so the same graph gives the same answer
  // on every machine.
  int64_t list_steps = 0;
};

// In place. `fills` gains entries for materialized constant vectors.
// `target_terms` entries produced by vectorized densities are replaced by
// the single summed output slot (at the first lane's position). Slots are
// appended to g.slots; callers with arrays parallel to slots must resize.
// STANLI_NO_REROLL=1 disables the pass.
//
// `extra_roots` must list every slot something outside the op graph reads:
// jacobian terms and constrained-parameter views, which the executor reads
// directly and which therefore have no consuming op to find. The pass
// refuses to fold a region that would stop writing one. Passing an
// incomplete list is a miscompile, so this is not defaulted.
RerollStats reroll(Graph& g,
                   std::vector<std::pair<int, std::vector<double>>>& fills,
                   std::vector<int>& target_terms,
                   const std::vector<int>& extra_roots);

}  // namespace stanli

#endif
