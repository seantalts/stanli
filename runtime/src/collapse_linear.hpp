// The numerical preparation behind the observation collapse's closed forms.
// Not installed. Kept apart from collapse.cpp so the graph pass does not
// compile a linear-algebra library.
#ifndef STANLI_COLLAPSE_LINEAR_HPP
#define STANLI_COLLAPSE_LINEAR_HPP

#include <cstdint>
#include <utility>
#include <vector>

namespace stanli {

// A sum that keeps its rounding errors.
struct Compensated {
  double hi = 0, lo = 0;
  void add(double x) {
    const double s = hi + x;
    const double v = s - hi;
    lo += (hi - (s - v)) + (x - v);
    hi = s;
  }
  // x * y, with the product's own rounding error.
  void add_product(double x, double y);
  double value() const { return hi + lo; }
};

// Per-group count, centre, sum(y - centre) and sum((y - centre)^2), laid out
// as OP_NORMAL_GROUPED_LPDF reads them. The centre is the rounded group mean;
// the two sums are accumulated with their rounding errors, so the density is
// as accurate as the data allow whatever the data's offset from zero.
std::vector<double> grouped_statistics(const std::vector<double>& y,
                                       const std::vector<int>& group_of,
                                       size_t groups);

// What one observation adds to its group's statistics under a
// CollapseFamily: t[0] and t[1] are its terms of the two sums, t[2] its term
// of the constant Stan keeps only without propto. `second` is the trial
// count for the binomials and ignored otherwise. False when the observation
// is one the closed form cannot stand in for (outside the support, or on
// its edge where the density is not finite).
bool family_observation(int family, double y, double second, double t[3]);
// The CollapseFamily of a density opcode, or -1; and how many parameters it
// takes.
int family_of_opcode(uint16_t opcode);
int family_parameters(int family);

// One group's location as constant + sum(coefficient * theta[index]).
struct LinearRow {
  double constant = 0;
  std::vector<std::pair<int, double>> terms;
};

// The data of OP_LINEAR_GAUSSIAN_LPDF for `rows` over `p` parameters, given
// the groups' statistics in grouped_statistics' layout. Empty when there are
// fewer groups than parameters or the least-squares centre is not finite.
std::vector<double> linear_gaussian_data(int64_t p,
                                         const std::vector<LinearRow>& rows,
                                         const std::vector<double>& statistics);

}  // namespace stanli

#endif
