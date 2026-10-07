// Observation collapse, analysis half: which likelihood terms repeat over
// the data, and how far each would collapse. Nothing is rewritten, so these
// graphs are built and inspected, never executed.
#include <stanli/collapse.hpp>
#include <stanli/graph.hpp>
#include <stanli/optable.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

static int failures = 0;
static void expect(const char* what, bool ok) {
  if (!ok) {
    ++failures;
    std::printf("FAIL %s\n", what);
  }
}

using namespace stanli;
using Fills = std::vector<std::pair<int, std::vector<double>>>;

constexpr int kN = 20;

static bool refused(const CollapseTerm& t, const char* why) {
  return t.refusal != nullptr && std::strcmp(t.refusal, why) == 0;
}

// Twenty distinct observations.
static std::vector<double> distinct_y() {
  std::vector<double> y;
  for (int i = 0; i < kN; ++i) y.push_back(0.25 * i - 1.5);
  return y;
}

static int data_slot(Graph& g, Fills& fills, std::vector<double> v) {
  const int s = g.add_slot((int64_t)v.size(), false);
  fills.emplace_back(s, std::move(v));
  return s;
}

// y[n] ~ normal(a[group[n]], sigma): three groups, twenty distinct rows.
static void test_gathered_mean_groups() {
  Graph g;
  Fills fills;
  const int a = g.add_slot(3, true), sigma = g.add_slot(1, true);
  const int y = data_slot(g, fills, distinct_y());
  const int mu = g.add_slot(kN, false), lp = g.add_slot(1, false);
  std::vector<int> group;
  for (int i = 0; i < kN; ++i) group.push_back(i % 3);
  g.add_op(OP_GATHER, {a}, mu, group);
  const int dens = g.add_op(OP_NORMAL_LPDF, {y, mu, sigma}, lp);
  const CollapseReport r = analyze_collapse(g, fills, {lp});
  expect("gather: one term", r.terms.size() == 1);
  if (r.terms.size() != 1) return;
  const CollapseTerm& t = r.terms[0];
  expect("gather: names the density op", t.op == dens && t.ops == 1);
  expect("gather: twenty observations", t.n == kN);
  expect("gather: every row differs by its variate", t.rows == kN);
  expect("gather: three groups", t.groups == 3);
  expect("gather: data variate", t.variate_is_data);
  expect("gather: collapses", t.refusal == nullptr);
}

// mu = a + b * x with x taking four values.
static void test_repeated_covariate_groups() {
  Graph g;
  Fills fills;
  const int a = g.add_slot(1, true), b = g.add_slot(1, true);
  const int sigma = g.add_slot(1, true);
  std::vector<double> x;
  for (int i = 0; i < kN; ++i) x.push_back(60.0 + (i % 4));
  const int xs = data_slot(g, fills, x);
  const int y = data_slot(g, fills, distinct_y());
  const int bx = g.add_slot(kN, false), mu = g.add_slot(kN, false);
  const int lp = g.add_slot(1, false);
  g.add_op(OP_MUL, {b, xs}, bx);
  g.add_op(OP_ADD, {a, bx}, mu);
  g.add_op(OP_NORMAL_LPDF, {y, mu, sigma}, lp);
  const CollapseReport r = analyze_collapse(g, fills, {lp});
  expect("covariate: one term", r.terms.size() == 1);
  if (r.terms.size() != 1) return;
  expect("covariate: four groups", r.terms[0].groups == 4);
  expect("covariate: collapses", r.terms[0].refusal == nullptr);
}

// Data are compared bitwise: -0.0 and 0.0 are different rows.
static void test_data_equality_is_bitwise() {
  Graph g;
  Fills fills;
  const int b = g.add_slot(1, true), sigma = g.add_slot(1, true);
  std::vector<double> x(kN, 0.0);
  for (int i = 0; i < kN; i += 2) x[(size_t)i] = -0.0;
  const int xs = data_slot(g, fills, x);
  const int y = data_slot(g, fills, distinct_y());
  const int mu = g.add_slot(kN, false), lp = g.add_slot(1, false);
  g.add_op(OP_MUL, {b, xs}, mu);
  g.add_op(OP_NORMAL_LPDF, {y, mu, sigma}, lp);
  const CollapseReport r = analyze_collapse(g, fills, {lp});
  expect("signed zero: one term", r.terms.size() == 1);
  if (r.terms.size() != 1) return;
  expect("signed zero: two groups", r.terms[0].groups == 2);
}

