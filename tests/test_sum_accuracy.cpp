// Kernels that reduce thousands of contributions into one value, held to the
// exact sum of those contributions. A pass that merges a loop of scalar calls
// into one vector call hands these kernels sums of 2,000 to 33,000 terms; a
// plain running sum then loses up to a few hundred ULP. The reference here
// adds the same doubles in double-double arithmetic, which is exact to far
// below one ULP of the result.
#include "density_ops.hpp"
#include "expect.hpp"

#include <stanli/extrema_grouping.hpp>
#include <stanli/graph.hpp>
#include <stanli/kernel_types.hpp>
#include <stanli/optable.hpp>
#include <stan/math.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

using namespace stanli;

namespace {

constexpr int64_t kN = 5000;
// Two ULP leaves room for the rounding of the result and of each product.
constexpr uint64_t kUlp = 2;

std::vector<double> terms(int64_t n, double base, int64_t period) {
  std::vector<double> x((size_t)n);
  for (int64_t i = 0; i < n; ++i) x[(size_t)i] = base + 1.0 / (3 + i % period);
  return x;
}

// lp = sum(w .* t), so each element of t receives adjoint w[i].
int weighted_sum(Graph& g, int t, int w) {
  const int64_t n = g.slots[(size_t)t].len;
  const int prod = g.add_slot(n, false), lp = g.add_slot(1, false);
  g.add_op(OP_MUL, {t, w}, prod);
  g.add_op(OP_SUM_VEC, {prod}, lp);
  g.result_slot = lp;
  return lp;
}

// A broadcast scalar parameter s in an elementwise op over kN elements: its
// adjoint is the sum of kN contributions.
void test_broadcast_scalar_adjoints() {
  const std::vector<double> x = terms(kN, 0.1, 97);
  const std::vector<double> w = terms(kN, 0.7, 89);
  ExactSum sum_w, sum_wx;
  for (int64_t i = 0; i < kN; ++i) {
    sum_w.add(w[(size_t)i]);
    sum_wx.add_product(w[(size_t)i], x[(size_t)i]);
  }
  struct Case {
    const char* name;
    uint16_t opcode;
    int scalar_at;  // input position of s; the others are x (then x again)
    int n_in;
    double want;
  };
  const Case cases[] = {
      {"mul s*x", OP_MUL, 0, 2, sum_wx.value()},
      {"mul x*s", OP_MUL, 1, 2, sum_wx.value()},
      {"add s+x", OP_ADD, 0, 2, sum_w.value()},
      {"add x+s", OP_ADD, 1, 2, sum_w.value()},
      {"sub s-x", OP_SUB, 0, 2, sum_w.value()},
      {"sub x-s", OP_SUB, 1, 2, -sum_w.value()},
      {"fma s*x+x", OP_FMA, 0, 3, sum_wx.value()},
      {"fma x*s+x", OP_FMA, 1, 3, sum_wx.value()},
      {"fma x*x+s", OP_FMA, 2, 3, sum_w.value()},
  };
  for (const Case& c : cases) {
    Graph g;
    const int s = g.add_slot(1, true);
    const int xs = g.add_slot(kN, false), ws = g.add_slot(kN, false);
    const int t = g.add_slot(kN, false);
    Op op;
    op.opcode = c.opcode;
    op.out = t;
    op.n_in = c.n_in;
    for (int k = 0; k < c.n_in; ++k) op.in[k] = k == c.scalar_at ? s : xs;
    g.ops.push_back(op);
    weighted_sum(g, t, ws);
    Executor ex(std::move(g));
    ex.param_ptr(s)[0] = 0.3;
    for (int64_t i = 0; i < kN; ++i) {
      ex.value_ptr(xs)[i] = x[(size_t)i];
      ex.value_ptr(ws)[i] = w[(size_t)i];
    }
    double grad = 0.0;
    ex.gradient(&grad);
    expect_ulp(std::string(c.name) + " scalar adjoint", grad, c.want, kUlp);
  }
}

// An elementwise density (out[n] is observation n's lp) with a broadcast
// location: d/dmu of normal(y | mu = 0, sigma = 1) is y[n], so the adjoint
// of mu is the sum of y.
void test_elementwise_density_scalar_adjoint() {
  const std::vector<double> y = terms(kN, 0.2, 101);
  ExactSum want;
  for (double v : y) want.add(v);
  Graph g;
  const int ys = g.add_slot(kN, false), mu = g.add_slot(1, true);
  const int sigma = g.add_slot(1, false), out = g.add_slot(kN, false);
  const int lp = g.add_slot(1, false);
  g.add_op(OP_NORMAL_LPDF, {ys, mu, sigma}, out);
  g.ops.back().variant =
      DensityMode{0x2u, /*elementwise=*/true, /*propto=*/false}.byte();
  g.add_op(OP_SUM_VEC, {out}, lp);
  g.result_slot = lp;
  Executor ex(std::move(g));
  for (int64_t i = 0; i < kN; ++i) ex.value_ptr(ys)[i] = y[(size_t)i];
  ex.param_ptr(mu)[0] = 0.0;
  ex.value_ptr(sigma)[0] = 1.0;
  double grad = 0.0;
  ex.gradient(&grad);
  expect_ulp("elementwise normal broadcast mu adjoint", grad, want.value(),
             kUlp);
}

// ordered_logistic over kN observations that share one set of cut points.
// The reference takes each observation's cut-point gradient from Stan Math's
// scalar call and adds them exactly. A cut point receives positive terms from
// the category above it and negative ones from the category below, so the
// budget is in ULPs of the sum of their magnitudes: that is the scale at which
// any inexact sum, pairwise or compensated by Neumaier's rule, rounds them.
void test_ordered_logistic_cutpoints() {
  constexpr int kCuts = 3;
  std::vector<int> y((size_t)kN);
  std::vector<double> eta((size_t)kN);
  for (int64_t i = 0; i < kN; ++i) {
    y[(size_t)i] = 1 + (int)(i % (kCuts + 1));
    eta[(size_t)i] = -0.4 + 0.3 / (1 + i % 13);
  }
  const double cut_values[kCuts] = {-1.1, 0.35, 1.6};
  ExactSum want[kCuts], magnitude[kCuts];
  for (int64_t i = 0; i < kN; ++i) {
    stan::math::nested_rev_autodiff nested;
    Eigen::Matrix<stan::math::var, -1, 1> c(kCuts);
    for (int k = 0; k < kCuts; ++k) c(k) = cut_values[k];
    stan::math::var lp =
        stan::math::ordered_logistic_lpmf(y[(size_t)i], eta[(size_t)i], c);
    stan::math::grad(lp.vi_);
    for (int k = 0; k < kCuts; ++k) {
      want[k].add(c(k).adj());
      magnitude[k].add(std::abs(c(k).adj()));
    }
  }
  Graph g;
  const int ls = g.add_slot(kN, false), cs = g.add_slot(kCuts, true);
  const int lp = g.add_slot(1, false);
  g.add_op(OP_ORDERED_LOGISTIC_LPMF, {ls, cs}, lp,
           testutil::vectorized_idata(y, kCuts, {1}, 0));
  g.ops.back().variant = 0x02u;
  g.result_slot = lp;
  Executor ex(std::move(g));
  for (int64_t i = 0; i < kN; ++i) ex.value_ptr(ls)[i] = eta[(size_t)i];
  for (int k = 0; k < kCuts; ++k) ex.param_ptr(cs)[k] = cut_values[k];
  std::vector<double> grad(kCuts);
  ex.gradient(grad.data());
  for (int k = 0; k < kCuts; ++k)
    expect_ulp_of("ordered_logistic cut point " + std::to_string(k), grad[k],
                  want[k].value(), magnitude[k].value(), kUlp);
}

// The SUM_VEC that re-roll and partition build over the merged target terms.
void test_merged_term_sum() {
  const std::vector<double> x = terms(kN, -0.9, 91);
  ExactSum want;
  for (double v : x) want.add(v);
  Graph g;
  const int xs = g.add_slot(kN, true), lp = g.add_slot(1, false);
  g.add_op(OP_SUM_VEC, {xs}, lp);
  g.ops.back().variant = (uint8_t)SumGrouping::Pairwise;
  g.result_slot = lp;
  Executor ex(std::move(g));
  for (int64_t i = 0; i < kN; ++i) ex.param_ptr(xs)[i] = x[(size_t)i];
  std::vector<double> grad((size_t)kN);
  const double value = ex.gradient(grad.data());
  expect_ulp("pairwise SUM_VEC value", value, want.value(), kUlp);
}

// Terms that cancel: pairs of x and nearly -x with magnitudes up to 1e8, in a
// fixed shuffled order, so that their sum is about 1e-8 of the largest term.
std::vector<double> cancelling_terms(int64_t n) {
  std::vector<double> x((size_t)n);
  uint64_t state = 0x9e3779b97f4a7c15ull;
  const auto next = [&state] {
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    return (double)(state >> 11) / 9007199254740992.0;  // [0, 1)
  };
  for (int64_t i = 0; i < n; ++i)
    x[(size_t)i] =
        i % 2 ? -x[(size_t)i - 1] + (next() - 0.5) : 1e8 * (next() - 0.5);
  for (int64_t i = n - 1; i > 0; --i)
    std::swap(x[(size_t)i], x[(size_t)(next() * (double)(i + 1))]);
  return x;
}

// sum() and mean() of more than kPlainSumMax values are compensated,
// whatever the grouping that Stan Math would use: within one ULP of the
// exact sum even when the terms cancel. Up to kPlainSumMax values keep Stan
// Math's grouping.
void test_sum_and_mean_values() {
  for (const int64_t n :
       {kPlainSumMax + 1, int64_t{46}, int64_t{750}, int64_t{3000}}) {
    const std::vector<double> x = cancelling_terms(n);
    ExactSum want;
    for (double v : x) want.add(v);
    const std::string at = " n=" + std::to_string(n);
    for (const SumGrouping grouping :
         {SumGrouping::InOrder, SumGrouping::Packet, SumGrouping::Phased})
      expect_ulp("sum_grouped " + std::to_string((int)grouping) + at,
                 sum_grouped(x.data(), n, grouping, 1), want.value(), 1);
    for (const uint16_t opcode : {OP_SUM_VEC, OP_MEAN}) {
      Graph g;
      const int xs = g.add_slot(n, true), out = g.add_slot(1, false);
      g.add_op(opcode, {xs}, out);
      g.result_slot = out;
      Executor ex(std::move(g));
      for (int64_t i = 0; i < n; ++i) ex.param_ptr(xs)[i] = x[(size_t)i];
      std::vector<double> grad((size_t)n);
      const double value = ex.gradient(grad.data());
      const bool mean = opcode == OP_MEAN;
      expect_ulp((mean ? "mean" : "sum") + at, value,
                 mean ? want.value() / (double)n : want.value(), 1);
      expect_eq((mean ? "mean adjoint" : "sum adjoint") + at, grad[0],
                mean ? 1.0 / (double)n : 1.0);
    }
  }
  // The cutoff: 32 cancelling terms are added in order, bit for bit, and 33
  // are compensated, where the plain sum is more than one ULP off.
  for (const int64_t n : {kPlainSumMax, kPlainSumMax + 1}) {
    const std::vector<double> x = cancelling_terms(n);
    double in_order = 0.0;
    for (double v : x) in_order += v;
    ExactSum want;
    for (double v : x) want.add(v);
    const double got = sum_grouped(x.data(), n, SumGrouping::InOrder, 0);
    const std::string at = " n=" + std::to_string(n);
    expect_true("plain sum is off by more than one ULP" + at,
                ulp_distance(in_order, want.value()) > 1);
    if (n <= kPlainSumMax)
      expect_eq("in order" + at, got, in_order);
    else
      expect_ulp("compensated" + at, got, want.value(), 1);
  }
}

// Non-finite terms and zeros give what a plain sum gives.
void test_compensated_sum_edges() {
  const double inf = std::numeric_limits<double>::infinity();
  const auto plain = [](const std::vector<double>& x) {
    double s = 0.0;
    for (double v : x) s += v;
    return s;
  };
  std::vector<std::pair<std::string, std::vector<double>>> cases = {
      {"empty", {}},
      {"forty -0.0", std::vector<double>(40, -0.0)},
      {"forty +0.0", std::vector<double>(40, 0.0)},
      {"overflow", std::vector<double>(40, 1e308)},
  };
  std::vector<double> x(40, 1.0);
  x[7] = inf;
  cases.push_back({"one inf", x});
  x[12] = -inf;
  cases.push_back({"inf and -inf", x});
  x.assign(40, 1.0);
  x[13] = std::numeric_limits<double>::quiet_NaN();
  cases.push_back({"nan", x});
  for (const auto& [name, terms] : cases) {
    const double got = compensated_sum(terms.data(), (int64_t)terms.size());
    const double want = plain(terms);
    if (std::isnan(want))
      expect_eq_or_both_nan("compensated_sum " + name, got, want);
    else
      expect_true("compensated_sum " + name,
                  got == want && std::signbit(got) == std::signbit(want));
  }
}

// A summed integer-outcome density over kN observations, as re-roll and
// partition emit for a loop of scalar calls. The value is held to the exact
// sum of the scalar calls' values, which CmdStan adds one at a time. The
// gradient and a rejection are held to Stan Math's vector call, bit for bit.
template <typename Scalar, typename Vector>
void check_summed_density(const std::string& name, uint16_t opcode, bool propto,
                          const std::vector<int>& y, std::vector<double> arg,
                          double invalid, const Scalar& scalar,
                          const Vector& vector) {
  ExactSum want, magnitude;
  for (size_t i = 0; i < y.size(); ++i) {
    const double t = scalar(y[i], arg[i], propto);
    want.add(t);
    magnitude.add(std::abs(t));
  }
  const auto run = [&](const std::vector<double>& values,
                       std::vector<double>& grad) {
    Graph g;
    const int a = g.add_slot((int64_t)values.size(), true),
              lp = g.add_slot(1, false);
    g.add_op(opcode, {a}, lp, y);
    g.ops.back().variant = DensityMode{0x1u, false, propto}.byte();
    g.result_slot = lp;
    Executor ex(std::move(g));
    for (size_t i = 0; i < values.size(); ++i) ex.param_ptr(a)[i] = values[i];
    grad.assign(values.size(), 0.0);
    return ex.gradient(grad.data());
  };
  const auto reference = [&](const std::vector<double>& values,
                             std::vector<double>& grad) {
    stan::math::nested_rev_autodiff nested;
    Eigen::Matrix<stan::math::var, -1, 1> a(values.size());
    for (size_t i = 0; i < values.size(); ++i) a(i) = values[i];
    stan::math::var lp = vector(y, a, propto);
    stan::math::grad(lp.vi_);
    grad.resize(values.size());
    for (size_t i = 0; i < values.size(); ++i) grad[i] = a(i).adj();
  };
  std::vector<double> grad, want_grad;
  const double value = run(arg, grad);
  reference(arg, want_grad);
  expect_ulp_of(name + " summed value", value, want.value(), magnitude.value(),
                kUlp);
  for (size_t i = 0; i < arg.size(); ++i)
    expect_eq(name + " gradient " + std::to_string(i), grad[i], want_grad[i]);

  arg[arg.size() / 2] = invalid;
  std::string got_error, want_error;
  try {
    run(arg, grad);
  } catch (const std::exception& e) {
    got_error = e.what();
  }
  try {
    reference(arg, want_grad);
  } catch (const std::exception& e) {
    want_error = e.what();
  }
  expect_true(name + " rejects as Stan Math does: " + got_error,
              !want_error.empty() && got_error == want_error);
}

void test_summed_densities() {
  std::vector<int> y((size_t)kN), count((size_t)kN);
  std::vector<double> theta((size_t)kN), alpha((size_t)kN);
  for (int64_t i = 0; i < kN; ++i) {
    y[(size_t)i] = (int)(i % 3 != 0);
    theta[(size_t)i] = 0.55 + 0.3 / (2 + i % 17);
    count[(size_t)i] = 3 + (int)(i % 7);
    alpha[(size_t)i] = 1.2 + 0.4 / (1 + i % 11);
  }
  for (bool propto : {false, true}) {
    const std::string form = propto ? " propto" : "";
    check_summed_density(
        "bernoulli" + form, OP_BERNOULLI_LPMF, propto, y, theta, 1.5,
        [](int n, double t, bool p) {
          return p ? stan::math::bernoulli_lpmf<true>(n, stan::math::var(t))
                         .val()
                   : stan::math::bernoulli_lpmf<false>(n, t);
        },
        [](const std::vector<int>& n, const auto& t, bool p) {
          return p ? stan::math::bernoulli_lpmf<true>(n, t)
                   : stan::math::bernoulli_lpmf<false>(n, t);
        });
    check_summed_density(
        "poisson_log" + form, OP_POISSON_LOG_LPMF, propto, count, alpha,
        std::nan(""),
        [](int n, double a, bool p) {
          return p ? stan::math::poisson_log_lpmf<true>(n, stan::math::var(a))
                         .val()
                   : stan::math::poisson_log_lpmf<false>(n, a);
        },
        [](const std::vector<int>& n, const auto& a, bool p) {
          return p ? stan::math::poisson_log_lpmf<true>(n, a)
                   : stan::math::poisson_log_lpmf<false>(n, a);
        });
  }
}

}  // namespace

int main() {
  test_broadcast_scalar_adjoints();
  test_elementwise_density_scalar_adjoint();
  test_ordered_logistic_cutpoints();
  test_merged_term_sum();
  test_summed_densities();
  test_sum_and_mean_values();
  test_compensated_sum_edges();
  return finish("test_sum_accuracy");
}
