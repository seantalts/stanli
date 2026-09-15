// Private preparation diagnostics for the re-roll pass. This header is not
// installed; the public RerollStats layout and reroll() entrypoint stay
// unchanged.
#ifndef STANLI_REROLL_PROFILE_HPP
#define STANLI_REROLL_PROFILE_HPP

#include <stanli/reroll.hpp>

namespace stanli {
namespace detail {

constexpr int64_t kMinLanes = 4;
constexpr int kMaxPeriod = 32;

struct RerollDispositionStats {
  int64_t packed_rows = 0;
  int64_t term_density = 0;
  int64_t element_density = 0;
  int64_t term_widen = 0;
  int64_t element_store = 0;
};

struct ProfiledRerollStats {
  RerollStats work;
  RerollDispositionStats dispositions;
};

// The lowerer calls this only for STANLI_PROFILE_PREP. Ordinary compilation
// continues through the public reroll() function and does not count
// dispositions.
ProfiledRerollStats reroll_profiled(
    Graph& g, std::vector<std::pair<int, std::vector<double>>>& fills,
    std::vector<int>& target_terms, const std::vector<int>& extra_roots);

struct SignatureCheckResult {
  int64_t pairs_checked = 0;
  int64_t violations = 0;
};

// Over every pair of ops within `window` positions of each other, and a
// spread of lane distances, checks that ops_match(g, a, b, distance) true
// implies the two ops' signature prefilter keys are equal.
SignatureCheckResult check_signature_soundness(const Graph& g, int64_t window);

}  // namespace detail
}  // namespace stanli

#endif