// Integer outcomes have no statistic here; rows are (outcome, theta).
static void test_integer_outcomes_count_rows() {
  Graph g;
  Fills fills;
  const int b = g.add_slot(2, true);
  const int eta = g.add_slot(kN, false), theta = g.add_slot(kN, false);
  const int lp = g.add_slot(1, false);
  std::vector<int> group, outcome;
  for (int i = 0; i < kN; ++i) {
    group.push_back(i % 2);
    outcome.push_back(i % 4 < 2);
  }
  g.add_op(OP_GATHER, {b}, eta, group);
  g.add_op(OP_INV_LOGIT, {eta}, theta);
  g.add_op(OP_BERNOULLI_LPMF, {theta}, lp, outcome);
  const CollapseReport r = analyze_collapse(g, fills, {lp});
  expect("bernoulli: one term", r.terms.size() == 1);
  if (r.terms.size() != 1) return;
  const CollapseTerm& t = r.terms[0];
  expect("bernoulli: four rows", t.n == kN && t.rows == 4);
  expect("bernoulli: no statistic", t.groups == -1);
  expect("bernoulli: collapses", t.refusal == nullptr);
}

// The predictor built one scalar at a time and stored into a vector, as an
// unrolled loop lowers; binomial outcomes arrive as two [len, vals] groups.
static void test_scalar_chains_into_a_vector() {
  Graph g;
  Fills fills;
  const int a = g.add_slot(4, true);
  const int p = data_slot(
      g, fills,
      std::vector<double>(kN, std::numeric_limits<double>::quiet_NaN()));
  for (int i = 0; i < kN; ++i) {
    const int ai = g.add_slot(1, false), pi = g.add_slot(1, false);
    g.add_op(OP_INDEX, {a}, ai, {i % 4});
    g.add_op(OP_INV_LOGIT, {ai}, pi);
    g.add_op(OP_SET_INDEX_INPLACE, {p, pi}, p, {i});
  }
  std::vector<int> idata{kN};
  for (int i = 0; i < kN; ++i) idata.push_back(i % 2);  // successes
  idata.push_back(-1);  // trials: one language-level scalar
  idata.push_back(5);
  const int lp = g.add_slot(1, false);
  g.add_op(OP_BINOMIAL_LPMF, {p}, lp, idata);
  const CollapseReport r = analyze_collapse(g, fills, {lp});
  expect("chains: one term", r.terms.size() == 1);
  if (r.terms.size() != 1) return;
  const CollapseTerm& t = r.terms[0];
  expect("chains: twenty observations", t.n == kN);
  // i % 4 fixes i % 2, so the four predictors give four rows.
  expect("chains: four rows", t.rows == 4);
  expect("chains: collapses", t.refusal == nullptr);
}

// Twenty scalar densities are one term; equal data in different slots is
// the same row.
static void test_scalar_family() {
  Graph g;
  Fills fills;
  const int a = g.add_slot(2, true), sigma = g.add_slot(1, true);
  std::vector<int> terms;
  int first = -1;
  for (int i = 0; i < kN; ++i) {
    const int y = data_slot(g, fills, {i % 5 < 2 ? 1.5 : -0.5});
    const int mu = g.add_slot(1, false), lp = g.add_slot(1, false);
    g.add_op(OP_INDEX, {a}, mu, {i % 2});
    const int dens = g.add_op(OP_NORMAL_LPDF, {y, mu, sigma}, lp);
    if (first < 0) first = dens;
    terms.push_back(lp);
  }
  const CollapseReport r = analyze_collapse(g, fills, terms);
  expect("family: one term", r.terms.size() == 1);
  if (r.terms.size() != 1) return;
  const CollapseTerm& t = r.terms[0];
  expect("family: names its first op", t.op == first);
  expect("family: twenty ops, twenty observations", t.ops == kN && t.n == kN);
  expect("family: four rows", t.rows == 4);
  expect("family: collapses", t.refusal == nullptr);
}

