// Observation collapse (fast mode): with the data in hand, many likelihood
// terms evaluate the same expression for many observations. This pass finds
// them and evaluates each distinct row once, weighted by how often it
// occurs. Design:
// notes/performance/2026-10-06-sufficient-statistic-collapse.md.
#ifndef STANLI_COLLAPSE_HPP
#define STANLI_COLLAPSE_HPP

#include <stanli/graph.hpp>

#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace stanli {

// One likelihood term: a vector density op, or the scalar target terms one
// opcode produces.
struct CollapseTerm {
  int op = -1;  // index of the op, or of the first op of the scalar terms
  uint16_t opcode = 0;
  int64_t ops = 0;  // 1, or the number of scalar terms
  int64_t n = 0;    // observations
  // Rows that differ once every argument, the variate included, is compared
  // by value: what weighted evaluation keeps.
  int64_t rows = 0;
  // Rows that differ in the arguments other than the variate, for the
  // densities with a sufficient statistic (normal, lognormal) and a data
  // variate; -1 otherwise.
  int64_t groups = -1;
  bool variate_is_data = false;
  // Why the term is left alone, or nullptr if it collapses.
  const char* refusal = nullptr;
  // How it collapses: "rows" (each distinct row once, weighted), "groups"
  // (a sufficient statistic per group), "linear" (a quadratic form in the
  // values its locations are affine in), or nullptr.
  const char* evaluator = nullptr;
};

struct CollapseReport {
  std::vector<CollapseTerm> terms;
  // Ops carrying a payload this analysis cannot see into (compiled regions,
  // retained loops, reductions), by opcode: where a likelihood may be hiding.
  std::map<uint16_t, int64_t> opaque_ops;
};

struct CollapseStats {
  int vector_terms = 0;         // vector densities now evaluated per row
  int statistic_terms = 0;      // or per group, from a sufficient statistic
  int linear_terms = 0;         // or as one quadratic form
  int64_t observations = 0;     // elements those densities had
  int64_t rows = 0;             // elements they have now
  int scalar_terms_merged = 0;  // scalar target terms replaced by a weight
  int ops_removed = 0;
};

// Terms shorter than this are not worth a rewrite.
inline constexpr int64_t kCollapseMinObservations = 16;
// A density evaluated per row runs its elementwise form. Where that form
// costs one recorder call per element, the rows must be this many times
// fewer than the observations; elsewhere, half.
inline constexpr int64_t kCollapseRecorderRatio = 8;
inline constexpr int64_t kCollapseNativeRatio = 2;

// OP_NORMAL_GROUPED_LPDF: sum over groups of the normal log density of that
// group's observations, from per-group statistics.
//   in[0]  statistics, four columns of one value per group: count n, a
//          centre c near the group mean, d = sum(y - c), S = sum((y - c)^2)
//   in[1]  location, one per group or one for all
//   in[2]  scale, likewise
//   in[3]  a constant added to the result
// With r = c - mu, sum((y - mu)^2) = S + 2 r d + n r^2 exactly, whatever c
// is; keeping d is what lets c be a rounded mean without losing accuracy.
inline constexpr int kGroupedStatColumns = 4;
// Variant bits. Under propto the scale's log term is dropped when the scale
// is not active, as Stan drops it for a data scale.
inline constexpr uint8_t kGroupedLocationActive = 1 << 0;
inline constexpr uint8_t kGroupedScaleActive = 1 << 1;
inline constexpr uint8_t kGroupedLognormal = 1 << 2;  // names the checks
// The term was a normal_id_glm: its checks and their names.
inline constexpr uint8_t kGroupedGlm = 1 << 3;
inline constexpr uint8_t kGroupedPropto = 1 << 7;

// OP_LINEAR_GAUSSIAN_LPDF: the normal log density of observations whose
// locations are affine in a parameter vector theta, with one scale. Around a
// centre t near the least-squares solution, with u = theta - t,
//   sum((y - mu)^2) = K - 2 u.b + |R u|^2
// exactly, whatever t is, so the centre needs only to be close for the sum
// to be accurate near the mode.
//   in[0]  K, the observation count, t (P values), b (P values), then R:
//          upper triangular, row by row, P (P + 1) / 2 values
//   in[1]  theta, P values
//   in[2]  scale
//   in[3]  a constant added to the result
// Variant bits are the grouped density's; the location bit is theta's.
inline constexpr int kLinearGaussianHeader = 2;
inline int64_t linear_gaussian_data_len(int64_t p) {
  return kLinearGaussianHeader + 2 * p + p * (p + 1) / 2;
}

// Read-only. `target_terms` are the slots summed into the log density;
// `fills` are the bind-time data values. Two elements count as equal only
// if they are the same expression over the same parameter elements and
// bitwise-equal data, so an op this analysis does not model can only keep
// rows apart.
CollapseReport analyze_collapse(
    const Graph& g,
    const std::vector<std::pair<int, std::vector<double>>>& fills,
    const std::vector<int>& target_terms);

// In place. A collapsing vector density keeps one element per distinct row
// and sums them by weight; scalar target terms with equal values are
// computed once and multiplied by their count, when that saves more than
// the multiplies cost. The log
// density and its gradient change only by rounding. `extra_roots` lists every
// slot something outside the op graph reads, as for cse(). STANLI_NO_COLLAPSE=1
// disables the pass.
CollapseStats collapse_observations(
    Graph& g, std::vector<std::pair<int, std::vector<double>>>& fills,
    std::vector<int>& target_terms, const std::vector<int>& extra_roots,
    CollapseReport* report = nullptr);

// One "COLLAPSE {json}" line per term and per opaque opcode, on stderr.
void print_collapse_report(const CollapseReport& report, const char* graph);

}  // namespace stanli

#endif
