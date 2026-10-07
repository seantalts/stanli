// Observation collapse: likelihood terms that repeat over the data are
// evaluated once per distinct row and weighted. Every rewritten graph is run
// against the graph it came from.
#include "env_helpers.hpp"
#include "graph_helpers.hpp"
#include <stanli/collapse.hpp>
#include <stanli/graph.hpp>
#include <stanli/optable.hpp>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
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
using stanli::testutil::Fills;

constexpr int kN = 20;  // enough for a density with a native elementwise form
constexpr int kWide = 64;  // enough for one that pays a recorder call per row

struct Model {
  Graph g;
  Fills fills;
  std::vector<int> terms;
  std::vector<int> roots;
};

// Positive, so the same point serves a scale parameter.
static double fill_at(int64_t i) { return 0.3 + 0.2 * (double)(i % 3); }

template <class FillAt>
static std::vector<double> evaluate_at(Model m, FillAt at) {
  testutil::reduce_into_result(m.g, m.terms);
  return testutil::run_grad(std::move(m.g), m.fills, at);
}

static std::vector<double> evaluate(Model m) { return evaluate_at(m, fill_at); }

// The fast-mode gate's metric: lp on its own, the gradient against its
// largest entry.
static bool close(const std::vector<double>& a, const std::vector<double>& b) {
  if (a.size() != b.size() || a.empty()) return false;
  double diff = 0, scale = 1;
  for (size_t i = 1; i < a.size(); ++i) {
    diff = std::max(diff, std::abs(a[i] - b[i]));
    scale = std::max({scale, std::abs(a[i]), std::abs(b[i])});
  }
  return std::abs(a[0] - b[0]) <= 1e-12 * std::max(1.0, std::abs(a[0])) &&
         diff <= 1e-12 * scale;
}

struct Collapsed {
  Model model;
  CollapseStats stats;
  CollapseReport report;
  bool same_values = false;
};

static Collapsed collapse(const Model& m) {
  Collapsed c;
  c.model = m;
  c.stats = collapse_observations(c.model.g, c.model.fills, c.model.terms,
                                  c.model.roots, &c.report);
  c.same_values = close(evaluate(m), evaluate(c.model));
  return c;
}

static int count_opcode(const Graph& g, uint16_t oc) {
  int n = 0;
  for (const Op& op : g.ops) n += op.opcode == oc;
  return n;
}

static const Op* find_op(const Graph& g, uint16_t oc) {
  for (const Op& op : g.ops)
    if (op.opcode == oc) return &op;
  return nullptr;
}

static int64_t out_len(const Graph& g, uint16_t oc) {
  const Op* op = find_op(g, oc);
  return op ? g.slots[(size_t)op->out].len : -1;
}

static bool refused(const CollapseTerm& t, const char* why) {
  return t.refusal != nullptr && std::strcmp(t.refusal, why) == 0;
}

static int data_slot(Model& m, std::vector<double> v) {
  const int s = m.g.add_slot((int64_t)v.size(), false);
  m.fills.emplace_back(s, std::move(v));
  return s;
}

static std::vector<double> distinct_y(int n) {
  std::vector<double> y;
  for (int i = 0; i < n; ++i) y.push_back(0.25 * i - 1.5);
  return y;
}

// y[n] ~ bernoulli(inv_logit(b[group[n]])): two groups, two outcomes.
static Model bernoulli_by_group(int n, int groups = 2) {
  Model m;
  const int b = m.g.add_slot(groups, true);
  const int eta = m.g.add_slot(n, false), theta = m.g.add_slot(n, false);
  const int lp = m.g.add_slot(1, false);
  std::vector<int> group, outcome;
  for (int i = 0; i < n; ++i) {
    group.push_back(i % groups);
    outcome.push_back(i % 4 < 2);
  }
  m.g.add_op(OP_GATHER, {b}, eta, group);
  m.g.add_op(OP_INV_LOGIT, {eta}, theta);
  m.g.add_op(OP_BERNOULLI_LPMF, {theta}, lp, outcome);
  m.terms = {lp};
  return m;
}

static void test_gathered_predictor() {
  const Collapsed c = collapse(bernoulli_by_group(kN));
  const Graph& g = c.model.g;
  expect("gather: one term over four rows", c.stats.vector_terms == 1 &&
                                                c.stats.observations == kN &&
                                                c.stats.rows == 4);
  const Op* dens = find_op(g, OP_BERNOULLI_LPMF);
  expect("gather: density is elementwise over the rows",
         dens && (dens->variant & 0x40u) && dens->n_idata == 4 &&
             g.slots[(size_t)dens->out].len == 4);
  expect("gather: predictor built at four elements",
         count_opcode(g, OP_INV_LOGIT) == 1 && out_len(g, OP_INV_LOGIT) == 4 &&
             count_opcode(g, OP_GATHER) == 1 && out_len(g, OP_GATHER) == 4);
  expect("gather: weighted by a dot product", count_opcode(g, OP_DOT) == 1);
  expect("gather: same log density and gradient", c.same_values);
}

// A slot something outside the graph reads keeps its full-length producer.
static void test_roots_stay() {
  Model m = bernoulli_by_group(kN);
  m.roots = {find_op(m.g, OP_INV_LOGIT)->out};
  const Collapsed c = collapse(m);
  expect("root: collapses", c.stats.vector_terms == 1);
  expect("root: full-length predictor kept beside the short one",
         count_opcode(c.model.g, OP_INV_LOGIT) == 2);
  expect("root: same values", c.same_values);
}

// The predictor is also summed into the target: both readers are served.
static void test_shared_predictor() {
  Model m = bernoulli_by_group(kN);
  const int total = m.g.add_slot(1, false);
  m.g.add_op(OP_SUM_VEC, {find_op(m.g, OP_INV_LOGIT)->out}, total);
  m.terms.push_back(total);
  const Collapsed c = collapse(m);
  expect("shared: collapses", c.stats.vector_terms == 1);
  expect("shared: both predictors present",
         count_opcode(c.model.g, OP_INV_LOGIT) == 2);
  expect("shared: same values", c.same_values);
}

static void test_one_row() {
  Model m = bernoulli_by_group(kN, 1);
  Op& dens = m.g.ops.back();
  std::vector<int> ones((size_t)kN, 1);
  m.g.idata_pool.push_back(ones);
  dens.idata = m.g.idata_pool.back().data();
  const Collapsed c = collapse(m);
  expect("one row: collapses to one element",
         c.stats.vector_terms == 1 && c.stats.rows == 1);
  expect("one row: same values", c.same_values);
}