// X * beta with four distinct rows of X.
static void test_matvec_rows() {
  Graph g;
  Fills fills;
  const int beta = g.add_slot(2, true), sigma = g.add_slot(1, true);
  std::vector<double> x((size_t)(2 * kN));
  for (int i = 0; i < kN; ++i) {
    x[(size_t)i] = 1.0;                   // column 0
    x[(size_t)(kN + i)] = 0.5 * (i % 4);  // column 1
  }
  const int xs = data_slot(g, fills, x);
  const int y = data_slot(g, fills, distinct_y());
  const int mu = g.add_slot(kN, false), lp = g.add_slot(1, false);
  g.add_op(OP_MATVEC, {xs, beta}, mu, {kN, 2});
  g.add_op(OP_NORMAL_LPDF, {y, mu, sigma}, lp);
  const CollapseReport r = analyze_collapse(g, fills, {lp});
  expect("matvec: one term", r.terms.size() == 1);
  if (r.terms.size() != 1) return;
  expect("matvec: four groups", r.terms[0].groups == 4);
  expect("matvec: collapses", r.terms[0].refusal == nullptr);
}

// A predictor copied into a declared vector and read back in pieces, as a
// model that assigns `vector[N] mu = ...` lowers.
static void test_slices_carry_values() {
  Graph g;
  Fills fills;
  const int a = g.add_slot(2, true), sigma = g.add_slot(1, true);
  const int y = data_slot(g, fills, distinct_y());
  const int picked = g.add_slot(kN, false);
  const int decl = data_slot(
      g, fills,
      std::vector<double>(2 * kN, std::numeric_limits<double>::quiet_NaN()));
  const int stored = g.add_slot(2 * kN, false);
  const int evens = g.add_slot(kN, false), tail = g.add_slot(kN, false);
  const int lp1 = g.add_slot(1, false), lp2 = g.add_slot(1, false);
  std::vector<int> group;
  for (int i = 0; i < kN; ++i) group.push_back(i % 2);
  g.add_op(OP_GATHER, {a}, picked, group);
  // Even positions, then the second half: both hold the gathered values.
  g.add_op(OP_SET_SLICE_STRIDED, {decl, picked}, stored, {0, 2});
  g.add_op(OP_SET_SLICE_INPLACE, {stored, picked}, stored, {kN});
  g.add_op(OP_SLICE_STRIDED, {stored}, evens, {0, 2});
  g.add_op(OP_SLICE, {stored}, tail, {kN});
  g.add_op(OP_NORMAL_LPDF, {y, evens, sigma}, lp1);
  g.add_op(OP_NORMAL_LPDF, {y, tail, sigma}, lp2);
  const CollapseReport r = analyze_collapse(g, fills, {lp1, lp2});
  expect("slices: two terms", r.terms.size() == 2);
  if (r.terms.size() != 2) return;
  // The strided store wrote positions 0, 2, .., 38; the contiguous one then
  // overwrote 20..39. Evens below 20 keep the gathered values and evens
  // from 20 on read gathered[i - 20], so both terms still see two groups.
  expect("slices: strided read sees two groups", r.terms[0].groups == 2);
  expect("slices: contiguous read sees two groups", r.terms[1].groups == 2);
}

// A store between two densities changes what the second one reads.
static void test_a_later_store_is_a_later_value() {
  Graph g;
  Fills fills;
  const int b = g.add_slot(1, true), c = g.add_slot(1, true);
  const int sigma = g.add_slot(1, true);
  const int xs = data_slot(g, fills, std::vector<double>(kN, 2.0));
  const int y = data_slot(g, fills, distinct_y());
  const int mu = g.add_slot(kN, false);
  const int lp1 = g.add_slot(1, false), lp2 = g.add_slot(1, false);
  g.add_op(OP_MUL, {b, xs}, mu);
  g.add_op(OP_NORMAL_LPDF, {y, mu, sigma}, lp1);
  g.add_op(OP_SET_INDEX_INPLACE, {mu, c}, mu, {7});
  g.add_op(OP_NORMAL_LPDF, {y, mu, sigma}, lp2);
  const CollapseReport r = analyze_collapse(g, fills, {lp1, lp2});
  expect("store: two terms", r.terms.size() == 2);
  if (r.terms.size() != 2) return;
  expect("store: one group before", r.terms[0].groups == 1);
  expect("store: two groups after", r.terms[1].groups == 2);
}

