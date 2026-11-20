// Long sums that stay accurate to a few ULP.
//
// When a pass merges a loop of scalar calls into one vector call, one kernel
// adds thousands of terms into one value. A plain running sum then loses up
// to about n/2 ULP, and is often less accurate than CmdStan, which adds the
// same terms one call at a time. A pairwise sum splits the range in halves,
// so its error grows with log2(n) instead of n.
//
// Up to kPairwiseBlock terms are added one at a time, in the order that the
// kernel always used. That keeps short sums bit for bit equal to the scalar
// operations and Stan Math calls that they replace.
#ifndef STANLI_PAIRWISE_SUM_HPP
#define STANLI_PAIRWISE_SUM_HPP

#include <cmath>
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

// slots[slot(k)] += value(k) for k in [0, n), in the order of k. Grouping
// the values by slot for a pairwise sum would cost time in proportion to the
// number of slots on every call, so a slot keeps the rounding error of its
// running sum by Neumaier's rule instead, and the error is added at the end
// to a slot that received more than kPairwiseBlock values. A slot that
// received fewer ends on the plain running sum. slot(k) must lie in
// [0, n_slots).
template <typename Slot, typename Value>
void scatter_add(double* slots, int64_t n_slots, int64_t n, const Slot& slot,
                 const Value& value) {
  if (n <= kPairwiseBlock) {  // no slot can receive a long sum
    for (int64_t k = 0; k < n; ++k) slots[slot(k)] += value(k);
    return;
  }
  struct Correction {
    double error = 0.0;
    int64_t count = 0;
  };
  // All zero between calls: finish() clears every entry that was used.
  thread_local std::vector<Correction> corrections;
  if (static_cast<int64_t>(corrections.size()) < n_slots)
    corrections.resize(static_cast<size_t>(n_slots));
  // A run of values for one slot is added in registers.
  for (int64_t k = 0; k < n;) {
    const auto s = static_cast<size_t>(slot(k));
    double sum = slots[s];
    Correction c = corrections[s];
    do {
      const double b = value(k), next = sum + b;
      c.error +=
          std::abs(sum) >= std::abs(b) ? (sum - next) + b : (b - next) + sum;
      ++c.count;
      sum = next;
    } while (++k < n && static_cast<size_t>(slot(k)) == s);
    slots[s] = sum;
    corrections[s] = c;
  }
  // Visit each used entry once more, by slot or by value, whichever is fewer.
  const auto finish = [&](size_t s) {
    Correction& c = corrections[s];
    if (c.count > kPairwiseBlock) slots[s] += c.error;
    c = Correction{};
  };
  if (n_slots <= n) {
    for (int64_t s = 0; s < n_slots; ++s) finish(static_cast<size_t>(s));
  } else {
    for (int64_t k = 0; k < n; ++k) finish(static_cast<size_t>(slot(k)));
  }
}

}  // namespace stanli

#endif