// The predictor built one scalar at a time and stored into a vector, as an
// unrolled loop lowers; binomial outcomes arrive as two [len, vals] groups.
static void test_scalar_chains() {
  Model m;
  const int a = m.g.add_slot(4, true);
  const int decl = data_slot(
      m, std::vector<double>(kN, std::numeric_limits<double>::quiet_NaN()));
  const int p = m.g.add_slot(kN, false);
  for (int i = 0; i < kN; ++i) {
    const int ai = m.g.add_slot(1, false), pi = m.g.add_slot(1, false);
    m.g.add_op(OP_INDEX, {a}, ai, {i % 4});
    m.g.add_op(OP_INV_LOGIT, {ai}, pi);
    if (i == 0)
      m.g.add_op(OP_SET_INDEX, {decl, pi}, p, {i});
    else
      m.g.add_op(OP_SET_INDEX_INPLACE, {p, pi}, p, {i});
  }
  std::vector<int> idata{kN};
  for (int i = 0; i < kN; ++i) idata.push_back(i % 2);  // successes
  idata.push_back(-1);  // trials: one language-level scalar
  idata.push_back(5);
  const int lp = m.g.add_slot(1, false);
  m.g.add_op(OP_BINOMIAL_LPMF, {p}, lp, idata);
  m.terms = {lp};
  const Collapsed c = collapse(m);
  const Graph& g = c.model.g;
  // i % 4 fixes i % 2, so the four predictors give four rows.
  expect("chains: four rows", c.stats.vector_terms == 1 && c.stats.rows == 4);
  expect("chains: four scalar chains left",
         count_opcode(g, OP_INV_LOGIT) == 4 && count_opcode(g, OP_INDEX) == 4);
  const Op* dens = find_op(g, OP_BINOMIAL_LPMF);
  const std::vector<int> want{4, 0, 1, 0, 1, -1, 5};
  expect("chains: outcome groups restricted",
         dens && dens->n_idata == (int64_t)want.size() &&
             std::equal(want.begin(), want.end(), dens->idata));
  expect("chains: same values", c.same_values);
}

// The predictor filled one scalar at a time, each a[group] + b * x: the
// kept rows are rebuilt from the parameters with vector ops, not kept as
// one chain per row and packed.
static void test_scalar_chains_become_vector_ops() {
  Model m;
  const int a = m.g.add_slot(5, true), b = m.g.add_slot(1, true);
  const int sigma = m.g.add_slot(1, true);
  const int n = 60;
  const int y = data_slot(m, distinct_y(n));
  const int decl = data_slot(
      m, std::vector<double>(n, std::numeric_limits<double>::quiet_NaN()));
  const int mu = m.g.add_slot(n, false);
  for (int i = 0; i < n; ++i) {
    const int xs = data_slot(m, {i % 2 ? 1.0 : 0.25});
    const int ai = m.g.add_slot(1, false), mi = m.g.add_slot(1, false);
    m.g.add_op(OP_INDEX, {a}, ai, {i % 5});
    m.g.add_op(OP_FMA, {xs, b, ai}, mi);
    if (i == 0)
      m.g.add_op(OP_SET_INDEX, {decl, mi}, mu, {i});
    else
      m.g.add_op(OP_SET_INDEX_INPLACE, {mu, mi}, mu, {i});
  }
  const int lp = m.g.add_slot(1, false);
  m.g.add_op(OP_NORMAL_LPDF, {y, mu, sigma}, lp);
  m.g.ops.back().variant = 0x86;
  m.terms = {lp};
  const Collapsed c = collapse(m);
  const Graph& g = c.model.g;
  expect("chains to vectors: ten groups",
         c.stats.statistic_terms == 1 && c.stats.rows == 10);
  expect("chains to vectors: no scalar chains or packing left",
         count_opcode(g, OP_FMA) == 0 && count_opcode(g, OP_INDEX) == 0 &&
             count_opcode(g, OP_SET_INDEX) == 0 &&
             count_opcode(g, OP_SET_INDEX_INPLACE) == 0);
  expect("chains to vectors: one gather, one multiply, one add",
         count_opcode(g, OP_GATHER) == 1 && out_len(g, OP_GATHER) == 10 &&
             count_opcode(g, OP_MUL) == 1 && count_opcode(g, OP_ADD) == 1);
  expect("chains to vectors: same values", c.same_values);
}

// mu = a + b * x copied into a declared vector, as `vector[N] mu = ...`
// lowers, under a density that pays a recorder call per row.
static void test_copied_predictor() {
  Model m;
  const int a = m.g.add_slot(1, true), b = m.g.add_slot(1, true);
  std::vector<double> x;
  std::vector<int> y;
  for (int i = 0; i < kWide; ++i) {
    x.push_back(0.5 * (i % 2));
    y.push_back((i / 2) % 2 ? 3 : 1);
  }
  const int xs = data_slot(m, x);
  const int decl = data_slot(
      m, std::vector<double>(kWide, std::numeric_limits<double>::quiet_NaN()));
  const int bx = m.g.add_slot(kWide, false), mu = m.g.add_slot(kWide, false);
  const int stored = m.g.add_slot(kWide, false), lp = m.g.add_slot(1, false);
  m.g.add_op(OP_MUL, {b, xs}, bx);
  m.g.add_op(OP_ADD, {a, bx}, mu);
  m.g.add_op(OP_SET_SLICE, {decl, mu}, stored, {0});
  m.g.add_op(OP_POISSON_LOG_LPMF, {stored}, lp, y);
  m.terms = {lp};
  const Collapsed c = collapse(m);
  const Graph& g = c.model.g;
  expect("copy: four rows", c.stats.vector_terms == 1 && c.stats.rows == 4);
  expect("copy: the copy is gone", count_opcode(g, OP_SET_SLICE) == 0);
  expect("copy: predictor built at four elements",
         out_len(g, OP_MUL) == 4 && out_len(g, OP_ADD) == 4 &&
             count_opcode(g, OP_GATHER) == 0);
  expect("copy: same values", c.same_values);
}

// A real variate repeats too: rows are (y, mu). The mask of a density that
// left its variant unset must still mark every argument active.
static void test_real_variate_rows() {
  Model m;
  const int b = m.g.add_slot(1, true), sigma = m.g.add_slot(1, true);
  std::vector<double> x, y;
  for (int i = 0; i < kWide; ++i) {
    x.push_back(1.0 + (i % 2));
    y.push_back((i / 2) % 2 ? 1.5 : -0.5);
  }
  const int xs = data_slot(m, x), ys = data_slot(m, y);
  const int mu = m.g.add_slot(kWide, false), lp = m.g.add_slot(1, false);
  m.g.add_op(OP_MUL, {b, xs}, mu);
  m.g.add_op(OP_CAUCHY_LPDF, {ys, mu, sigma}, lp);
  m.terms = {lp};
  const Collapsed c = collapse(m);
  expect("real variate: four rows",
         c.stats.vector_terms == 1 && c.stats.rows == 4);
  expect("real variate: no statistic for this density",
         c.report.terms.size() == 1 && c.report.terms[0].groups == -1);
  expect("real variate: same values", c.same_values);
}

// X * beta with four distinct rows of X.
static void test_matvec_rows() {
  Model m;
  const int beta = m.g.add_slot(2, true);
  std::vector<double> x((size_t)(2 * kN));
  std::vector<int> y;
  for (int i = 0; i < kN; ++i) {
    x[(size_t)i] = 1.0;                   // column 0
    x[(size_t)(kN + i)] = 0.5 * (i % 2);  // column 1
    y.push_back((i / 2) % 2);
  }
  const int xs = data_slot(m, x);
  const int eta = m.g.add_slot(kN, false), lp = m.g.add_slot(1, false);
  m.g.add_op(OP_MATVEC, {xs, beta}, eta, {kN, 2});
  m.g.add_op(OP_BERNOULLI_LOGIT_LPMF, {eta}, lp, y);
  m.terms = {lp};
  const Collapsed c = collapse(m);
  expect("matvec: four rows", c.stats.vector_terms == 1 && c.stats.rows == 4);
  const Op* mv = find_op(c.model.g, OP_MATVEC);
  expect("matvec: four rows of the design kept",
         mv && mv->n_idata == 2 && mv->idata[0] == 4 && mv->idata[1] == 2 &&
             c.model.g.slots[(size_t)mv->in[0]].len == 8);
  expect("matvec: same values", c.same_values);
}

