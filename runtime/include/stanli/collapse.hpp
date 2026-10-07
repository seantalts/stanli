// Observation collapse (fast mode): with the data in hand, many likelihood
// terms evaluate the same expression for many observations. This header is
// the analysis half: it finds the terms and counts how far each would
// collapse. Nothing is rewritten yet. Design:
// notes/performance/2026-10-06-sufficient-statistic-collapse.md.
#ifndef STANLI_COLLAPSE_HPP
#define STANLI_COLLAPSE_HPP

#include <stanli/graph.hpp>

#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace stanli {

// One likelihood term: a vector density op, or a family of scalar density
// ops with the same opcode and variant.
struct CollapseTerm {
  int op = -1;  // index of the op, or of the family's first op
  uint16_t opcode = 0;
  int64_t ops = 0;  // 1, or the family's size
  int64_t n = 0;    // observations
  // Rows that differ once every argument, the variate included, is compared
  // by value: what weighted evaluation would keep.
  int64_t rows = 0;
  // Rows that differ in the arguments other than the variate, for the
  // densities with a sufficient statistic (normal, lognormal) and a data
  // variate; -1 otherwise.
  int64_t groups = -1;
  bool variate_is_data = false;
  // Why the term is left alone, or nullptr if it would collapse.
  const char* refusal = nullptr;
};

struct CollapseReport {
  std::vector<CollapseTerm> terms;
  // Ops carrying a payload this analysis cannot see into (compiled regions,
  // retained loops, reductions), by opcode: where a likelihood may be hiding.
  std::map<uint16_t, int64_t> opaque_ops;
};

// Terms shorter than this are not worth a rewrite.
inline constexpr int64_t kCollapseMinObservations = 16;

// Read-only. `target_terms` are the slots summed into the log density;
// `fills` are the bind-time data values. Two elements count as equal only
// if they are the same expression over the same parameter elements and
// bitwise-equal data, so an op this analysis does not model can only keep
// rows apart.
CollapseReport analyze_collapse(
    const Graph& g,
    const std::vector<std::pair<int, std::vector<double>>>& fills,
    const std::vector<int>& target_terms);

// One "COLLAPSE {json}" line per term and per opaque opcode, on stderr.
void print_collapse_report(const CollapseReport& report, const char* graph);

}  // namespace stanli

#endif
