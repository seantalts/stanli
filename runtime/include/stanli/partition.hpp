// Lane bucketing: rewrites repeated per-observation work into vector ops
// without requiring the repeats to be adjacent or periodic. Re-roll asks
// "does a template repeat with period P"; this asks "where does a lane end",
// segments on that, and groups the lanes by structure.
#ifndef STANLI_PARTITION_HPP
#define STANLI_PARTITION_HPP

#include <stanli/graph.hpp>

#include <cstdint>
#include <utility>
#include <vector>

namespace stanli {

struct PartitionStats {
  int groups = 0;    // buckets rewritten
  int lanes = 0;     // lanes absorbed
  int declined = 0;  // buckets refused, by the cost model or a legality rule
  // Deterministic work, asserted as a ratio between two problem sizes by
  // tests/test_partition.cpp: a wall-clock reading says different things on
  // a laptop and a shared CI runner, and these do not.
  int64_t segment_steps = 0;      // ops visited while finding lane bounds
  int64_t fingerprint_steps = 0;  // lane ops hashed and classified
  int64_t list_steps = 0;         // per-slot use/writer entries read, probes
                                  //   included: the term that once made
                                  //   re-roll quadratic on ldaK5
};

// In place, after re-roll and its in-place re-run, before the island carver.
// `fills` gains entries for materialized constant vectors. `target_terms`
// entries produced by a fused lane are replaced by the single reduction's
// output, at the first dead entry's position.
//
// `extra_roots` must list every slot something outside the op graph reads
// (jacobian terms, constrained-parameter views), as re-roll's and CSE's must:
// they have no consuming op, so a lane writing one cannot be proven dead.
// Passing an incomplete list is a miscompile, so this is not defaulted.
// STANLI_NO_PARTITION=1 disables the pass.
PartitionStats partition_lanes(
    Graph& g, std::vector<std::pair<int, std::vector<double>>>& fills,
    std::vector<int>& target_terms, const std::vector<int>& extra_roots);

}  // namespace stanli

#endif