// A store after the first density changes what the second one reads; the
// first must still see the values of its own moment.
static void test_a_later_store() {
  Model m;
  const int b = m.g.add_slot(1, true), c0 = m.g.add_slot(1, true);
  std::vector<double> x;
  std::vector<int> y;
  for (int i = 0; i < kWide; ++i) {
    x.push_back(0.5 * (i % 2));
    y.push_back((i / 2) % 2 ? 3 : 1);
  }
  const int xs = data_slot(m, x);
  const int mu = m.g.add_slot(kWide, false), mu2 = m.g.add_slot(kWide, false);
  const int lp1 = m.g.add_slot(1, false), lp2 = m.g.add_slot(1, false);
  m.g.add_op(OP_MUL, {b, xs}, mu);
  m.g.add_op(OP_POISSON_LOG_LPMF, {mu}, lp1, y);
  m.g.add_op(OP_SET_INDEX, {mu, c0}, mu2, {7});
  m.g.add_op(OP_POISSON_LOG_LPMF, {mu2}, lp2, y);
  m.terms = {lp1, lp2};
  const Collapsed c = collapse(m);
  expect("store: both terms collapse, four and five rows",
         c.stats.vector_terms == 2 && c.stats.rows == 9);
  expect("store: same values", c.same_values);
}

// Where an argument can only be gathered from its full-length vector, the
// rewrite is taken back whole unless the rows are few.
static void test_unrestricted_argument() {
  for (const int n : {16, kWide}) {
    Model m;
    const int b = m.g.add_slot(2, true), c0 = m.g.add_slot(1, true);
    const int eta = m.g.add_slot(n, false), eta2 = m.g.add_slot(n, false);
    const int theta = m.g.add_slot(n, false), lp = m.g.add_slot(1, false);
    std::vector<int> group, outcome;
    for (int i = 0; i < n; ++i) {
      group.push_back(i % 2);
      outcome.push_back(i % 4 < 2);
    }
    m.g.add_op(OP_GATHER, {b}, eta, group);
    // One element replaced: the vector is a mix of sources, so only a
    // gather can restrict it.
    m.g.add_op(OP_SET_INDEX, {eta, c0}, eta2, {7});
    m.g.add_op(OP_INV_LOGIT, {eta2}, theta);
    m.g.add_op(OP_BERNOULLI_LPMF, {theta}, lp, outcome);
    m.terms = {lp};
    const Collapsed c = collapse(m);
    if (n == 16) {
      // Five rows of sixteen.
      expect("unrestricted, few observations: taken back whole",
             c.stats.vector_terms == 0 &&
                 c.model.g.ops.size() == m.g.ops.size() &&
                 c.model.g.slots.size() == m.g.slots.size() &&
                 c.model.fills.size() == m.fills.size());
      expect("unrestricted, few observations: says why",
             c.report.terms.size() == 1 &&
                 refused(c.report.terms[0],
                         "its arguments could not be restricted") &&
                 c.report.terms[0].evaluator == nullptr);
    } else {
      // Five rows of sixty-four pay even so.
      expect("unrestricted, many observations: collapses",
             c.stats.vector_terms == 1 && c.stats.rows == 5 &&
                 count_opcode(c.model.g, OP_INV_LOGIT) == 1 &&
                 out_len(c.model.g, OP_INV_LOGIT) == 5);
    }
    expect("unrestricted: same values", c.same_values);
  }
}

// A vector updated element by element, each update reading the element
// back, as `mu[n] += ...` lowers: only the updates behind the kept rows
// remain.
static void test_element_updates_die_with_their_rows() {
  Model m;
  const int a = m.g.add_slot(2, true);
  const int sigma = m.g.add_slot(1, true);
  const int y = data_slot(m, distinct_y(kN));
  const int start = m.g.add_slot(kN, false), mu = m.g.add_slot(kN, false);
  std::vector<int> group;
  for (int i = 0; i < kN; ++i) group.push_back(i % 2);
  m.g.add_op(OP_GATHER, {a}, start, group);
  for (int i = 0; i < kN; ++i) {
    const int was = m.g.add_slot(1, false), now = m.g.add_slot(1, false);
    m.g.add_op(OP_INDEX, {i == 0 ? start : mu}, was, {i});
    m.g.add_op(OP_MUL, {was, was}, now);
    if (i == 0)
      m.g.add_op(OP_SET_INDEX, {start, now}, mu, {i});
    else
      m.g.add_op(OP_SET_INDEX_INPLACE, {mu, now}, mu, {i});
  }
  const int lp = m.g.add_slot(1, false);
  m.g.add_op(OP_NORMAL_LPDF, {y, mu, sigma}, lp);
  m.terms = {lp};
  const Collapsed c = collapse(m);
  expect("updates: two groups", c.stats.rows == 2);
  expect("updates: two updates left of twenty",
         count_opcode(c.model.g, OP_MUL) == 2 &&
             count_opcode(c.model.g, OP_INDEX) == 2);
  expect("updates: same values", c.same_values);
}

// Scalar target terms with equal values become one term and a count,
// whatever slots their equal data sit in.
static void test_scalar_terms() {
  Model m;
  const int a = m.g.add_slot(2, true), sigma = m.g.add_slot(1, true);
  for (int i = 0; i < 40; ++i) {
    const int y = data_slot(m, {i % 4 < 2 ? 1.5 : -0.5});
    const int mu = m.g.add_slot(1, false), lp = m.g.add_slot(1, false);
    m.g.add_op(OP_INDEX, {a}, mu, {i % 2});
    m.g.add_op(OP_CAUCHY_LPDF, {y, mu, sigma}, lp);
    m.terms.push_back(lp);
  }
  const int extra = m.g.add_slot(1, false);
  m.g.add_op(OP_SQUARE, {sigma}, extra);
  m.terms.push_back(extra);
  const Collapsed c = collapse(m);
  expect("scalars: thirty-six terms merged",
         c.stats.scalar_terms_merged == 36 && c.model.terms.size() == 5);
  expect("scalars: four densities and four weights left",
         count_opcode(c.model.g, OP_CAUCHY_LPDF) == 4 &&
             count_opcode(c.model.g, OP_MUL) == 4 &&
             count_opcode(c.model.g, OP_INDEX) == 4);
  expect("scalars: same values", c.same_values);
}

// Terms that read the very same slots are CSE's to merge, and are left.
static void test_scalar_terms_equal_by_slot() {
  Model m;
  const int a = m.g.add_slot(20, true), sigma = m.g.add_slot(1, true);
  const int y = data_slot(m, {0.75});
  for (int i = 0; i < 40; ++i) {
    const int mu = m.g.add_slot(1, false), lp = m.g.add_slot(1, false);
    m.g.add_op(OP_INDEX, {a}, mu, {i % 20});
    m.g.add_op(OP_CAUCHY_LPDF, {y, mu, sigma}, lp);
    m.terms.push_back(lp);
  }
  const Collapsed c = collapse(m);
  expect("equal by slot: untouched",
         c.stats.scalar_terms_merged == 0 &&
             c.model.g.ops.size() == m.g.ops.size() &&
             c.model.terms == m.terms);
}

