// Helpers shared by the graph passes. Not installed.
#ifndef STANLI_PASS_UTIL_HPP
#define STANLI_PASS_UTIL_HPP

#include <stanli/debug_switch.hpp>
#include <stanli/graph.hpp>
#include <stanli/message_sink.hpp>
#include <stanli/optable.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace stanli {

using Fills = std::vector<std::pair<int, std::vector<double>>>;

// The lane-fusing passes' shared currencies, whose dispatch cost the island
// carver also uses: ~5 ns per graph op eliminated, against
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
    uint64_t h = 1469598103934665603ull;
    for (int64_t v : k.w) {
      h ^= static_cast<uint64_t>(v);
      h *= 1099511628211ull;
    }
    return static_cast<size_t>(h);
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

// Replaces each slot that the term list names k > 1 times with one term,
// the slot times k, at the position of its first copy. The lane passes call
// this first: they fuse a lane's term once, so a slot named twice would
// lose its second copy.
inline void scale_repeated_terms(Graph& g, Fills& fills,
                                 std::vector<int>& target_terms) {
  std::unordered_map<int, int> copies;
  for (int t : target_terms) ++copies[t];
  if (copies.size() == target_terms.size()) return;
  if (debug_env("STANLI_LANE_REPORT"))
    emit_diagnostic("LANES {\"pass\":\"terms\",\"repeated\":" +
                    std::to_string(target_terms.size() - copies.size()) + "}");
  std::vector<int> scaled;
  scaled.reserve(copies.size());
  for (int t : target_terms) {
    int& k = copies[t];
    if (k == 1) scaled.push_back(t);
    if (k <= 1) continue;
    const int count = g.add_slot(1, false);
    fills.emplace_back(count, std::vector<double>{(double)k});
    const int term = g.add_slot(1, false);
    g.add_op(OP_MUL, {t, count}, term);
    scaled.push_back(term);
    k = 0;
  }
  target_terms = std::move(scaled);
}

// A fused region's lanes leave the target list for its one replacement, which
// takes the first dead entry's position; the rest of the list closes up around
// it. Regions are recorded as they are emitted and the list is rewritten once
// by apply, while `term_set` stays current for membership questions. Each
// slot must appear in the list at most once (scale_repeated_terms).
class TermRetirement {
 public:
  int64_t steps = 0;  // dead slots recorded plus list entries rewritten

  void retire(const std::vector<int>& dead, int new_term,
              std::unordered_set<int>& term_set) {
    const int region = static_cast<int>(replacement_.size());
    replacement_.push_back(new_term);
    for (int s : dead) {
      ++steps;
      claimed_.try_emplace(s, region);
      term_set.erase(s);
    }
    term_set.insert(new_term);
  }

  void apply(std::vector<int>& terms) {
    if (replacement_.empty()) return;
    std::vector<char> placed(replacement_.size(), 0);
    std::vector<int> next;
    next.reserve(terms.size());
    for (int s : terms) {
      ++steps;
      const auto it = claimed_.find(s);
      if (it == claimed_.end()) {
        next.push_back(s);
      } else if (!placed[(size_t)it->second]) {
        placed[(size_t)it->second] = 1;
        next.push_back(replacement_[(size_t)it->second]);
      }
    }
    terms = std::move(next);
  }

 private:
  std::unordered_map<int, int> claimed_;  // dead slot -> first region
  std::vector<int> replacement_;
};

inline std::unordered_map<int, double> scalar_constants(const Fills& fills) {
  std::unordered_map<int, double> const_val;
  for (const auto& f : fills)
    if (f.second.size() == 1) const_val.emplace(f.first, f.second[0]);
  return const_val;
}

}  // namespace stanli

#endif