// An op the analysis does not model keeps every row apart.
static void test_unknown_producer_keeps_rows_apart() {
  Graph g;
  Fills fills;
  const int a = g.add_slot(kN, true), sigma = g.add_slot(1, true);
  const int y = data_slot(g, fills, distinct_y());
  const int mu = g.add_slot(kN, false), lp = g.add_slot(1, false);
  g.add_op(OP_SOFTMAX, {a}, mu);
  g.add_op(OP_NORMAL_LPDF, {y, mu, sigma}, lp);
  const CollapseReport r = analyze_collapse(g, fills, {lp});
  expect("unknown: one term", r.terms.size() == 1);
  if (r.terms.size() != 1) return;
  expect("unknown: twenty groups", r.terms[0].groups == kN);
  expect("unknown: refused", refused(r.terms[0], "too few repeated rows"));
}

struct Refusal {
  int n = kN;
  bool nan = false;
  bool read_elsewhere = false;
  bool active_variate = false;
};

static CollapseReport refusal_case(const Refusal& c) {
  Graph g;
  Fills fills;
  const int a = g.add_slot(2, true), sigma = g.add_slot(1, true);
  std::vector<double> yv;
  for (int i = 0; i < c.n; ++i) yv.push_back(0.25 * i);
  if (c.nan) yv[3] = std::numeric_limits<double>::quiet_NaN();
  const int y =
      c.active_variate ? g.add_slot(c.n, true) : data_slot(g, fills, yv);
  const int mu = g.add_slot(c.n, false), lp = g.add_slot(1, false);
  std::vector<int> group;
  for (int i = 0; i < c.n; ++i) group.push_back(i % 2);
  g.add_op(OP_GATHER, {a}, mu, group);
  g.add_op(OP_NORMAL_LPDF, {y, mu, sigma}, lp);
  if (c.read_elsewhere) {
    const int twice = g.add_slot(1, false);
    g.add_op(OP_ADD, {lp, lp}, twice);
    return analyze_collapse(g, fills, {twice});
  }
  return analyze_collapse(g, fills, {lp});
}

static void test_refusals() {
  {
    const CollapseReport r = refusal_case({});
    expect("baseline: collapses", r.terms.size() == 1 &&
                                      r.terms[0].refusal == nullptr &&
                                      r.terms[0].groups == 2);
  }
  {
    Refusal c;
    c.n = kCollapseMinObservations - 1;
    const CollapseReport r = refusal_case(c);
    expect("short term refused",
           r.terms.size() == 1 && refused(r.terms[0], "too few observations"));
  }
  {
    Refusal c;
    c.nan = true;
    const CollapseReport r = refusal_case(c);
    expect("NaN variate refused",
           r.terms.size() == 1 && refused(r.terms[0], "NaN in the variate"));
  }
  {
    Refusal c;
    c.read_elsewhere = true;
    const CollapseReport r = refusal_case(c);
    expect("value read by another op refused",
           r.terms.size() == 1 &&
               refused(r.terms[0], "the term's value is read by another op"));
  }
  {
    Refusal c;
    c.active_variate = true;
    const CollapseReport r = refusal_case(c);
    expect("active variate has no statistic",
           r.terms.size() == 1 && !r.terms[0].variate_is_data &&
               r.terms[0].groups == -1 &&
               refused(r.terms[0], "too few repeated rows"));
  }
}

// A density of data alone is a constant, not a likelihood term.
static void test_data_only_density_is_not_a_term() {
  Graph g;
  Fills fills;
  const int p = g.add_slot(1, true);
  const int y = data_slot(g, fills, distinct_y());
  const int mu = data_slot(g, fills, {0.0}), sigma = data_slot(g, fills, {1.0});
  const int c = g.add_slot(1, false), lp = g.add_slot(1, false);
  g.add_op(OP_NORMAL_LPDF, {y, mu, sigma}, c);
  g.add_op(OP_SQUARE, {p}, lp);
  const CollapseReport r = analyze_collapse(g, fills, {c, lp});
  expect("data-only density: no terms", r.terms.empty());
}

int main() {
  test_gathered_mean_groups();
  test_repeated_covariate_groups();
  test_data_equality_is_bitwise();
  test_integer_outcomes_count_rows();
  test_scalar_chains_into_a_vector();
  test_scalar_family();
  test_matvec_rows();
  test_slices_carry_values();
  test_a_later_store_is_a_later_value();
  test_unknown_producer_keeps_rows_apart();
  test_refusals();
  test_data_only_density_is_not_a_term();
  if (failures == 0) std::printf("test_collapse OK\n");
  return failures == 0 ? 0 : 1;
}