// Values of every count merge, each into one term, and no slot is listed
// twice.
static void test_scalar_terms_mixed_counts() {
  Model m;
  const int a = m.g.add_slot(8, true), sigma = m.g.add_slot(1, true);
  for (int i = 0; i < 46; ++i) {
    // a[0] forty times, a[1..3] twice each; the observation is equal data
    // in a slot of its own.
    const int which = i < 40 ? 0 : 1 + (i - 40) / 2;
    const int y = data_slot(m, {0.75});
    const int mu = m.g.add_slot(1, false), lp = m.g.add_slot(1, false);
    m.g.add_op(OP_INDEX, {a}, mu, {which});
    m.g.add_op(OP_CAUCHY_LPDF, {y, mu, sigma}, lp);
    m.terms.push_back(lp);
  }
  const Collapsed c = collapse(m);
  std::vector<int> sorted = c.model.terms;
  std::sort(sorted.begin(), sorted.end());
  expect("mixed counts: forty-two merged, four terms left",
         c.stats.scalar_terms_merged == 42 && c.model.terms.size() == 4);
  expect("mixed counts: no slot listed twice",
         std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());
  expect("mixed counts: four weights, four densities",
         count_opcode(c.model.g, OP_MUL) == 4 &&
             count_opcode(c.model.g, OP_CAUCHY_LPDF) == 4);
  expect("mixed counts: same values", c.same_values);
}

// The repeat is in the whole term: a two-component mixture per observation.
static void test_mixture_terms() {
  Model m;
  const int b = m.g.add_slot(2, true), sigma = m.g.add_slot(1, true);
  int mu[2];
  for (int k = 0; k < 2; ++k) {
    mu[k] = m.g.add_slot(1, false);
    m.g.add_op(OP_INDEX, {b}, mu[k], {k});
  }
  for (int i = 0; i < 32; ++i) {
    const int y = data_slot(m, {i % 2 ? 1.5 : -0.5});
    const int l0 = m.g.add_slot(1, false), l1 = m.g.add_slot(1, false);
    const int t = m.g.add_slot(1, false);
    m.g.add_op(OP_NORMAL_LPDF, {y, mu[0], sigma}, l0);
    m.g.add_op(OP_NORMAL_LPDF, {y, mu[1], sigma}, l1);
    m.g.add_op(OP_LSE2, {l0, l1}, t);
    m.terms.push_back(t);
  }
  const Collapsed c = collapse(m);
  expect("mixture: thirty terms merged",
         c.stats.scalar_terms_merged == 30 && c.model.terms.size() == 2);
  expect("mixture: two mixtures of two densities left",
         count_opcode(c.model.g, OP_LSE2) == 2 &&
             count_opcode(c.model.g, OP_NORMAL_LPDF) == 4);
  expect("mixture: same values", c.same_values);
}

// ---- per-group statistics --------------------------------------------------

struct NormalByGroup {
  int n = kN;
  int groups = 3;
  uint16_t opcode = OP_NORMAL_LPDF;
  uint8_t variant = 0;
  bool sigma_is_data = false;
  bool sigma_per_group = false;
  double shift = 0;  // added to the data and, as data, to the location
};

// y[n] ~ normal(a[group[n]] + shift, sigma) with every y different.
static Model normal_by_group(const NormalByGroup& c) {
  Model m;
  const int a = m.g.add_slot(c.groups, true);
  std::vector<double> yv;
  std::vector<int> group;
  for (int i = 0; i < c.n; ++i) {
    const double small = 0.25 * i - 1.5 + 0.01 * (i % 7);
    yv.push_back(c.opcode == OP_LOGNORMAL_LPDF ? std::exp(0.2 * small)
                                               : c.shift + small);
    group.push_back(i % c.groups);
  }
  const int y = data_slot(m, yv);
  int mu = m.g.add_slot(c.n, false);
  m.g.add_op(OP_GATHER, {a}, mu, group);
  if (c.shift != 0) {
    const int by = data_slot(m, {c.shift});
    const int shifted = m.g.add_slot(c.n, false);
    m.g.add_op(OP_ADD, {mu, by}, shifted);
    mu = shifted;
  }
  int sigma;
  if (c.sigma_is_data) {
    sigma = data_slot(m, {0.8});
  } else if (c.sigma_per_group) {
    const int s = m.g.add_slot(c.groups, true);
    sigma = m.g.add_slot(c.n, false);
    m.g.add_op(OP_GATHER, {s}, sigma, group);
  } else {
    sigma = m.g.add_slot(1, true);
  }
  const int lp = m.g.add_slot(1, false);
  m.g.add_op(c.opcode, {y, mu, sigma}, lp);
  m.g.ops.back().variant = c.variant;
  m.terms = {lp};
  return m;
}

static void expect_grouped(const char* what, const NormalByGroup& c) {
  const Collapsed r = collapse(normal_by_group(c));
  const Graph& g = r.model.g;
  const bool grouped = r.stats.statistic_terms == 1 &&
                       r.stats.vector_terms == 0 && r.stats.rows == c.groups &&
                       count_opcode(g, OP_NORMAL_GROUPED_LPDF) == 1 &&
                       count_opcode(g, c.opcode) == 0 &&
                       out_len(g, OP_GATHER) == c.groups;
  if (!grouped || !r.same_values) {
    ++failures;
    std::printf("FAIL %s: %s\n", what,
                grouped ? "values differ" : "not rewritten as expected");
  }
}

static void test_grouped_normal() {
  expect_grouped("grouped normal, full density", {});
  {
    // As the lowering marks `y ~ normal(mu, sigma)`: propto, data variate.
    NormalByGroup c;
    c.variant = 0x86;
    expect_grouped("grouped normal, propto", c);
  }
  {
    // A data scale under propto: Stan drops the log-scale term.
    NormalByGroup c;
    c.variant = 0x82;
    c.sigma_is_data = true;
    expect_grouped("grouped normal, propto with a data scale", c);
  }
  {
    NormalByGroup c;
    c.sigma_is_data = true;
    expect_grouped("grouped normal, full density with a data scale", c);
  }
  {
    NormalByGroup c;
    c.sigma_per_group = true;
    c.variant = 0x86;
    expect_grouped("grouped normal, a scale per group", c);
  }
  {
    // One group is one location: either closed form serves.
    NormalByGroup c;
    c.groups = 1;
    const Collapsed r = collapse(normal_by_group(c));
    expect("one group: collapsed to one element",
           r.stats.statistic_terms + r.stats.linear_terms == 1 &&
               r.stats.rows == 1 && count_opcode(r.model.g, c.opcode) == 0);
    expect("one group: same values", r.same_values);
  }
  {
    NormalByGroup c;
    c.opcode = OP_LOGNORMAL_LPDF;
    expect_grouped("grouped lognormal, full density", c);
    c.variant = 0x86;
    expect_grouped("grouped lognormal, propto", c);
  }
  {
    const Collapsed r = collapse(normal_by_group({}));
    expect("grouped normal: reported as groups",
           r.report.terms.size() == 1 && r.report.terms[0].groups == 3 &&
               r.report.terms[0].refusal == nullptr &&
               std::strcmp(r.report.terms[0].evaluator, "groups") == 0);
  }
}

