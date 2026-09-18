// Helpers shared by the graph passes. Not installed.
#ifndef STANLI_PASS_UTIL_HPP
#define STANLI_PASS_UTIL_HPP

#include <stanli/graph.hpp>
#include <stanli/optable.hpp>

#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace stanli {

using Fills = std::vector<std::pair<int, std::vector<double>>>;

// The lane-fusing passes' shared currencies (island.cpp's dispatch cost
// against its per-element cost): ~5 ns per graph op eliminated, against
// ~1 ns per element a slice or gather copies or scatters, and ~6 op
// dispatches to evaluate one density element. A fused region must beat
// its unpacked cost by more than a rounding error, or it churns the graph
// for nothing; kPartitionMargin is that margin.
constexpr int64_t kLaneOpCost = 5;
constexpr int64_t kLanePartitionMargin = 8 * kLaneOpCost;
constexpr int64_t kLaneDensityElem = 6;

// Densities whose elementwise form (densities_lpmf.cpp) costs per element
// what their summed one does, so evaluating one elementwise instead of
// vectorized costs nothing extra. Every other density trades one
// vectorized call for W recorder calls, which this does price.
inline bool lane_elt_costs_per_element(uint16_t opcode) {
  switch (opcode) {
    case OP_BERNOULLI_LPMF:
    case OP_BERNOULLI_LOGIT_LPMF:
    case OP_BINOMIAL_LPMF:
    case OP_BINOMIAL_LOGIT_LPMF:
      return false;
    default:
      return true;
  }
}

inline bool is_element_store(const Op& op) {
  return (op.opcode == OP_SET_INDEX || op.opcode == OP_SET_INDEX_INPLACE) &&
         op.n_in == 2 && op.n_idata == 1 && op.out == op.in[0];
}

struct Key {
  std::vector<int64_t> w;
  bool operator==(const Key& o) const { return w == o.w; }
};

struct KeyHash {
  size_t operator()(const Key& k) const {
    size_t h = 1469598103934665603ull;
    for (int64_t v : k.w) {
      h ^= static_cast<size_t>(v);
      h *= 1099511628211ull;
    }
    return h;
  }
};

inline std::vector<size_t>::const_iterator first_at_or_after(
    const std::vector<size_t>& v, size_t x, int64_t& steps) {
  size_t lo = 0, hi = v.size();
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    ++steps;
    if (v[mid] < x)
      lo = mid + 1;
    else
      hi = mid;
  }
  return v.begin() + (ptrdiff_t)lo;
}

inline bool any_at_or_after(const std::vector<size_t>& v, size_t x,
                            int64_t& steps) {
  return first_at_or_after(v, x, steps) != v.end();
}

inline std::unordered_map<int, double> scalar_constants(const Fills& fills) {
  std::unordered_map<int, double> const_val;
  for (const auto& f : fills)
    if (f.second.size() == 1) const_val.emplace(f.first, f.second[0]);
  return const_val;
}

}  // namespace stanli

#endif
