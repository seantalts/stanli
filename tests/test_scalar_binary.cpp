// STANLI_SCALAR_BINARY_LIST ops vs the stan-math var overloads CmdStan's
// generated C++ would run: value and every gradient lane, bitwise, across
// the four shape combos (vv, vs, sv, ss). In-support inputs per function.
#include "graph_helpers.hpp"

#include <stanli/graph.hpp>
#include <stanli/optable.hpp>

#include <stan/math.hpp>
#include <cstdio>
#include <string>
#include <vector>

static int failures = 0;
static void expect_eq(const std::string& what, double got, double want) {
  if (got != want) {
    ++failures;
    std::printf("FAIL %-28s got %.17g want %.17g\n", what.c_str(), got, want);
  }
}

using stan::math::var;

template <typename F>
static void check_shape(const std::string& tag, uint16_t opcode,
                        const std::vector<double>& av,
                        const std::vector<double>& bv, F&& f) {
  const int64_t n = std::max((int64_t)av.size(), (int64_t)bv.size());
  auto r = stanli::testutil::run_op_sum(opcode, n, {av, bv}, {true, true});
  // Reference: scalar var call per lane, scalars shared -- the graph
  // stan-math's own scalar overloads build for the same expression.
  std::vector<var> a, b;
  for (double x : av) a.emplace_back(x);
  for (double x : bv) b.emplace_back(x);
  var lp = 0.0;
  for (int64_t i = 0; i < n; ++i)
    lp += f(a[av.size() == 1 ? 0 : i], b[bv.size() == 1 ? 0 : i]);
  lp.grad();
  expect_eq(tag + " lp", r.value, lp.val());
  size_t gi = 0;
  for (const auto& x : a) {
    expect_eq(tag + " g" + std::to_string(gi), r.grad[gi], x.adj());
    ++gi;
  }
  for (const auto& x : b) {
    expect_eq(tag + " g" + std::to_string(gi), r.grad[gi], x.adj());
    ++gi;
  }
  stan::math::recover_memory();
}

template <typename F>
static void check_fn(const std::string& name, uint16_t opcode,
                     const std::vector<double>& av,
                     const std::vector<double>& bv, F&& f) {
  check_shape(name + " vv", opcode, av, bv, f);
  check_shape(name + " vs", opcode, av, {bv[0]}, f);
  check_shape(name + " sv", opcode, {av[0]}, bv, f);
  check_shape(name + " ss", opcode, {av[0]}, {bv[0]}, f);
}

// The int-argument half of the surface, STANLI_SCALAR_BINARY_INT_FIRST_LIST
// and its SECOND twin. stan-math takes the order/count/exponent as an int,
// so the reference passes an int too, and only the real side has a
// gradient: `iv` never appears in the expected adjoints.
template <typename F>
static void check_int_shape(const std::string& tag, uint16_t opcode,
                            bool int_first, const std::vector<double>& rv,
                            const std::vector<int>& iv, F&& f) {
  const int64_t n = std::max((int64_t)rv.size(), (int64_t)iv.size());
  // An int argument reaches a kernel through an ordinary double slot.
  const std::vector<double> id(iv.begin(), iv.end());
  auto r =
      int_first
          ? stanli::testutil::run_op_sum(opcode, n, {id, rv}, {false, true})
          : stanli::testutil::run_op_sum(opcode, n, {rv, id}, {true, false});
  std::vector<var> a;
  for (double x : rv) a.emplace_back(x);
  var lp = 0.0;
  for (int64_t i = 0; i < n; ++i)
    lp += f(a[rv.size() == 1 ? 0 : i], iv[iv.size() == 1 ? 0 : i]);
  lp.grad();
  expect_eq(tag + " lp", r.value, lp.val());
  for (size_t gi = 0; gi < a.size(); ++gi)
    expect_eq(tag + " g" + std::to_string(gi), r.grad[gi], a[gi].adj());
  stan::math::recover_memory();
}

template <typename F>
static void check_int_fn(const std::string& name, uint16_t opcode,
                         bool int_first, const std::vector<double>& rv,
                         const std::vector<int>& iv, F&& f) {
  check_int_shape(name + " vv", opcode, int_first, rv, iv, f);
  check_int_shape(name + " vs", opcode, int_first, rv, {iv[0]}, f);
  check_int_shape(name + " sv", opcode, int_first, {rv[0]}, iv, f);
  check_int_shape(name + " ss", opcode, int_first, {rv[0]}, {iv[0]}, f);
}