// Data far from zero: the statistics keep the accuracy the observations
// had. The reference sums the observations in extended precision at the
// location the graph itself computes.
static void test_grouped_normal_shifted_data() {
#if LDBL_MANT_DIG >= 64
  for (const double shift : {0.0, 1e3, 1e6, 1e9}) {
    NormalByGroup c;
    c.n = 60;
    c.shift = shift;
    Model m = normal_by_group(c);
    const std::vector<double> yv = m.fills[0].second;
    collapse_observations(m.g, m.fills, m.terms, m.roots);
    expect("shifted: rewritten",
           count_opcode(m.g, OP_NORMAL_GROUPED_LPDF) == 1);
    const std::vector<double> got = evaluate(m);
    // Parameters in slot order: a[0..2], then sigma.
    const double sigma = fill_at(3);
    long double lp = 0, d_sigma = 0, d_a[3] = {0, 0, 0};
    for (int i = 0; i < c.n; ++i) {
      const double mu = fill_at(i % 3) + shift;  // as OP_ADD rounds it
      const long double z = ((long double)yv[(size_t)i] - mu) / sigma;
      lp += -std::log((long double)sigma) - 0.5L * z * z -
            0.918938533204672741780329736406L;
      d_a[i % 3] += z / sigma;
      d_sigma += (z * z - 1) / sigma;
    }
    const std::vector<double> want{(double)lp, (double)d_a[0], (double)d_a[1],
                                   (double)d_a[2], (double)d_sigma};
    double scale = 1, diff = 0;
    for (size_t k = 1; k < want.size(); ++k) {
      scale = std::max(scale, std::abs(want[k]));
      diff = std::max(diff, std::abs(got[k] - want[k]));
    }
    const bool ok = got.size() == want.size() && diff <= 2e-14 * scale &&
                    std::abs(got[0] - want[0]) <= 2e-14 * std::abs(want[0]);
    if (!ok) {
      ++failures;
      std::printf(
          "FAIL shifted by %g: gradient off by %.3g of its largest "
          "entry, lp by %.3g\n",
          shift, diff / scale, std::abs(got[0] - want[0]) / std::abs(want[0]));
    }
  }
#endif
}

// A point Stan rejects is rejected, with Stan's message.
static std::string rejection(Model m, double sigma) {
  testutil::reduce_into_result(m.g, m.terms);
  try {
    testutil::run_grad(std::move(m.g), m.fills,
                       [&](int64_t i) { return i == 3 ? sigma : 0.5; });
  } catch (const std::exception& e) {
    return e.what();
  }
  return "";
}

static void test_grouped_normal_rejects() {
  const Model m = normal_by_group({});
  Model collapsed = m;
  collapse_observations(collapsed.g, collapsed.fills, collapsed.terms,
                        collapsed.roots);
  expect("reject: rewritten",
         count_opcode(collapsed.g, OP_NORMAL_GROUPED_LPDF) == 1);
  const std::string before = rejection(m, -0.5);
  expect("reject: the original rejects a negative scale", !before.empty());
  expect("reject: same message", rejection(collapsed, -0.5) == before);
  expect("reject: a valid point is accepted",
         rejection(collapsed, 0.5).empty());
}

// Values the statistics cannot stand in for leave the term to the row form,
// or alone.
static void test_grouped_normal_refusals() {
  {
    Model m = normal_by_group({});
    m.fills[0].second[4] = std::numeric_limits<double>::infinity();
    const Collapsed r = collapse(m);
    expect("infinite observation: not grouped", r.stats.statistic_terms == 0);
  }
  {
    NormalByGroup c;
    c.opcode = OP_LOGNORMAL_LPDF;
    Model m = normal_by_group(c);
    m.fills[0].second[4] = 0.0;
    const Collapsed r = collapse(m);
    expect("zero lognormal observation: left alone",
           r.stats.statistic_terms == 0 && r.stats.linear_terms == 0 &&
               r.model.g.ops.size() == m.g.ops.size());
  }
  {
    // Ten groups of twenty observations is not worth it.
    NormalByGroup c;
    c.groups = 11;
    const Collapsed r = collapse(normal_by_group(c));
    expect("too many groups: not grouped",
           r.stats.statistic_terms == 0 &&
               refused(r.report.terms[0], "too few repeated rows"));
  }
}

// One scalar normal term per observation, as a loop no earlier pass fused
// leaves them: the same statistics as the vector term.
static void test_grouped_normal_from_scalar_terms() {
  for (const bool per_group_scale : {false, true}) {
    Model m;
    const int a = m.g.add_slot(3, true);
    const int s = m.g.add_slot(per_group_scale ? 3 : 1, true);
    for (int i = 0; i < 40; ++i) {
      const int y = data_slot(m, {0.25 * i - 4.0 + 0.01 * (i % 7)});
      const int mu = m.g.add_slot(1, false), lp = m.g.add_slot(1, false);
      m.g.add_op(OP_INDEX, {a}, mu, {i % 3});
      int sigma = s;
      if (per_group_scale) {
        sigma = m.g.add_slot(1, false);
        m.g.add_op(OP_INDEX, {s}, sigma, {i % 3});
      }
      m.g.add_op(OP_NORMAL_LPDF, {y, mu, sigma}, lp);
      m.g.ops.back().variant = 0x86;
      m.terms.push_back(lp);
    }
    // One more term, so the scale has a use of its own.
    const int squared = m.g.add_slot(per_group_scale ? 3 : 1, false);
    const int extra = m.g.add_slot(1, false);
    m.g.add_op(OP_SQUARE, {s}, squared);
    m.g.add_op(OP_SUM_VEC, {squared}, extra);
    m.terms.push_back(extra);
    const Collapsed c = collapse(m);
    expect("scalar family: one grouped term over three groups",
           c.stats.statistic_terms == 1 && c.stats.rows == 3 &&
               c.stats.observations == 40 &&
               count_opcode(c.model.g, OP_NORMAL_GROUPED_LPDF) == 1 &&
               count_opcode(c.model.g, OP_NORMAL_LPDF) == 0 &&
               c.model.terms.size() == 2);
    // The locations are a[0..2] in order: the parameter vector itself.
    expect("scalar family: no location chains left",
           count_opcode(c.model.g, OP_INDEX) == (per_group_scale ? 3 : 0));
    expect("scalar family: same values", c.same_values);
  }
}

