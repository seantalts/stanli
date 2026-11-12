// Private constants and diagnostics for the re-roll pass. This header is not
// installed.
#ifndef STANLI_REROLL_PROFILE_HPP
#define STANLI_REROLL_PROFILE_HPP

#include <stanli/reroll.hpp>

namespace stanli {
namespace detail {

constexpr int64_t kMinLanes = 4;
constexpr int kMaxPeriod = 32;

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
