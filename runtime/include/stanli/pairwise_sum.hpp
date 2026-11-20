// Sums whose rounding error grows with log2(n) rather than with n.
//
// When a pass merges a loop of scalar calls into one vector call, one kernel
// adds thousands of terms into one value. A plain running sum then loses up
// to about n/2 ULP, and is often less accurate than CmdStan, which adds the
// same terms one call at a time. Splitting the range in halves keeps the
// error to a few ULP at any length.
//
// Up to kPairwiseBlock terms are added one at a time, in the order that the
// kernel always used. That keeps short sums bit for bit equal to the scalar
// operations and Stan Math calls that they replace.
#ifndef STANLI_PAIRWISE_SUM_HPP
#define STANLI_PAIRWISE_SUM_HPP

#include <cstdint>
#include <vector>

namespace stanli {

inline constexpr int64_t kPairwiseBlock = 8;

// term(begin) + ... + term(end - 1): blocks of up to kPairwiseBlock terms in
// order, and the blocks pairwise.
template <typename Term>
double pairwise_sum(int64_t begin, int64_t end, const Term& term) {
  if (end - begin <= kPairwiseBlock) {
    double sum = 0.0;
    for (int64_t i = begin; i < end; ++i) sum += term(i);
    return sum;
  }
  const int64_t mid = begin + (end - begin) / 2;
  return pairwise_sum(begin, mid, term) + pairwise_sum(mid, end, term);
}

inline double pairwise_sum(const double* x, int64_t n) {
  return pairwise_sum(0, n, [x](int64_t i) { return x[i]; });
}

// acc += term(0) + ... + term(n - 1). A short sum goes into acc one term at a
// time; a longer one is summed pairwise and added once.
template <typename Term>
void add_sum(double& acc, int64_t n, const Term& term) {
  if (n <= kPairwiseBlock) {
    for (int64_t i = 0; i < n; ++i) acc += term(i);
    return;
  }
  acc += pairwise_sum(0, n, term);
}

// slots[slot(k)] += value(k) for k in [0, n). The values that share a slot
// are added to it with add_sum, in the order of k. slot(k) must lie in
// [0, n_slots).
template <typename Slot, typename Value>
void scatter_add_pairwise(double* slots, int64_t n_slots, int64_t n,
                          const Slot& slot, const Value& value) {
  std::vector<int64_t> start(static_cast<size_t>(n_slots) + 1, 0);
  for (int64_t k = 0; k < n; ++k) ++start[static_cast<size_t>(slot(k)) + 1];
  for (int64_t s = 0; s < n_slots; ++s) start[s + 1] += start[s];
  std::vector<int64_t> fill(start.begin(), start.end() - 1);
  std::vector<double> grouped(static_cast<size_t>(n));
  for (int64_t k = 0; k < n; ++k)
    grouped[static_cast<size_t>(fill[static_cast<size_t>(slot(k))]++)] =
        value(k);
  for (int64_t s = 0; s < n_slots; ++s) {
    const double* group = grouped.data() + start[s];
    add_sum(slots[s], start[s + 1] - start[s],
            [group](int64_t i) { return group[i]; });
  }
}

}  // namespace stanli

#endif