// A family whose locations are a[group] + b * x, as a hierarchical
// regression unrolls: the groups' locations are rebuilt as a few vector ops
// over the parameters, not one scalar chain per group.
static void test_scalar_family_locations_are_vectorized() {
  Model m;
  const int a = m.g.add_slot(5, true), b = m.g.add_slot(1, true);
  const int sigma = m.g.add_slot(1, true);
  for (int i = 0; i < 60; ++i) {
    const int xs = data_slot(m, {i % 2 ? 1.0 : 0.25});
    const int y = data_slot(m, {0.1 * i - 3.0 + 0.01 * (i % 7)});
    const int ai = m.g.add_slot(1, false), mu = m.g.add_slot(1, false);
    const int lp = m.g.add_slot(1, false);
    m.g.add_op(OP_INDEX, {a}, ai, {i % 5});
    m.g.add_op(OP_FMA, {xs, b, ai}, mu);
    m.g.add_op(OP_NORMAL_LPDF, {y, mu, sigma}, lp);
    m.g.ops.back().variant = 0x86;
    m.terms.push_back(lp);
  }
  const Collapsed c = collapse(m);
  const Graph& g = c.model.g;
  // Ten groups (five of a, two of x) in six values: the group form.
  expect("vectorized family: one grouped term over ten groups",
         c.stats.statistic_terms == 1 && c.stats.rows == 10 &&
             count_opcode(g, OP_NORMAL_GROUPED_LPDF) == 1 &&
             count_opcode(g, OP_NORMAL_LPDF) == 0);
  expect("vectorized family: no scalar chains or packing left",
         count_opcode(g, OP_FMA) == 0 && count_opcode(g, OP_INDEX) == 0 &&
             count_opcode(g, OP_SET_INDEX) == 0 &&
             count_opcode(g, OP_SET_INDEX_INPLACE) == 0);
  expect("vectorized family: one gather, one multiply, one add",
         count_opcode(g, OP_GATHER) == 1 && out_len(g, OP_GATHER) == 10 &&
             count_opcode(g, OP_MUL) == 1 && count_opcode(g, OP_ADD) == 1);
  expect("vectorized family: same values", c.same_values);
}

// The same family with locations b * x[n] + a: one quadratic form.
static void test_linear_gaussian_from_scalar_terms() {
  Model m;
  const int a = m.g.add_slot(1, true), b = m.g.add_slot(1, true);
  const int sigma = m.g.add_slot(1, true);
  for (int i = 0; i < 40; ++i) {
    const double x = std::sin(0.7 * (i + 1));
    const int xs = data_slot(m, {x});
    const int y = data_slot(m, {0.4 + 1.3 * x + 0.05 * std::cos(2.1 * i)});
    const int mu = m.g.add_slot(1, false), lp = m.g.add_slot(1, false);
    m.g.add_op(OP_FMA, {b, xs, a}, mu);
    m.g.add_op(OP_NORMAL_LPDF, {y, mu, sigma}, lp);
    m.g.ops.back().variant = 0x86;
    m.terms.push_back(lp);
  }
  const Collapsed c = collapse(m);
  const Op* op = find_op(c.model.g, OP_LINEAR_GAUSSIAN_LPDF);
  expect("scalar family: one quadratic form in two values",
         c.stats.linear_terms == 1 && op != nullptr &&
             c.model.g.slots[(size_t)op->in[1]].len == 2 &&
             count_opcode(c.model.g, OP_NORMAL_LPDF) == 0 &&
             count_opcode(c.model.g, OP_FMA) == 0 && c.model.terms.size() == 1);
  expect("scalar family, linear: same values", c.same_values);
}

// ---- locations affine in a few values --------------------------------------

struct Regression {
  int n = 60;
  uint16_t opcode = OP_NORMAL_LPDF;
  uint8_t variant = 0;
  bool repeat_a_column = false;  // a rank-deficient design
  bool by_fma = false;           // alpha + beta0 * x0 + ..., one op per term
  double shift = 0;              // added to the data as an intercept offset
};

constexpr int kColumns = 3;
static const double kTruth[kColumns + 1] = {0.7, -1.1, 0.4, 0.25};  // +alpha

static double design(const Regression& c, int i, int col) {
  if (c.repeat_a_column && col == 2) col = 0;
  return std::sin(0.37 * (i + 1) * (col + 1)) + 0.1 * col;
}

// y ~ normal(alpha + X * beta, sigma) with no two rows of X alike.
// Parameters in slot order: beta[0..2], alpha, sigma.
static Model regression(const Regression& c) {
  Model m;
  const int beta = m.g.add_slot(kColumns, true);
  const int alpha = m.g.add_slot(1, true), sigma = m.g.add_slot(1, true);
  std::vector<double> x((size_t)(kColumns * c.n)), yv;
  for (int i = 0; i < c.n; ++i) {
    double mean = kTruth[kColumns] + c.shift;
    for (int col = 0; col < kColumns; ++col) {
      x[(size_t)(col * c.n + i)] = design(c, i, col);
      mean += kTruth[col] * design(c, i, col);
    }
    const double noise = 0.05 * std::cos(1.3 * i);
    yv.push_back(c.opcode == OP_LOGNORMAL_LPDF ? std::exp(mean + noise)
                                               : mean + noise);
  }
  const int y = data_slot(m, yv);
  int mu;
  if (c.by_fma) {
    mu = alpha;
    for (int col = 0; col < kColumns; ++col) {
      const int xc =
          data_slot(m, std::vector<double>(x.begin() + col * c.n,
                                           x.begin() + (col + 1) * c.n));
      const int b = m.g.add_slot(1, false), next = m.g.add_slot(c.n, false);
      m.g.add_op(OP_INDEX, {beta}, b, {col});
      m.g.add_op(OP_FMA, {b, xc, mu}, next);
      mu = next;
    }
  } else {
    const int xs = data_slot(m, x);
    const int xb = m.g.add_slot(c.n, false);
    mu = m.g.add_slot(c.n, false);
    m.g.add_op(OP_MATVEC, {xs, beta}, xb, {c.n, kColumns});
    m.g.add_op(OP_ADD, {xb, alpha}, mu);
  }
  if (c.shift != 0) {
    const int by = data_slot(m, {c.shift});
    const int shifted = m.g.add_slot(c.n, false);
    m.g.add_op(OP_ADD, {mu, by}, shifted);
    mu = shifted;
  }
  const int lp = m.g.add_slot(1, false);
  m.g.add_op(c.opcode, {y, mu, sigma}, lp);
  m.g.ops.back().variant = c.variant;
  m.terms = {lp};
  return m;
}

static void expect_linear(const char* what, const Regression& c) {
  const Collapsed r = collapse(regression(c));
  const Graph& g = r.model.g;
  const Op* op = find_op(g, OP_LINEAR_GAUSSIAN_LPDF);
  const bool linear =
      r.stats.linear_terms == 1 && op != nullptr &&
      count_opcode(g, c.opcode) == 0 && count_opcode(g, OP_MATVEC) == 0 &&
      count_opcode(g, OP_FMA) == 0 &&
      g.slots[(size_t)op->in[1]].len == kColumns + 1 &&
      r.report.terms.size() == 1 && r.report.terms[0].evaluator != nullptr &&
      std::strcmp(r.report.terms[0].evaluator, "linear") == 0;
  if (!linear || !r.same_values) {
    ++failures;
    std::printf("FAIL %s: %s\n", what,
                linear ? "values differ" : "not rewritten as expected");
  }
}

static void test_linear_gaussian() {
  expect_linear("regression, full density", {});
  {
    Regression c;
    c.variant = 0x86;
    expect_linear("regression, propto", c);
  }
  {
    Regression c;
    c.by_fma = true;
    c.variant = 0x86;
    expect_linear("regression built term by term", c);
  }
  {
    // Two equal columns: the form is exact for any centre, so rank does
    // not matter.
    Regression c;
    c.repeat_a_column = true;
    expect_linear("rank-deficient regression", c);
  }
  {
    Regression c;
    c.opcode = OP_LOGNORMAL_LPDF;
    expect_linear("lognormal regression, full density", c);
    c.variant = 0x86;
    expect_linear("lognormal regression, propto", c);
  }
}