int main() {
  using namespace stanli;
#define F(fn) \
  [](const var& x, const var& y) -> var { return stan::math::fn(x, y); }

  // Free-sign pairs.
  const std::vector<double> xs{0.5, -1.2, 2.0, 0.3};
  const std::vector<double> ys{1.5, 0.7, -0.4, 2.2};
  // Positive pairs, for the log-domain functions.
  const std::vector<double> ps{0.9, 1.7, 0.35, 2.4};
  const std::vector<double> qs{1.1, 0.6, 2.2, 0.8};
  // n >= k >= 0, for lchoose.
  const std::vector<double> ns{7.5, 4.0, 9.25, 6.0};
  const std::vector<double> ks{2.5, 1.0, 3.0, 0.5};
  // a > b elementwise AND against each other's first element, so every
  // broadcast shape stays inside log_inv_logit_diff's support.
  const std::vector<double> hi{1.5, 0.7, 2.0, 2.2};
  const std::vector<double> lo{0.5, -1.2, -0.4, 0.3};

  check_fn("atan2", OP_ATAN2, xs, ys, F(atan2));
  check_fn("beta", OP_BETA_FN, ps, qs, F(beta));
  check_fn("fdim", OP_FDIM, xs, ys, F(fdim));
  check_fn("fmax", OP_FMAX, xs, ys, F(fmax));
  check_fn("fmin", OP_FMIN, xs, ys, F(fmin));
  check_fn("fmod", OP_FMOD, xs, ys, F(fmod));
  check_fn("gamma_p", OP_GAMMA_P, ps, qs, F(gamma_p));
  check_fn("gamma_q", OP_GAMMA_Q, ps, qs, F(gamma_q));
  check_fn("hypot", OP_HYPOT, xs, ys, F(hypot));
  check_fn("lbeta", OP_LBETA, ps, qs, F(lbeta));
  check_fn("lchoose", OP_LCHOOSE, ns, ks, F(binomial_coefficient_log));
  check_fn("lmultiply", OP_LMULTIPLY, xs, qs, F(lmultiply));
  check_fn("log_falling_factorial", OP_LOG_FALLING_FACTORIAL, ns, ks,
           F(log_falling_factorial));
  check_fn("log_inv_logit_diff", OP_LOG_INV_LOGIT_DIFF, hi, lo,
           F(log_inv_logit_diff));
  check_fn("log_modified_bessel_first_kind", OP_LOG_MODIFIED_BESSEL_1, ps, qs,
           F(log_modified_bessel_first_kind));
  check_fn("log_rising_factorial", OP_LOG_RISING_FACTORIAL, ps, qs,
           F(log_rising_factorial));
  check_fn("owens_t", OP_OWENS_T, xs, ys, F(owens_t));
#undef F

  // The int-argument functions. Domains: the second-kind Bessels and
  // lmgamma need a positive argument (lmgamma a positive one past
  // (k-1)/2), binary_log_loss a probability and a 0/1 outcome, and both
  // factorials a nonnegative count.
#define FI(fn) [](const var& x, int k) -> var { return stan::math::fn(k, x); }
  const std::vector<int> orders{0, 1, 2, 3};
  check_int_fn("bessel_first_kind", OP_BESSEL_1, true, xs, orders,
               FI(bessel_first_kind));
  check_int_fn("bessel_second_kind", OP_BESSEL_2, true, ps, orders,
               FI(bessel_second_kind));
  check_int_fn("modified_bessel_first_kind", OP_MODIFIED_BESSEL_1, true, xs,
               orders, FI(modified_bessel_first_kind));
  check_int_fn("modified_bessel_second_kind", OP_MODIFIED_BESSEL_2, true, ps,
               orders, FI(modified_bessel_second_kind));
  check_int_fn("binary_log_loss", OP_BINARY_LOG_LOSS, true,
               {0.2, 0.6, 0.35, 0.8}, {0, 1, 1, 0}, FI(binary_log_loss));
  check_int_fn("lmgamma", OP_LMGAMMA, true, {0.9, 1.7, 1.35, 2.4}, {1, 2, 1, 2},
               FI(lmgamma));
#undef FI
#define FI(fn) [](const var& x, int k) -> var { return stan::math::fn(x, k); }
  const std::vector<int> counts{0, 1, 2, 3};
  check_int_fn("falling_factorial", OP_FALLING_FACTORIAL, false, ps, counts,
               FI(falling_factorial));
  check_int_fn("rising_factorial", OP_RISING_FACTORIAL, false, ps, counts,
               FI(rising_factorial));
  check_int_fn("ldexp", OP_LDEXP, false, xs, {-2, 0, 1, 5}, FI(ldexp));
#undef FI

  // A matrix against an int array pairs n[i][j] with m(i, j), which is not
  // the flat pairing: the matrix is column-major and the array's trailing
  // extents are row-major. The lowering hands the kernel the leaf's rows
  // and cols to undo that (IntLane in kernels/scalar_binary.cpp). Distinct
  // exponents make a swapped pairing a factor of two, and the oracle is
  // stan-math's own (matrix, nested int vector) overload.
  {
    const std::vector<double> mcol{0.5, -1.25, 2.5, 0.75};  // column-major
    const std::vector<double> nrow{1, 3, -2, 4};            // row-major
    auto r = stanli::testutil::run_op_sum(OP_LDEXP, 4, {mcol, nrow},
                                          {true, false}, {2, 2});
    Eigen::Matrix<var, -1, -1> m(2, 2);
    for (int j = 0; j < 2; ++j)
      for (int i = 0; i < 2; ++i) m(i, j) = mcol[j * 2 + i];
    const std::vector<std::vector<int>> nn{{1, 3}, {-2, 4}};
    auto out = stan::math::ldexp(m, nn);
    // Column-major, the order the graph's OP_SUM_VEC walks its lanes.
    var lp = 0.0;
    for (int j = 0; j < 2; ++j)
      for (int i = 0; i < 2; ++i) lp += out(i, j);
    lp.grad();
    expect_eq("ldexp matrix-int lp", r.value, lp.val());
    for (int j = 0; j < 2; ++j)
      for (int i = 0; i < 2; ++i)
        expect_eq("ldexp matrix-int g" + std::to_string(j * 2 + i),
                  r.grad[j * 2 + i], m(i, j).adj());
    stan::math::recover_memory();
  }

  // A data argument must reach stan-math as a double, not a promoted var:
  // the var,double overloads of fmax and fmin give TIES to the var side,
  // and the var,var overloads give them to b. The conformance sweep hit
  // this through int arrays (int slots are always data) probed at exact
  // ties: fmax(2 + eps*theta, 2) at theta = 0.
  {
    auto r =
        stanli::testutil::run_op_sum(OP_FMAX, 1, {{2.0}, {2.0}}, {true, false});
    var a = 2.0;
    var lp = stan::math::fmax(a, 2.0);
    lp.grad();
    expect_eq("fmax tie-vs-data lp", r.value, lp.val());
    expect_eq("fmax tie-vs-data g0", r.grad[0], a.adj());
    stan::math::recover_memory();
  }
  {
    auto r =
        stanli::testutil::run_op_sum(OP_FMIN, 1, {{2.0}, {2.0}}, {true, false});
    var a = 2.0;
    var lp = stan::math::fmin(a, 2.0);
    lp.grad();
    expect_eq("fmin tie-vs-data lp", r.value, lp.val());
    expect_eq("fmin tie-vs-data g0", r.grad[0], a.adj());
    stan::math::recover_memory();
  }
  {
    // Data on the left, parameter on the right: dv overloads.
    auto r =
        stanli::testutil::run_op_sum(OP_FMAX, 1, {{2.0}, {2.0}}, {false, true});
    var b = 2.0;
    var lp = stan::math::fmax(2.0, b);
    lp.grad();
    expect_eq("fmax data-tie-v lp", r.value, lp.val());
    expect_eq("fmax data-tie-v g0", r.grad[0], b.adj());
    stan::math::recover_memory();
  }

  if (failures == 0) std::printf("test_scalar_binary OK\n");
  return failures == 0 ? 0 : 1;
}
