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

// sum() and mean() of up to kPlainSumMax terms keep their plain grouping,
// which is Stan Math's to the bit; longer ones are compensated
// (compensated_sum). A compensated sum costs about seven operations per
// term, so the cutoff gives up its accuracy on sums of up to 32 terms for
// speed. Measured on arm64, 181 independent sums of 32 terms take 4.4 ns each
// plain and 10.1 ns compensated, and a model that adds 181 sums of 11 terms
// had a 1.14x slower gradient with the cutoff at 8.
inline constexpr int64_t kPlainSumMax = 32;

// The sum that pairwise_sum forms when log2(Blocks) halvings, and no fewer,
// split every part of the range down to kPairwiseBlock terms or fewer. Each
// of the Blocks blocks then has m or m + 1 terms. The blocks are added side
// by side rather than one after another, so that the processor can overlap
// their additions, and no call separates them.
template <int Blocks, typename Term>
[[gnu::always_inline]] inline double pairwise_blocks(int64_t begin, int64_t end,
                                                     const Term& term) {
  int64_t start[Blocks + 1];
  start[0] = begin;
  start[Blocks] = end;
  for (int step = Blocks; step > 1; step /= 2) {
    for (int j = 0; j < Blocks; j += step)
      start[j + step / 2] = start[j] + (start[j + step] - start[j]) / 2;
  }
  const int64_t m = (end - begin) / Blocks;
  double sum[Blocks] = {};
  for (int64_t t = 0; t < m; ++t) {
    for (int j = 0; j < Blocks; ++j) sum[j] += term(start[j] + t);
  }
  for (int j = 0; j < Blocks; ++j)
    if (start[j] + m < start[j + 1]) sum[j] += term(start[j] + m);
  for (int width = Blocks; width > 1; width /= 2) {
    for (int j = 0; j < width / 2; ++j) sum[j] = sum[2 * j] + sum[2 * j + 1];
  }
  return sum[0];
}

// term(begin) + ... + term(end - 1): blocks of up to kPairwiseBlock terms in
// order, and the blocks pairwise.
template <typename Term>
double pairwise_sum(int64_t begin, int64_t end, const Term& term) {
  const int64_t n = end - begin;
  if (n <= kPairwiseBlock) {
    double sum = 0.0;
    for (int64_t i = begin; i < end; ++i) sum += term(i);
    return sum;
  }
  // The halving reaches its blocks at one level for every n up to
  // 8 * kPairwiseBlock except 17 and 33 to 35, which the recursion handles.
  if (n <= 2 * kPairwiseBlock) return pairwise_blocks<2>(begin, end, term);
  if (n <= 4 * kPairwiseBlock && n / 2 > kPairwiseBlock)
    return pairwise_blocks<4>(begin, end, term);
  if (n <= 8 * kPairwiseBlock && n / 4 > kPairwiseBlock)
    return pairwise_blocks<8>(begin, end, term);
  const int64_t mid = begin + n / 2;
  return pairwise_sum(begin, mid, term) + pairwise_sum(mid, end, term);
}

inline double pairwise_sum(const double* x, int64_t n) {
  return pairwise_sum(0, n, [x](int64_t i) { return x[i]; });
}

// x[0] + ... + x[n - 1] with Neumaier's compensation, which keeps the exact
// rounding error of every addition and adds the errors at the end. Knuth's
// two-sum finds each error without Neumaier's comparison. Four lanes each
// take every fourth term, and the lanes and their errors are combined the
// same way at the end; written out, the lanes vectorize and the result does
// not depend on the vector width. The result is within about
// eps * |sum| + (n * eps)^2 * sum(|x|) of the exact sum (Ogita, Rump and
// Oishi's Sum2): close to correctly rounded unless the terms cancel to
// almost nothing. An infinite or NaN total is returned without its
// correction, which would be NaN, so non-finite terms give what a plain sum
// gives.
inline double compensated_sum(const double* x, int64_t n) {
  constexpr int kLanes = 4;
  const auto two_sum = [](double a, double b, double& sum, double& error) {
    sum = a + b;
    const double b_part = sum - a;
    error = (a - (sum - b_part)) + (b - b_part);
  };
  double sum[kLanes] = {}, error[kLanes] = {};
  int64_t i = 0;
  for (; i + kLanes <= n; i += kLanes) {
    for (int j = 0; j < kLanes; ++j) {
      double e;
      two_sum(sum[j], x[i + j], sum[j], e);
      error[j] += e;
    }
  }
  for (int j = 0; i < n; ++i, ++j) {
    double e;
    two_sum(sum[j], x[i], sum[j], e);
    error[j] += e;
  }
  for (int width = kLanes; width > 1; width /= 2) {
    for (int j = 0; j < width / 2; ++j) {
      double e;
      two_sum(sum[2 * j], sum[2 * j + 1], sum[j], e);
      error[j] = error[2 * j] + error[2 * j + 1] + e;
    }
  }
  return std::isfinite(sum[0]) ? sum[0] + error[0] : sum[0];
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