// Near the mode, where the residuals are small beside the data, against the
// observations summed in extended precision.
static void test_linear_gaussian_near_the_mode() {
#if LDBL_MANT_DIG >= 64
  for (const double shift : {0.0, 1e3, 1e6}) {
    for (const bool rank_deficient : {false, true}) {
      Regression c;
      c.shift = shift;
      c.repeat_a_column = rank_deficient;
      Model m = regression(c);
      const std::vector<double> yv = m.fills[0].second;
      collapse_observations(m.g, m.fills, m.terms, m.roots);
      expect("near mode: rewritten",
             count_opcode(m.g, OP_LINEAR_GAUSSIAN_LPDF) == 1);
      const double sigma = 0.05;
      const auto at = [&](int64_t i) {
        return i == kColumns + 1 ? sigma : kTruth[i] * 1.001;
      };
      const std::vector<double> got = evaluate_at(m, at);
      long double lp = 0, d_sigma = 0, d[kColumns + 1] = {0, 0, 0, 0};
      for (int i = 0; i < c.n; ++i) {
        // The shift comes off the observation first, exactly, so the
        // reference keeps its precision for the residual.
        long double mu = at(kColumns);
        for (int col = 0; col < kColumns; ++col)
          mu += (long double)at(col) * design(c, i, col);
        const long double z =
            ((long double)yv[(size_t)i] - (long double)shift - mu) / sigma;
        lp += -std::log((long double)sigma) - 0.5L * z * z -
              0.918938533204672741780329736406L;
        for (int col = 0; col < kColumns; ++col)
          d[col] += z / sigma * design(c, i, col);
        d[kColumns] += z / sigma;
        d_sigma += (z * z - 1) / sigma;
      }
      const std::vector<double> want{(double)lp,   (double)d[0],
                                     (double)d[1], (double)d[2],
                                     (double)d[3], (double)d_sigma};
      double scale = 1, diff = 0;
      for (size_t k = 1; k < want.size(); ++k) {
        scale = std::max(scale, std::abs(want[k]));
        diff = std::max(diff, std::abs(got[k] - want[k]));
      }
      // The reference forms the shifted location exactly; the form's own
      // error is a few units in the last place of the largest entry.
      const bool ok = got.size() == want.size() && diff <= 1e-13 * scale &&
                      std::abs(got[0] - want[0]) <=
                          1e-13 * std::max(1.0, std::abs(want[0]));
      if (!ok) {
        ++failures;
        std::printf(
            "FAIL near mode, shift %g, rank deficient %d: gradient "
            "off by %.3g of its largest entry, lp by %.3g\n",
            shift, (int)rank_deficient, diff / scale,
            std::abs(got[0] - want[0]) / std::max(1.0, std::abs(want[0])));
      }
    }
  }
#endif
}

// normal_id_glm(y | X, alpha, beta, sigma) is the same term with its
// locations already written as a design times a vector.
static Model glm_regression(uint8_t variant, int n = 60) {
  Regression c;
  c.n = n;
  Model m;
  const int beta = m.g.add_slot(kColumns, true);
  const int alpha = m.g.add_slot(1, true), sigma = m.g.add_slot(1, true);
  std::vector<double> x((size_t)(kColumns * n)), yv;
  for (int i = 0; i < n; ++i) {
    double mean = kTruth[kColumns];
    for (int col = 0; col < kColumns; ++col) {
      x[(size_t)(col * n + i)] = design(c, i, col);
      mean += kTruth[col] * design(c, i, col);
    }
    yv.push_back(mean + 0.05 * std::cos(1.3 * i));
  }
  const int y = data_slot(m, yv), xs = data_slot(m, x);
  const int lp = m.g.add_slot(1, false);
  m.g.add_op(OP_NORMAL_ID_GLM_LPDF, {y, xs, alpha, beta, sigma}, lp,
             {n, kColumns});
  m.g.ops.back().variant = variant;
  m.terms = {lp};
  return m;
}

static void test_linear_gaussian_from_a_glm() {
  // Activity bits: alpha, beta, sigma; then the same under propto.
  for (const uint8_t variant : {uint8_t{0x1c}, uint8_t{0x9c}}) {
    const Collapsed r = collapse(glm_regression(variant));
    const Graph& g = r.model.g;
    const Op* op = find_op(g, OP_LINEAR_GAUSSIAN_LPDF);
    expect("glm: one quadratic form over four values",
           r.stats.linear_terms == 1 && op != nullptr &&
               count_opcode(g, OP_NORMAL_ID_GLM_LPDF) == 0 &&
               g.slots[(size_t)op->in[1]].len == kColumns + 1);
    expect("glm: same values", r.same_values);
  }
  {
    const Model m = glm_regression(0x1c);
    Model collapsed = m;
    collapse_observations(collapsed.g, collapsed.fills, collapsed.terms,
                          collapsed.roots);
    // Parameter 4 is the scale here.
    const auto message = [](Model model) {
      testutil::reduce_into_result(model.g, model.terms);
      try {
        testutil::run_grad(std::move(model.g), model.fills,
                           [](int64_t i) { return i == 4 ? -0.5 : 0.5; });
      } catch (const std::exception& e) {
        return std::string(e.what());
      }
      return std::string();
    };
    const std::string before = message(m);
    expect("glm: the original rejects a negative scale", !before.empty());
    expect("glm: same message", message(collapsed) == before);
  }
  {
    // Too few observations for the design to be worth factoring.
    const Collapsed r = collapse(glm_regression(0x1c, 12));
    expect("short glm: untouched", r.stats.linear_terms == 0);
  }
}

// A location that is not affine in few values stays with the group form.
static void test_linear_gaussian_left_to_groups() {
  const Collapsed r = collapse(normal_by_group({}));
  expect("indicator design: grouped, not linear",
         r.stats.linear_terms == 0 && r.stats.statistic_terms == 1);
}

static void test_left_alone() {
  {
    const Model m = bernoulli_by_group(kCollapseMinObservations - 1);
    const Collapsed c = collapse(m);
    expect("short term: untouched",
           c.stats.vector_terms == 0 && c.model.g.ops.size() == m.g.ops.size());
    expect("short term: says why",
           c.report.terms.size() == 1 &&
               refused(c.report.terms[0], "too few observations"));
  }
  {
    // Four rows of sixty-four would do; four of twenty does not pay for a
    // recorder call per row.
    Model m;
    const int b = m.g.add_slot(1, true);
    std::vector<double> x;
    std::vector<int> y;
    for (int i = 0; i < kN; ++i) {
      x.push_back(0.5 * (i % 2));
      y.push_back((i / 2) % 2 ? 3 : 1);
    }
    const int xs = data_slot(m, x);
    const int mu = m.g.add_slot(kN, false), lp = m.g.add_slot(1, false);
    m.g.add_op(OP_MUL, {b, xs}, mu);
    m.g.add_op(OP_POISSON_LOG_LPMF, {mu}, lp, y);
    m.terms = {lp};
    const Collapsed c = collapse(m);
    expect("recorder density: untouched", c.stats.vector_terms == 0);
    expect("recorder density: says why",
           c.report.terms.size() == 1 && c.report.terms[0].rows == 4 &&
               refused(c.report.terms[0], "too few repeated rows"));
  }
  {
    test_setenv("STANLI_NO_COLLAPSE", "1");
    const Model m = bernoulli_by_group(kN);
    const Collapsed c = collapse(m);
    test_unsetenv("STANLI_NO_COLLAPSE");
    expect("disabled: untouched",
           c.stats.vector_terms == 0 && c.model.g.ops.size() == m.g.ops.size());
  }
}

// ---- what the analysis reports ---------------------------------------------

static CollapseReport analyze(const Model& m) {
  return analyze_collapse(m.g, m.fills, m.terms);
}

// y[n] ~ normal(a[group[n]], sigma): every row differs by its variate, so
// weighting has nothing to merge; the groups are for the statistic form.
static void test_reports_rows_and_groups() {
  Model m;
  const int a = m.g.add_slot(3, true), sigma = m.g.add_slot(1, true);
  const int y = data_slot(m, distinct_y(kN));
  const int mu = m.g.add_slot(kN, false), lp = m.g.add_slot(1, false);
  std::vector<int> group;
  for (int i = 0; i < kN; ++i) group.push_back(i % 3);
  m.g.add_op(OP_GATHER, {a}, mu, group);
  const int dens = m.g.add_op(OP_NORMAL_LPDF, {y, mu, sigma}, lp);
  m.terms = {lp};
  const CollapseReport r = analyze(m);
  expect("report: one term", r.terms.size() == 1);
  if (r.terms.size() != 1) return;
  const CollapseTerm& t = r.terms[0];
  expect("report: names the density op", t.op == dens && t.ops == 1);
  expect("report: twenty observations, twenty rows", t.n == kN && t.rows == kN);
  expect("report: three groups", t.groups == 3 && t.variate_is_data);
  expect("report: collapses by groups",
         t.refusal == nullptr && t.evaluator != nullptr &&
             std::strcmp(t.evaluator, "groups") == 0);
}

// Data are compared bitwise: -0.0 and 0.0 are different rows.
static void test_data_equality_is_bitwise() {
  Model m;
  const int b = m.g.add_slot(1, true), sigma = m.g.add_slot(1, true);
  std::vector<double> x(kN, 0.0);
  for (int i = 0; i < kN; i += 2) x[(size_t)i] = -0.0;
  const int xs = data_slot(m, x);
  const int y = data_slot(m, distinct_y(kN));
  const int mu = m.g.add_slot(kN, false), lp = m.g.add_slot(1, false);
  m.g.add_op(OP_MUL, {b, xs}, mu);
  m.g.add_op(OP_NORMAL_LPDF, {y, mu, sigma}, lp);
  m.terms = {lp};
  const CollapseReport r = analyze(m);
  expect("signed zero: two groups",
         r.terms.size() == 1 && r.terms[0].groups == 2);
}

// A predictor copied into a declared vector and read back in pieces.
static void test_slices_carry_values() {
  Model m;
  const int a = m.g.add_slot(2, true), sigma = m.g.add_slot(1, true);
  const int y = data_slot(m, distinct_y(kN));
  const int picked = m.g.add_slot(kN, false);
  const int decl = data_slot(
      m, std::vector<double>(2 * kN, std::numeric_limits<double>::quiet_NaN()));
  const int stored = m.g.add_slot(2 * kN, false);
  const int evens = m.g.add_slot(kN, false), tail = m.g.add_slot(kN, false);
  const int lp1 = m.g.add_slot(1, false), lp2 = m.g.add_slot(1, false);
  std::vector<int> group;
  for (int i = 0; i < kN; ++i) group.push_back(i % 2);
  m.g.add_op(OP_GATHER, {a}, picked, group);
  // Even positions, then the second half: both hold gathered values.
  m.g.add_op(OP_SET_SLICE_STRIDED, {decl, picked}, stored, {0, 2});
  m.g.add_op(OP_SET_SLICE_INPLACE, {stored, picked}, stored, {kN});
  m.g.add_op(OP_SLICE_STRIDED, {stored}, evens, {0, 2});
  m.g.add_op(OP_SLICE, {stored}, tail, {kN});
  m.g.add_op(OP_NORMAL_LPDF, {y, evens, sigma}, lp1);
  m.g.add_op(OP_NORMAL_LPDF, {y, tail, sigma}, lp2);
  m.terms = {lp1, lp2};
  const CollapseReport r = analyze(m);
  expect("slices: two terms", r.terms.size() == 2);
  if (r.terms.size() != 2) return;
  expect("slices: strided read sees two groups", r.terms[0].groups == 2);
  expect("slices: contiguous read sees two groups", r.terms[1].groups == 2);
}

// An op the analysis does not model keeps every row apart.
static void test_unknown_producer_keeps_rows_apart() {
  Model m;
  const int a = m.g.add_slot(kN, true), sigma = m.g.add_slot(1, true);
  const int y = data_slot(m, distinct_y(kN));
  const int mu = m.g.add_slot(kN, false), lp = m.g.add_slot(1, false);
  m.g.add_op(OP_SOFTMAX, {a}, mu);
  m.g.add_op(OP_NORMAL_LPDF, {y, mu, sigma}, lp);
  m.terms = {lp};
  const CollapseReport r = analyze(m);
  expect("unknown: twenty groups",
         r.terms.size() == 1 && r.terms[0].groups == kN);
}

// A density of data alone is a constant, not a likelihood term.
static void test_data_only_density_is_not_a_term() {
  Model m;
  const int p = m.g.add_slot(1, true);
  const int y = data_slot(m, distinct_y(kN));
  const int mu = data_slot(m, {0.0}), sigma = data_slot(m, {1.0});
  const int c = m.g.add_slot(1, false), lp = m.g.add_slot(1, false);
  m.g.add_op(OP_NORMAL_LPDF, {y, mu, sigma}, c);
  m.g.add_op(OP_SQUARE, {p}, lp);
  m.terms = {c, lp};
  expect("data-only density: no terms", analyze(m).terms.empty());
}

int main() {
  test_gathered_predictor();
  test_roots_stay();
  test_shared_predictor();
  test_one_row();
  test_scalar_chains();
  test_scalar_chains_become_vector_ops();
  test_copied_predictor();
  test_real_variate_rows();
  test_matvec_rows();
  test_a_later_store();
  test_unrestricted_argument();
  test_element_updates_die_with_their_rows();
  test_scalar_terms();
  test_scalar_terms_equal_by_slot();
  test_scalar_terms_mixed_counts();
  test_mixture_terms();
  test_grouped_normal();
  test_grouped_normal_shifted_data();
  test_grouped_normal_rejects();
  test_grouped_normal_refusals();
  test_grouped_normal_from_scalar_terms();
  test_scalar_family_locations_are_vectorized();
  test_linear_gaussian_from_scalar_terms();
  test_linear_gaussian();
  test_linear_gaussian_near_the_mode();
  test_linear_gaussian_from_a_glm();
  test_linear_gaussian_left_to_groups();
  test_left_alone();
  test_reports_rows_and_groups();
  test_data_equality_is_bitwise();
  test_slices_carry_values();
  test_unknown_producer_keeps_rows_apart();
  test_data_only_density_is_not_a_term();
  if (failures == 0) std::printf("test_collapse OK\n");
  return failures == 0 ? 0 : 1;
}
