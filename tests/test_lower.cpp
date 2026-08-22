// The graph compiler on eight schools: MIR text + data -> graph whose
// log_prob gradient matches a var reference that mirrors the lowering's
// evaluation order. Plus the unsupported-construct error path.
#include "env_helpers.hpp"
#include "categorical_check_mir.hpp"
#include "stdout_capture.hpp"
#include <stanli/compile.hpp>
#include <stanli/graph.hpp>
#include <stanli/optable.hpp>
#include <stanli/packet.hpp>
#include <stanli/wa_interp.hpp>

#include <stan/math.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <fstream>
#include <sstream>
#include <string>

static int failures = 0;
static void check(bool ok, const std::string& what) {
  if (!ok) {
    ++failures;
    std::printf("FAIL %s\n", what.c_str());
  }
}
static void expect_eq(const std::string& what, double got, double want) {
  if (got != want) {
    ++failures;
    std::printf("FAIL %-16s got %.17g want %.17g\n", what.c_str(), got, want);
  }
}
static int64_t ulp_key(double d) {
  int64_t i;
  std::memcpy(&i, &d, sizeof(i));
  return i < 0 ? std::numeric_limits<int64_t>::min() - i : i;
}
// Project parity budget: up to 2 ULP vs references is acceptable.
static void expect_ulp(const std::string& what, double got, double want) {
  const int64_t d = std::llabs(ulp_key(got) - ulp_key(want));
  if (d > 2) {
    ++failures;
    std::printf("FAIL %-16s got %.17g want %.17g (%lld ulp)\n", what.c_str(),
                got, want, (long long)d);
  }
}
static std::string slurp(const std::string& path) {
  std::ifstream f(path);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

static stanli::DataMap bound_check_data(double raw = 0.0, int N = 1, int M = 1,
                                        int R = 1, int C = 1, int BR = 1,
                                        int BC = 1) {
  stanli::DataMap d;
  d.set_real("d", 0.0);
  d.set_real("raw", raw);
  d.set_int("N", N);
  d.set_int("M", M);
  d.set_real_array("lo", std::vector<double>((size_t)M, -10.0));
  d.set_int("R", R);
  d.set_int("C", C);
  d.set_int("BR", BR);
  d.set_int("BC", BC);
  d.set_real_array("matrix_lo",
                   std::vector<double>((size_t)BR * (size_t)BC, -10.0),
                   {BR, BC});
  return d;
}

static const double kY[8] = {28, 8, -3, 7, -1, 1, 18, 12};
static const double kSigma[8] = {15, 10, 16, 11, 9, 11, 10, 18};

// Mirrors the lowering: reads (with jacobian) in declaration order, then
// statements in program order, target = add_n(terms..., jacs...).
static void reference(const double* q, double* lp_out, double* grad_out) {
  using stan::math::var;
  const int J = 8;
  var mu = q[0];
  var log_tau = q[1];
  Eigen::Matrix<var, -1, 1> tilde(J);
  for (int i = 0; i < J; ++i) tilde(i) = q[2 + i];

  var jac = 0.0;
  var tau = stan::math::lb_constrain<true>(log_tau, 0.0, jac);

  // theta = mu + tau * tilde, lowered as MUL(s,v) then ADD(s,v).
  Eigen::Matrix<var, -1, 1> theta =
      stan::math::add(mu, stan::math::multiply(tau, tilde));

  // ~ statements lower propto=true with activity from MIR adlevels: exactly
  // the instantiations CmdStan's generated C++ uses (data args stay double).
  Eigen::Map<const Eigen::VectorXd> y(kY, J);
  Eigen::Map<const Eigen::VectorXd> sigma(kSigma, J);
  var t1 = stan::math::normal_lpdf<true>(mu, 0.0, 5.0);
  var t2 = stan::math::cauchy_lpdf<true>(tau, 0.0, 5.0);
  var t3 = stan::math::normal_lpdf<true>(tilde, 0.0, 1.0);
  var t4 = stan::math::normal_lpdf<true>(y, theta, sigma);
  var lp = ((((t1 + t2) + t3) + t4) + jac);
  lp.grad();

  *lp_out = lp.val();
  grad_out[0] = mu.adj();
  grad_out[1] = log_tau.adj();
  for (int i = 0; i < J; ++i) grad_out[2 + i] = tilde(i).adj();
  stan::math::recover_memory();
}

int main() {
  // These fixtures pin ULP-level parity with DEFAULT CmdStan, whose AoS
  // Matrix<var> paths run scalar libm per element; the packet path answers
  // to `stanc --O1` instead and is verified there.
  stanli::set_packet_math(false);
  using namespace stanli;

  DataMap data;
  data.set_int("J", 8);
  data.set_real_array("y", std::vector<double>(kY, kY + 8));
  data.set_real_array("sigma", std::vector<double>(kSigma, kSigma + 8));

  CompiledModel cm = compile_model(slurp("tests/fixtures/es.tmir.sexp"), data);
  check(cm.n_unconstrained == 10, "10 unconstrained params");
  check(cm.param_names.size() == 3 && cm.param_names[0] == "mu" &&
            cm.param_names[1] == "tau" && cm.param_names[2] == "theta_tilde",
        "param names");

  Executor ex(std::move(cm.graph));
  cm.bind(ex);

  const double qs[3][10] = {
      {4.0, 1.0, 0.1, -0.2, 0.3, -0.4, 0.5, -0.6, 0.7, -0.8},
      {0.0, -1.5, 1.2, 0.8, -1.1, 0.05, -0.3, 0.9, -1.4, 0.2},
      {-2.5, 0.3, -0.7, 1.5, 0.6, -0.9, 1.1, 0.4, -0.2, -1.3}};
  for (int c = 0; c < 3; ++c) {
    for (int i = 0; i < 10; ++i) ex.params_data()[i] = qs[c][i];
    double grad[10], lp_ref, grad_ref[10];
    const double lp = ex.gradient(grad);
    reference(qs[c], &lp_ref, grad_ref);
    const std::string tag = "case" + std::to_string(c);
    expect_eq(tag + " lp", lp, lp_ref);
    for (int i = 0; i < 10; ++i)
      expect_eq(tag + " g" + std::to_string(i), grad[i], grad_ref[i]);
  }

  // For loops unroll: scalar-loop normal model vs per-term var reference.
  {
    DataMap d;
    d.set_int("N", 3);
    d.set_real_array("y", {1.0, 2.0, 3.0});
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/loopy.tmir.sexp"), d);
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    lex.params_data()[0] = 0.4;
    double g = 0, lp = lex.gradient(&g);

    using stan::math::var;
    var mu = 0.4;
    const double yv[3] = {1.0, 2.0, 3.0};
    var acc = 0.0;
    for (int n = 0; n < 3; ++n)
      acc = acc + stan::math::normal_lpdf<true>(yv[n], mu, 1.0);
    acc.grad();
    expect_eq("loopy lp", lp, acc.val());
    expect_eq("loopy dmu", g, mu.adj());
    stan::math::recover_memory();
  }

  // Static if inside an unrolled loop, condition indexing data by the loop
  // variable (M0 capture-recapture pattern): the branch is resolved at
  // compile time per iteration.
  {
    DataMap d;
    d.set_int("M", 3);
    d.set_int_array("s", {2, 0, 1});
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/staticif.tmir.sexp"), d);
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    lex.params_data()[0] = 0.3;
    double g = 0, lp = lex.gradient(&g);

    using stan::math::var;
    var pu = 0.3;
    // Mirror the lowering: model terms sum first, jacobians append last.
    var lj = 0.0;
    var pc = stan::math::lub_constrain(pu, 0.0, 1.0, lj);
    var acc = 0.0;
    const int sv[3] = {2, 0, 1};
    for (int i = 0; i < 3; ++i) {
      if (sv[i] > 0)
        acc = acc + stan::math::binomial_lpmf<false>(sv[i], 5, pc);
      else
        acc = acc + stan::math::bernoulli_lpmf<false>(0, pc);
    }
    acc = acc + lj;
    acc.grad();
    expect_eq("staticif lp", lp, acc.val());
    expect_eq("staticif dp", g, pu.adj());
    stan::math::recover_memory();
  }

  // Row of a 2-D int data array as a density outcome: y[i] reaches the
  // kernel as a T-length int array (Mb/Mt/irt_2pl pattern).
  {
    DataMap d =
        DataMap::from_json(R"({"M": 2, "T": 3, "y": [[1, 0, 1], [0, 0, 1]]})");
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/introw.tmir.sexp"), d);
    check(lm.n_unconstrained == 3, "introw 3 unconstrained");
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    for (int i = 0; i < 3; ++i) lex.params_data()[i] = 0.2 * (i + 1) - 0.3;
    double grad[3] = {0, 0, 0};
    const double lp = lex.gradient(grad);

    using stan::math::var;
    Eigen::Matrix<var, -1, 1> pu(3);
    for (int i = 0; i < 3; ++i) pu(i) = 0.2 * (i + 1) - 0.3;
    var lj = 0.0;
    Eigen::Matrix<var, -1, 1> pc = stan::math::lub_constrain(pu, 0.0, 1.0, lj);
    const std::vector<std::vector<int>> yv = {{1, 0, 1}, {0, 0, 1}};
    var acc = 0.0;
    for (int i = 0; i < 2; ++i)
      acc = acc + stan::math::bernoulli_lpmf<false>(yv[i], pc);
    acc = acc + lj;
    acc.grad();
    expect_eq("introw lp", lp, acc.val());
    for (int i = 0; i < 3; ++i)
      expect_eq("introw g" + std::to_string(i), grad[i], pu(i).adj());
    stan::math::recover_memory();
  }

  // Data-only expressions with no native lowering const-fold at compile
  // time: mean/sd bounds, negative_infinity in log_sum_exp, constant lccdf.
  {
    DataMap d;
    d.set_int("N", 2);
    d.set_real_array("y", {1.3, -0.7});
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/dfold.tmir.sexp"), d);
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    lex.params_data()[0] = 0.25;
    lex.params_data()[1] = -0.6;
    double grad[2] = {0, 0};
    const double lp = lex.gradient(grad);

    using stan::math::var;
    const double yv[2] = {1.3, -0.7};
    const double m = (yv[0] + yv[1]) / 2.0;
    const double s = std::sqrt(
        ((yv[0] - m) * (yv[0] - m) + (yv[1] - m) * (yv[1] - m)) / 1.0);
    Eigen::Matrix<var, -1, 1> muu(2);
    muu << 0.25, -0.6;
    var lj = 0.0;
    Eigen::Matrix<var, -1, 1> muc =
        stan::math::lub_constrain(muu, m - 3 * s, m + 3 * s, lj);
    var acc = stan::math::normal_lpdf<false>(yv[0], muc(0), 1.0);
    acc = acc + stan::math::log_sum_exp(
                    stan::math::normal_lpdf<false>(yv[1], muc(1), 1.0),
                    var(-std::numeric_limits<double>::infinity()));
    acc = acc + stan::math::student_t_lccdf(0.0, 3.0, 0.0, 10.0);
    acc = acc + lj;
    acc.grad();
    expect_ulp("dfold lp", lp, acc.val());
    for (int i = 0; i < 2; ++i)
      expect_ulp("dfold g" + std::to_string(i), grad[i], muu(i).adj());
    stan::math::recover_memory();
  }

  // Transformed-data UDF (loops, range slices, 2-D writes, return) plus
  // ternary selection on data conditions, for both branch polarities.
  for (int flag = 0; flag <= 1; ++flag) {
    DataMap d = DataMap::from_json(
        std::string(R"({"N": 3, "K": 3, "flag": )") + std::to_string(flag) +
        R"(, "W": [[1.0, 2.0, 4.0], [1.0, 0.5, 2.0], [1.0, -1.0, 6.0]]})");
    CompiledModel lm = compile_model(slurp("tests/fixtures/udf.tmir.sexp"), d);
    check(lm.n_unconstrained == 3, "udf 3 unconstrained");
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    const double q[3] = {0.4, -0.9, 1.7};
    for (int i = 0; i < 3; ++i) lex.params_data()[i] = q[i];
    double grad[3] = {0, 0, 0};
    const double lp = lex.gradient(grad);

    // adj column 2 exactly as the td interpreter computes it.
    const double c2[3] = {2.0, 0.5, -1.0};
    double m = 0;
    for (double v : c2) m += v;
    m /= 3.0;
    double s2 = 0;
    for (double v : c2) s2 += (v - m) * (v - m);
    const double sdv = std::sqrt(s2 / 2.0);

    using stan::math::var;
    Eigen::Matrix<var, -1, 1> b(3);
    b << q[0], q[1], q[2];
    var t1 = stan::math::normal_lpdf<false>(b, m, sdv * 2.0);
    var t2 = stan::math::normal_lpdf<false>(b(0), flag ? 0.5 : -0.5, 1.0);
    var t3 = flag ? stan::math::normal_lpdf<false>(b(1), b(0), 1.0)
                  : stan::math::normal_lpdf<false>(b(1), 0.0, 1.0);
    var acc = ((t1 + t2) + t3);
    acc.grad();
    const std::string tag = "udf f" + std::to_string(flag);
    expect_eq(tag + " lp", lp, acc.val());
    for (int i = 0; i < 3; ++i)
      expect_eq(tag + " g" + std::to_string(i), grad[i], b(i).adj());
    stan::math::recover_memory();
  }

  // Transformed-data While loop with a short-circuit guard, append_row.
  {
    DataMap d;
    d.set_int("N", 4);
    d.set_real_array("y", {2.0, 1.5, -1.0, 3.0});
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/tdext.tmir.sexp"), d);
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    lex.params_data()[0] = 0.3;
    double g = 0, lp = lex.gradient(&g);

    // c = 2 (first two positive), s = sum of y twice + c + sum of A col 1
    // where A[j] = [j, j] (row writes into a matrix).
    double s = 0;
    const double yv[4] = {2.0, 1.5, -1.0, 3.0};
    for (int rep = 0; rep < 2; ++rep)
      for (double v : yv) s += v;
    s += 2.0;
    double colsum = 0;
    for (int j = 1; j <= 4; ++j) colsum += j;
    s += colsum;
    using stan::math::var;
    var mu = 0.3;
    var acc = stan::math::normal_lpdf<false>(mu, s, 1.0);
    acc.grad();
    expect_eq("tdext lp", lp, acc.val());
    expect_eq("tdext dmu", g, mu.adj());
    stan::math::recover_memory();
  }

  // Transformed-data use of fmax/fmin/inv/inv_logit: the function
  // vocabulary must be the same one the ODE-side interpreter accepts.
  {
    DataMap d;
    d.set_real("a", 1.25);
    d.set_real("b", -0.5);
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/tdvocab.tmir.sexp"), d);
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    lex.params_data()[0] = 0.3;
    double g = 0;
    const double lp = lex.gradient(&g);

    const double m = std::fmax(1.25, -0.5) + std::fmin(1.25, -0.5) +
                     stan::math::inv(-0.5) + stan::math::inv_logit(1.25);
    // The four the register-machine compiler knew and the interpreter did
    // not. The compiled path falls back to the interpreter, so the
    // interpreter's vocabulary has to cover the compiler's.
    const double dens = stan::math::inv_gamma_lpdf(1.5, 2, 3) +
                        stan::math::weibull_lpdf(1.5, 2, 3) +
                        stan::math::logistic_lpdf(0.25, 0, 1) +
                        stan::math::double_exponential_lpdf(0.25, 0, 1);
    using stan::math::var;
    var mu = 0.3;
    var acc = stan::math::normal_lpdf<false>(mu, m + dens, 1.0);
    acc.grad();
    expect_eq("tdvocab lp", lp, acc.val());
    expect_eq("tdvocab dmu", g, mu.adj());
    stan::math::recover_memory();
  }

  // Deep array literals in transformed data have to land in a slot with the
  // same layout the JSON reader gives a data-block array of the same shape.
  // Rank 2 cannot tell the two apart -- an array-of-arrays and a matrix share
  // a flattening -- so the disagreement only shows from rank 3 down, where a
  // literal reached its slot with the trailing two extents swapped: a wrong
  // log density and wrong gradients, no error and no NaN.
  {
    DataMap d = DataMap::from_json(R"({
      "d2": [[11.0,12.0],[21.0,22.0]],
      "d3": [[[111.0,112.0],[121.0,122.0]],[[211.0,212.0],[221.0,222.0]]],
      "d4": [[[[1111.0,1112.0],[1121.0,1122.0]],
              [[1211.0,1212.0],[1221.0,1222.0]]],
             [[[2111.0,2112.0],[2121.0,2122.0]],
              [[2211.0,2212.0],[2221.0,2222.0]]]],
      "dvv": [[[111.0,112.0,113.0],[121.0,122.0,123.0]],
              [[211.0,212.0,213.0],[221.0,222.0,223.0]]],
      "dm": [[[111.0,112.0,113.0],[121.0,122.0,123.0]],
             [[211.0,212.0,213.0],[221.0,222.0,223.0]]]
    })");
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/ndlit.tmir.sexp"), d);
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    const double q[3] = {0.3, 0.5, -0.25};
    for (int i = 0; i < 3; ++i) lex.params_data()[i] = q[i];
    double grad[3] = {0, 0, 0};
    const double lp = lex.gradient(grad);

    // Every element of both the literal and the data holds its own decimal
    // index code, and each is weighted by that same code, so the reference is
    // a sum of squares and any permutation of a layout moves it. Codes are
    // small integers: the sums are exact, and expect_eq can be exact too.
    double wt = 0, s3 = 0;
    for (int i = 1; i <= 2; ++i)
      for (int j = 1; j <= 2; ++j) {
        const double c2 = 10 * i + j;
        wt += c2 * c2;
        for (int k = 1; k <= 2; ++k) {
          const double c3 = 100 * i + 10 * j + k;
          wt += c3 * c3;
          s3 += c3 * c3;
          for (int l = 1; l <= 2; ++l) {
            const double c4 = 1000 * i + 100 * j + 10 * k + l;
            wt += c4 * c4;
          }
        }
        for (int k = 1; k <= 3; ++k) {
          const double c = 100 * i + 10 * j + k;
          wt += 2 * c * c;  // the array of vectors and the array of matrices
        }
      }
    // literal minus data, element by element: zero only if both paths agree.
    expect_eq("ndlit ddiff", grad[0], 0.0);
    expect_eq("ndlit dwt", grad[1], wt);
    // The interpreter's own N-D read of the literal, inside transformed data.
    expect_eq("ndlit ds3", grad[2], s3);
    expect_eq("ndlit lp", lp, wt * q[1] + s3 * q[2]);
  }

  // Model-block UDFs on parameters, inlined: scalar chain, vector return,
  // statement body with local accumulator + loop.
  {
    DataMap d;
    d.set_int("N", 3);
    d.set_real_array("y", {1.1, -0.4, 2.2});
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/udflp.tmir.sexp"), d);
    check(lm.n_unconstrained == 4, "udflp 4 unconstrained");
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    const double q[4] = {0.7, 0.2, -1.1, 0.9};
    for (int i = 0; i < 4; ++i) lex.params_data()[i] = q[i];
    double grad[4] = {0, 0, 0, 0};
    const double lp = lex.gradient(grad);

    using stan::math::var;
    var mu = q[0];
    Eigen::Matrix<var, -1, 1> dv(3);
    dv << q[1], q[2], q[3];
    const double yarr[3] = {1.1, -0.4, 2.2};
    Eigen::Map<const Eigen::VectorXd> ym(yarr, 3);
    var af = mu * 2.0 + 0.5;
    var t1 = stan::math::normal_lpdf<false>(ym, af, 1.0);
    Eigen::Matrix<var, -1, 1> hv = stan::math::divide(dv, 2.0);
    var t2 = stan::math::normal_lpdf<false>(hv, 0.0, 1.0);
    var s = 0.0;
    for (int i = 0; i < 3; ++i) s = s + dv(i);
    var t3 = stan::math::normal_lpdf<false>(s / 2.0, 0.0, 1.0);
    var acc = ((t1 + t2) + t3);
    acc.grad();
    expect_eq("udflp lp", lp, acc.val());
    expect_eq("udflp gmu", grad[0], mu.adj());
    for (int i = 0; i < 3; ++i)
      expect_eq("udflp gd" + std::to_string(i), grad[1 + i], dv(i).adj());
    stan::math::recover_memory();
  }

  // Overloaded user functions (issue #125): stanc3 keeps every overload of
  // cox_lccdf under the same fdname, and --O1 inlines the lcdf wrapper so
  // log_prob calls both the vector and the scalar overload directly.
  // Resolution must go by argument type, not last-definition-wins.
  {
    DataMap d = DataMap::from_json(
        R"({"N": 3, "Y": [1.0, 2.0, 3.0], "bhaz": [0.4, 0.5, 0.6],
            "cbhaz": [0.2, 0.3, 0.4]})");
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/overload.tmir.sexp"), d);
    check(lm.n_unconstrained == 1, "overload 1 unconstrained");
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    lex.params_data()[0] = 0.7;
    double grad[1] = {0};
    const double lp = lex.gradient(grad);

    using stan::math::var;
    var alpha = 0.7;
    Eigen::VectorXd bh(3), cb(3);
    bh << 0.4, 0.5, 0.6;
    cb << 0.2, 0.3, 0.4;
    Eigen::Matrix<var, -1, 1> mu = alpha * bh;
    var acc = -stan::math::dot_product(cb, mu) +
              stan::math::log1m_exp(-cb(0) * alpha) +
              stan::math::normal_lpdf<false>(alpha, 0.0, 1.0);
    acc.grad();
    expect_ulp("overload lp", lp, acc.val());
    // alpha collects adjoints from three terms; the graph and the var tape
    // associate that sum differently, so this is a few-ulp comparison.
    check(std::fabs(grad[0] - alpha.adj()) < 1e-12,
          "overload galpha " + std::to_string(grad[0]) + " vs " +
              std::to_string(alpha.adj()));
    stan::math::recover_memory();
  }

  // Data-dependent gather width (issue #133): the same program gathers a
  // slice whose length transformed data computes from the data. Nev > 0 is
  // an ordinary gather; Nev == 0 must compile and contribute exactly zero
  // to lp and the gradient.
  {
    const double yv[4] = {1.5, -0.5, 2.0, 0.25};
    const double q[4] = {0.3, -0.8, 0.1, 0.6};
    for (int empty = 0; empty < 2; ++empty) {
      DataMap d;
      d.set_int("N", 4);
      d.set_real_array("Y", {yv[0], yv[1], yv[2], yv[3]});
      d.set_int_array("cens", empty ? std::vector<int>{1, 1, 1, 1}
                                    : std::vector<int>{0, 1, 0, 1});
      CompiledModel lm =
          compile_model(slurp("tests/fixtures/emptygather.tmir.sexp"), d);
      check(lm.n_unconstrained == 4, "emptygather 4 unconstrained");
      Executor lex(std::move(lm.graph));
      lm.bind(lex);
      for (int i = 0; i < 4; ++i) lex.params_data()[i] = q[i];
      double grad[4] = {0, 0, 0, 0};
      const double lp = lex.gradient(grad);

      using stan::math::var;
      Eigen::Matrix<var, -1, 1> mu(4);
      mu << q[0], q[1], q[2], q[3];
      var acc = 0.0;
      if (!empty) {
        Eigen::Matrix<var, -1, 1> mg(2);
        mg << mu(0) * 2.0, mu(2) * 2.0;
        Eigen::VectorXd yg(2);
        yg << yv[0], yv[2];
        acc += stan::math::dot_product(yg, stan::math::exp(mg)) +
               stan::math::sum(mg);
      }
      acc += stan::math::normal_lpdf<false>(mu, 0.0, 1.0);
      acc.grad();
      const std::string tag = empty ? "emptygather empty" : "emptygather full";
      expect_ulp(tag + " lp", lp, acc.val());
      for (int i = 0; i < 4; ++i)
        check(std::fabs(grad[i] - mu(i).adj()) < 1e-12,
              tag + " g" + std::to_string(i));
      stan::math::recover_memory();
    }
  }

  // An empty int data array as a gather index: JSON [] must stay
  // integer-typed, the empty density term must contribute nothing, and the
  // generated quantity over the empty gather must write 0.
  {
    const double yv[4] = {1.5, -0.5, 2.0, 0.25};
    for (int empty = 0; empty < 2; ++empty) {
      DataMap d = DataMap::from_json(
          empty ? R"({"M": 0, "idx": [], "Y": [1.5, -0.5, 2.0, 0.25]})"
                : R"({"M": 2, "idx": [3, 1], "Y": [1.5, -0.5, 2.0, 0.25]})");
      CompiledModel lm =
          compile_model(slurp("tests/fixtures/emptyidx.tmir.sexp"), d);
      Executor lex(std::move(lm.graph));
      lm.bind(lex);
      lex.params_data()[0] = 0.4;
      double grad[1] = {0};
      const double lp = lex.gradient(grad);

      using stan::math::var;
      var mu = 0.4;
      var acc = stan::math::normal_lpdf<false>(mu, 0.0, 2.0);
      if (!empty) {
        Eigen::Matrix<var, -1, 1> yg(2);
        yg << yv[2], yv[0];
        acc += stan::math::normal_lpdf<false>(yg, mu, 1.0);
      }
      acc.grad();
      const std::string tag = empty ? "emptyidx empty" : "emptyidx full";
      expect_ulp(tag + " lp", lp, acc.val());
      check(std::fabs(grad[0] - mu.adj()) < 1e-12, tag + " grad");
      stan::math::recover_memory();
      check(lm.write_array && lm.write_array->truncated.empty(),
            tag + " write_array compiled");
      if (lm.write_array && lm.write_array->truncated.empty()) {
        Executor wex(std::move(lm.write_array->graph));
        lm.write_array->bind(wex);
        wex.params_data()[0] = 0.4;
        wex.run_forward_only();
        bool found = false;
        for (const auto& col : lm.write_array->columns) {
          if (col.name != "gsum") continue;
          found = true;
          check(*wex.value_ptr(col.slot) == (empty ? 0.0 : yv[2] + yv[0]),
                tag + " gsum value");
        }
        check(found, tag + " has gsum");
      }
    }
  }

  // Data-dependent range bounds: hi < lo is an empty slice under CmdStan's
  // rvalue semantics, whatever the endpoints, on the array, vector, and
  // matrix row-range paths alike.
  {
    for (int empty = 0; empty < 2; ++empty) {
      DataMap d = DataMap::from_json(
          empty ? R"({"K": 0, "lo": 5, "hi": 2, "Y": [1.5, -0.5, 2.0, 0.25],
                      "Zm": [[1, 5], [2, 6], [3, 7], [4, 8]]})"
                : R"({"K": 2, "lo": 2, "hi": 3, "Y": [1.5, -0.5, 2.0, 0.25],
                      "Zm": [[1, 5], [2, 6], [3, 7], [4, 8]]})");
      CompiledModel lm =
          compile_model(slurp("tests/fixtures/rangeclamp.tmir.sexp"), d);
      Executor lex(std::move(lm.graph));
      lm.bind(lex);
      lex.params_data()[0] = -0.3;
      double grad[1] = {0};
      const double lp = lex.gradient(grad);

      using stan::math::var;
      var mu = -0.3;
      var acc = stan::math::normal_lpdf<false>(mu, 0.0, 2.0);
      if (empty) {
        // sum over an empty slice is 0 on the array and matrix paths; the
        // empty vector slice contributes nothing at all.
        acc += stan::math::normal_lpdf<false>(0.0, mu, 1.0) * 2.0;
      } else {
        acc += stan::math::normal_lpdf<false>(1.0 + 2.0, mu, 1.0);
        Eigen::Matrix<var, -1, 1> yg(2);
        yg << -0.5, 2.0;
        acc += stan::math::normal_lpdf<false>(yg, mu, 1.0);
        acc += stan::math::normal_lpdf<false>(1.0 + 2.0, mu, 1.0);
      }
      acc.grad();
      const std::string tag = empty ? "rangeclamp empty" : "rangeclamp full";
      expect_ulp(tag + " lp", lp, acc.val());
      check(std::fabs(grad[0] - mu.adj()) < 1e-12, tag + " grad");
      stan::math::recover_memory();
    }
  }

  // Index bounds are data, so the lowering must reject an out-of-bounds
  // index at bind time the way CmdStan rejects it at runtime; the silent
  // alternative is reading a neighboring arena slot. hi < lo ranges stay
  // empty. One fixture; the base dataset is in bounds and each violation
  // overrides one index.
  {
    const auto base_data = [] {
      DataMap d = DataMap::from_json(
          R"({"k": 2, "idx": [1, 4], "lo": 2, "hi": 3, "i1": 1, "j1": 2,
              "rl": 2, "rh": 3, "m": 3, "Y": [1.5, -0.5, 2.0, 0.25],
              "Zm": [[1, 5], [2, 6], [3, 7], [4, 8]]})");
      return d;
    };
    const std::string mir = slurp("tests/fixtures/oob.tmir.sexp");

    const auto reference = [&](bool rows_empty, double q) {
      using stan::math::var;
      var mu = q;
      const double yv[4] = {1.5, -0.5, 2.0, 0.25};
      const double zm[4][2] = {{1, 5}, {2, 6}, {3, 7}, {4, 8}};
      Eigen::Matrix<var, -1, 1> v(4);
      for (int i = 0; i < 4; ++i) v(i) = yv[i] + mu;
      Eigen::Matrix<var, -1, -1> M(4, 2);
      for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 2; ++j) M(i, j) = zm[i][j] + mu;
      var acc = stan::math::normal_lpdf<false>(v(1), 0.0, 1.0);
      Eigen::Matrix<var, -1, 1> vg(2);
      vg << v(0), v(3);
      acc += stan::math::normal_lpdf<false>(vg, 0.0, 1.0);
      Eigen::Matrix<var, -1, 1> vs(2);
      vs << v(1), v(2);
      acc += stan::math::normal_lpdf<false>(vs, 0.0, 1.0);
      acc += stan::math::normal_lpdf<false>(M(0, 0) + M(0, 1), 0.0, 1.0);
      acc += stan::math::normal_lpdf<false>(
          M(0, 1) + M(1, 1) + M(2, 1) + M(3, 1), 0.0, 1.0);
      if (rows_empty) {
        // sum over the empty row range is 0 on both terms.
        acc += stan::math::normal_lpdf<false>(0.0, 0.0, 1.0) * 2.0;
      } else {
        acc += stan::math::normal_lpdf<false>(
            M(1, 0) + M(2, 0) + M(1, 1) + M(2, 1), 0.0, 1.0);
        acc += stan::math::normal_lpdf<false>(M(1, 1) + M(2, 1), 0.0, 1.0);
      }
      acc += stan::math::normal_lpdf<false>(3.0, 0.0, 1.0);  // tds = xs[3]
      acc += stan::math::normal_lpdf<false>(mu, 0.0, 2.0);
      return acc;
    };

    const auto run_case = [&](DataMap d, bool rows_empty,
                              const std::string& tag) {
      CompiledModel lm = compile_model(mir, d);
      Executor lex(std::move(lm.graph));
      lm.bind(lex);
      lex.params_data()[0] = 0.35;
      double grad[1] = {0};
      const double lp = lex.gradient(grad);
      using stan::math::var;
      var acc = reference(rows_empty, 0.35);
      acc.grad();
      expect_ulp(tag + " lp", lp, acc.val());
      stan::math::recover_memory();
    };
    run_case(base_data(), false, "oob ctrl");
    {
      DataMap d = base_data();
      d.set_int("rl", 5);
      d.set_int("rh", 2);
      run_case(std::move(d), true, "oob rows-empty");
    }

    const auto expect_oob = [&](const char* name, long v,
                                const std::string& tag) {
      DataMap d = base_data();
      d.set_int(name, v);
      bool threw = false;
      try {
        compile_model(mir, d);
      } catch (const std::exception& ex) {
        threw =
            std::string(ex.what()).find("out of bounds") != std::string::npos;
        if (!threw)
          std::printf("  %s threw without 'out of bounds': %s\n", tag.c_str(),
                      ex.what());
      }
      check(threw, tag + " rejected");
    };
    expect_oob("k", 7, "oob v[7]");
    expect_oob("hi", 9, "oob v[2:9]");
    expect_oob("i1", 5, "oob M[5]");
    expect_oob("j1", 5, "oob M[:,5]");
    expect_oob("rh", 9, "oob M[2:9]");
    expect_oob("m", 9, "oob interp xs[9]");
    {
      DataMap d = base_data();
      d.set_int_array("idx", {1, 9});
      bool threw = false;
      try {
        compile_model(mir, d);
      } catch (const std::exception& ex) {
        threw =
            std::string(ex.what()).find("out of bounds") != std::string::npos;
      }
      check(threw, "oob v[[1,9]] rejected");
    }
  }

  // Index forms on parameters: gather, Between read/write, matrix row and
  // column slices, column writes.
  {
    DataMap d = DataMap::from_json(R"({"N": 4, "idx": [2, 1, 3, 2]})");
    CompiledModel lm = compile_model(slurp("tests/fixtures/idx.tmir.sexp"), d);
    check(lm.n_unconstrained == 9, "idx 9 unconstrained");
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    const double q[9] = {0.5, -0.7, 1.2,                   // v
                         0.1, -0.3, 0.6, 0.9, -1.1, 0.2};  // M col-major
    for (int i = 0; i < 9; ++i) lex.params_data()[i] = q[i];
    double grad[9];
    const double lp = lex.gradient(grad);

    using stan::math::var;
    Eigen::Matrix<var, -1, 1> v(3);
    v << q[0], q[1], q[2];
    Eigen::Matrix<var, -1, 1> M(6);  // col-major flat
    for (int i = 0; i < 6; ++i) M(i) = q[3 + i];
    const int idx[4] = {2, 1, 3, 2};
    Eigen::Matrix<var, -1, 1> g(4);
    for (int k = 0; k < 4; ++k) g(k) = v(idx[k] - 1);
    Eigen::Matrix<var, -1, 1> w(2);
    w << v(1), v(2);
    Eigen::Matrix<var, -1, 1> row1(3), col3(2), lcol2(2), v12(2);
    row1 << M(0), M(2), M(4);  // row 1 of 2x3 col-major: stride 2
    col3 << M(4), M(5);        // column 3: offset 4
    lcol2 = w;                 // L[:,2] = w
    v12 << v(0), v(1);
    var t1 = stan::math::normal_lpdf<false>(g, 0.0, 1.0);
    var t2 = stan::math::normal_lpdf<false>(v12, 0.0, 2.0);
    var t3 = stan::math::normal_lpdf<false>(row1, 0.0, 1.0);
    var t4 = stan::math::normal_lpdf<false>(col3, 0.0, 3.0);
    var t5 = stan::math::normal_lpdf<false>(lcol2, 0.0, 2.0);
    var t6 = stan::math::normal_lpdf<false>(w, 1.0, 1.0);
    var acc = (((((t1 + t2) + t3) + t4) + t5) + t6);
    acc.grad();
    expect_eq("idx lp", lp, acc.val());
    for (int i = 0; i < 3; ++i)
      expect_eq("idx gv" + std::to_string(i), grad[i], v(i).adj());
    for (int i = 0; i < 6; ++i)
      expect_eq("idx gM" + std::to_string(i), grad[3 + i], M(i).adj());
    stan::math::recover_memory();
  }

  // A logical view belongs to the name binding, not to the flat parameter
  // slot it aliases. M, r, v, and q are simultaneously live here and all
  // share storage with different matrix/vector labels.
  {
    try {
      DataMap d;
      d.set_real_array("A", {1.0, 3.0, 2.0, 4.0}, {2, 2});
      d.set_real_array("y", {0.5, -0.25}, {2});
      CompiledModel lm =
          compile_model(slurp("tests/fixtures/viewalias.tmir.sexp"), d);
      check(lm.n_unconstrained == 6, "viewalias 6 unconstrained");
      Executor lex(std::move(lm.graph));
      lm.bind(lex);
      const double points[2][6] = {{0.25, -0.5, 0.75, -1.0, 1.25, -1.5},
                                   {-0.25, 0.5, -0.75, 1.0, -1.25, 1.5}};
      for (int c = 0; c < 2; ++c) {
        const double* q = points[c];
        for (int i = 0; i < 6; ++i) lex.params_data()[i] = q[i];
        double grad[6] = {0, 0, 0, 0, 0, 0};
        const double lp = lex.gradient(grad);

        // M is 2x3 column-major. Its first row is q[0], q[2] + q[5],
        // q[4] after the indexed update, while the still-live flat v alias
        // remains q. D pins provenance after a parameter element write.
        // E pins both parameter-dependent whole-assignment arms while
        // retaining its matrix shape. B is updated from a data-only
        // reduction and must remain eligible as normal_id_glm's data matrix.
        // All inputs are binary fractions, so this independent closed form
        // and its full gradient are exact.
        const double aq0 = std::abs(q[0]);
        const double r0 = 0.5 - (q[0] + 3.0 * q[1]);
        const double r1 = -0.25 - (3.0 * q[0] + 4.0 * q[1]);
        const double want_lp =
            q[0] * q[0] + (q[2] + q[5]) * q[1] + q[4] * q[2] +
            2.0 * (q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3] +
                   q[4] * q[4] + q[5] * q[5]) +
            4.0 * q[0] + 4.0 * q[1] + q[0] * q[1] +
            aq0 * (4.0 * q[0] + 6.0 * q[1]) - 0.5 * r0 * r0 - 0.5 * r1 * r1;
        const double e_g0 =
            q[0] > 0 ? 8.0 * q[0] + 6.0 * q[1] : -8.0 * q[0] - 6.0 * q[1];
        const double want_grad[6] = {
            6.0 * q[0] + 4.0 + q[1] + e_g0 + r0 + 3.0 * r1,
            4.0 * q[1] + q[2] + q[5] + 4.0 + q[0] + 6.0 * aq0 + 3.0 * r0 +
                4.0 * r1,
            4.0 * q[2] + q[1] + q[4],
            4.0 * q[3],
            4.0 * q[4] + q[2],
            4.0 * q[5] + q[1]};
        const std::string tag = "viewalias" + std::to_string(c);
        expect_eq(tag + " lp", lp, want_lp);
        for (int i = 0; i < 6; ++i)
          expect_eq(tag + " g" + std::to_string(i), grad[i], want_grad[i]);
      }

      // The same logical matrix alias must survive the separately lowered
      // write_array graph used for transformed-parameter CSV columns.
      check(lm.write_array && lm.write_array->truncated.empty(),
            "viewalias write_array compiled");
      if (lm.write_array && lm.write_array->truncated.empty()) {
        Executor wex(std::move(lm.write_array->graph));
        lm.write_array->bind(wex);
        for (int i = 0; i < 6; ++i) wex.params_data()[i] = points[0][i];
        wex.run_forward_only();
        bool found_w = false;
        for (const auto& col : lm.write_array->columns) {
          if (col.name != "W") continue;
          found_w = true;
          check(col.naming == CompiledModel::ParamView::Naming::Matrix &&
                    col.rows == 2,
                "viewalias write_array matrix metadata");
          const double* w = wex.value_ptr(col.slot);
          for (int i = 0; i < 6; ++i)
            expect_eq("viewalias write W" + std::to_string(i), w[i],
                      points[0][i]);
        }
        check(found_w, "viewalias write_array has W");
      }
    } catch (const std::exception& e) {
      ++failures;
      std::printf("FAIL viewalias compile: %s\n", e.what());
    }
  }

  // categorical_lpmf, scalar and array outcomes, on a simplex parameter.
  {
    DataMap d = DataMap::from_json(R"({"K": 3, "y": 2, "ys": [3, 1, 3]})");
    CompiledModel lm = compile_model(slurp("tests/fixtures/cat.tmir.sexp"), d);
    check(lm.n_unconstrained == 2, "cat 2 unconstrained");
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    lex.params_data()[0] = 0.3;
    lex.params_data()[1] = -0.8;
    double grad[2];
    const double lp = lex.gradient(grad);

    using stan::math::var;
    Eigen::Matrix<var, -1, 1> q(2);
    q << 0.3, -0.8;
    var lj = 0.0;
    Eigen::Matrix<var, -1, 1> theta = stan::math::simplex_constrain(q, lj);
    var t1 = stan::math::categorical_lpmf<false>(2, theta);
    const std::vector<int> ys = {3, 1, 3};
    var t2 = stan::math::categorical_lpmf<false>(ys, theta);
    var acc = (t1 + t2) + lj;
    acc.grad();
    expect_eq("cat lp", lp, acc.val());
    for (int i = 0; i < 2; ++i)
      expect_eq("cat g" + std::to_string(i), grad[i], q(i).adj());
    stan::math::recover_memory();
  }

  // An empty array outcome validates a parameter vector but contributes a
  // disconnected zero. In particular, an unselected zero probability must
  // not be reached by a zero-adjoint log backward and turn into 0 / 0.
  {
    std::string mir = slurp("tests/fixtures/cat.tmir.sexp");
    auto replace_all = [&](const std::string& from, const std::string& to) {
      for (size_t pos = mir.find(from); pos != std::string::npos;
           pos = mir.find(from, pos + to.size()))
        mir.replace(pos, from.size(), to);
    };
    replace_all("(pattern (Lit Int 3))", "(pattern (Lit Int 0))");
    replace_all("(constrain Simplex)", "(constrain Identity)");

    DataMap d;
    d.set_int("K", 2);
    d.set_int("y", 1);
    d.set_int_array("ys", {});
    CompiledModel lm = compile_model(mir, d);
    check(lm.n_unconstrained == 2, "empty categorical identity width");
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    lex.params_data()[0] = 1.0;
    lex.params_data()[1] = 0.0;
    double grad[2];
    const double lp = lex.gradient(grad);

    using stan::math::var;
    Eigen::Matrix<var, -1, 1> theta(2);
    theta << 1.0, 0.0;
    const std::vector<int> empty;
    var acc = stan::math::categorical_lpmf<false>(1, theta) +
              stan::math::categorical_lpmf<false>(empty, theta);
    acc.grad();
    expect_eq("empty categorical lp", lp, acc.val());
    for (int i = 0; i < 2; ++i)
      expect_eq("empty categorical g" + std::to_string(i), grad[i],
                theta(i).adj());
    stan::math::recover_memory();
  }

  // Parameter-dependent categorical-logit calls keep their exact value and
  // pullback. Exercise both scalar
  // and array outcomes under normalized and propto MIR flags.
  for (bool propto : {false, true}) {
    std::string mir = slurp("tests/fixtures/cat.tmir.sexp");
    auto replace_all = [&](const std::string& from, const std::string& to) {
      for (size_t pos = mir.find(from); pos != std::string::npos;
           pos = mir.find(from, pos + to.size()))
        mir.replace(pos, from.size(), to);
    };
    replace_all("categorical_lpmf", "categorical_logit_lpmf");
    replace_all("(constrain Simplex)", "(constrain Identity)");
    if (propto) replace_all("(FnLpmf false)", "(FnLpmf true)");

    DataMap d = DataMap::from_json(R"({"K":3,"y":2,"ys":[3,1,3]})");
    CompiledModel lm = compile_model(mir, d);
    check(lm.n_unconstrained == 3,
          "categorical logit identity parameter width");
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    const double point[3] = {-0.4, 0.2, 0.9};
    std::copy(std::begin(point), std::end(point), lex.params_data());
    double grad[3];
    const double lp = lex.gradient(grad);

    using stan::math::var;
    Eigen::Matrix<var, -1, 1> beta(3);
    for (int i = 0; i < 3; ++i) beta(i) = point[i];
    const std::vector<int> ys = {3, 1, 3};
    var acc = propto ? stan::math::categorical_logit_lpmf<true>(2, beta) +
                           stan::math::categorical_logit_lpmf<true>(ys, beta)
                     : stan::math::categorical_logit_lpmf<false>(2, beta) +
                           stan::math::categorical_logit_lpmf<false>(ys, beta);
    acc.grad();
    const std::string mode = propto ? "propto" : "normalized";
    expect_eq("categorical logit parameter " + mode + " lp", lp, acc.val());
    for (int i = 0; i < 3; ++i)
      expect_eq(
          "categorical logit parameter " + mode + " g" + std::to_string(i),
          grad[i], beta(i).adj());
    stan::math::recover_memory();
  }

  // Reverse replay must accumulate into an adjoint already written by a
  // later consumer in the same order as Stan's tape. Grouping the density's
  // two logit contributions before adding that existing 0.1 changes one bit.
  {
    Graph g;
    const int beta = g.add_slot(3, true);
    const int outcome = g.add_slot(1, false);
    const int cat = g.add_slot(1, false);
    const int cat_op = g.add_op(OP_CATEGORICAL, {outcome, beta}, cat);
    auto spec = std::make_shared<CategoricalSpec>();
    spec->logit = true;
    spec->scalar_outcome = true;
    spec->arg_autodiff = true;
    spec->propto = false;
    g.ops[(size_t)cat_op].udata = spec.get();
    g.udata_pool.push_back(std::move(spec));
    const int beta0 = g.add_slot(1, false);
    g.add_op(OP_INDEX, {beta}, beta0, {0});
    const int scale = g.add_slot(1, false);
    const int linear = g.add_slot(1, false);
    g.add_op(OP_MUL, {beta0, scale}, linear);
    const int total = g.add_slot(1, false);
    g.add_op(OP_ADD, {cat, linear}, total);
    g.result_slot = total;

    Executor ex(std::move(g));
    std::fill(ex.params_data(), ex.params_data() + 3, -2.0);
    ex.value_ptr(outcome)[0] = 1.0;
    ex.value_ptr(scale)[0] = 0.1;
    double got_grad[3];
    const double got = ex.gradient(got_grad);

    Eigen::Matrix<stan::math::var, -1, 1> ref_beta(3);
    ref_beta << -2.0, -2.0, -2.0;
    stan::math::var ref_cat =
        stan::math::categorical_logit_lpmf<false>(1, ref_beta);
    stan::math::var ref_linear = ref_beta(0) * 0.1;
    stan::math::var ref = ref_cat + ref_linear;
    ref.grad();
    expect_eq("categorical existing-adjoint value", got, ref.val());
    for (int i = 0; i < 3; ++i)
      expect_eq("categorical existing-adjoint g" + std::to_string(i),
                got_grad[i], ref_beta(i).adj());
    stan::math::recover_memory();
  }

  // Array outcomes add one callback contribution per observation. Preserve
  // that exact accumulation order under a non-unit upstream adjoint; replacing
  // it with count * adjoint changes the selected gradient's low bit. The zero
  // unselected probability also pins Stan Math's disconnected-log topology.
  {
    Graph g;
    const int theta = g.add_slot(2, true);
    const int outcomes = g.add_slot(6, false);
    const int cat = g.add_slot(1, false);
    const int cat_op = g.add_op(OP_CATEGORICAL, {outcomes, theta}, cat);
    auto spec = std::make_shared<CategoricalSpec>();
    spec->logit = false;
    spec->scalar_outcome = false;
    spec->arg_autodiff = true;
    spec->propto = false;
    g.ops[(size_t)cat_op].udata = spec.get();
    g.udata_pool.push_back(std::move(spec));
    const int scale = g.add_slot(1, false);
    const int total = g.add_slot(1, false);
    g.add_op(OP_MUL, {cat, scale}, total);
    g.result_slot = total;

    Executor ex(std::move(g));
    ex.params_data()[0] = 1.0;
    ex.params_data()[1] = 0.0;
    std::fill(ex.value_ptr(outcomes), ex.value_ptr(outcomes) + 6, 1.0);
    ex.value_ptr(scale)[0] = 0.1;
    double got_grad[2];
    const double got = ex.gradient(got_grad);

    Eigen::Matrix<stan::math::var, -1, 1> ref_theta(2);
    ref_theta << 1.0, 0.0;
    const std::vector<int> ref_outcomes(6, 1);
    stan::math::var ref =
        stan::math::categorical_lpmf<false>(ref_outcomes, ref_theta) * 0.1;
    ref.grad();
    expect_eq("categorical array scaled value", got, ref.val());
    expect_eq("categorical array scaled selected", got_grad[0],
              ref_theta(0).adj());
    check(std::isnan(got_grad[1]) && std::isnan(ref_theta(1).adj()),
          "categorical array scaled unselected topology");
    stan::math::recover_memory();
  }

  // A user density is templated on each actual scalar type. The same
  // unqualified vector formal is double when called with data and var when
  // called with an autodiff local, even when both graph values are constant.
  for (const std::string& fn : {"categorical_lpmf", "categorical_logit_lpmf"}) {
    for (bool propto : {false, true}) {
      for (bool local : {false, true}) {
        for (bool body_local : {false, true}) {
          DataMap d;
          d.set_int("outcome", 2);
          const std::vector<double> arg =
              fn == "categorical_lpmf" ? std::vector<double>{0.2, 0.3, 0.5}
                                       : std::vector<double>{-1.0, 0.0, 1.0};
          d.set_real_array("arg", arg);
          CompiledModel lm = compile_model(
              categorical_udf_mir(fn, propto, local, body_local), d);
          Executor ex(std::move(lm.graph));
          lm.bind(ex);
          ex.params_data()[0] = 0.75;
          double grad = 0.0;
          const double got = ex.gradient(&grad);
          Eigen::VectorXd v(3);
          for (int i = 0; i < 3; ++i) v(i) = arg[(size_t)i];
          const double full =
              fn == "categorical_lpmf"
                  ? stan::math::categorical_lpmf<false>(2, v)
                  : stan::math::categorical_logit_lpmf<false>(2, v);
          const double want = 0.75 + (propto && !local ? 0.0 : full);
          const std::string tag =
              fn + std::string(propto ? " propto " : " normalized ") +
              (local ? "autodiff actual" : "data actual") +
              (body_local ? " through UDF local" : " direct formal");
          expect_ulp(tag + " value", got, want);
          expect_eq(tag + " anchor gradient", grad, 1.0);
        }
      }
    }
  }

  // Speculative constant folding must not move a data-only UDF's Stan Math
  // validation to model construction. On an invalid call it declines the
  // fold, leaving the exact runtime op and its original exception intact.
  for (const std::string& fn : {"categorical_lpmf", "categorical_logit_lpmf"}) {
    DataMap d;
    d.set_int("outcome", 4);
    const std::vector<double> arg = fn == "categorical_lpmf"
                                        ? std::vector<double>{0.2, 0.3, 0.5}
                                        : std::vector<double>{-1.0, 0.0, 1.0};
    d.set_real_array("arg", arg);
    try {
      CompiledModel lm =
          compile_model(categorical_udf_mir(fn, false, false), d);
      check(
          std::any_of(lm.graph.ops.begin(), lm.graph.ops.end(),
                      [](const Op& op) { return op.opcode == OP_CATEGORICAL; }),
          fn + " invalid data UDF retains runtime check");
      Executor ex(std::move(lm.graph));
      lm.bind(ex);
      ex.params_data()[0] = 0.75;
      double grad = 0.0;
      std::string got_message;
      try {
        (void)ex.gradient(&grad);
      } catch (const std::domain_error& e) {
        got_message = e.what();
      }
      Eigen::Map<const Eigen::VectorXd> v(arg.data(), (Eigen::Index)arg.size());
      std::string want_message;
      try {
        if (fn == "categorical_lpmf")
          (void)stan::math::categorical_lpmf<false>(4, v);
        else
          (void)stan::math::categorical_logit_lpmf<false>(4, v);
      } catch (const std::domain_error& e) {
        want_message = e.what();
      }
      check(!got_message.empty(), fn + " invalid data UDF rejects at runtime");
      check(got_message == want_message,
            fn + " invalid data UDF exception message");
    } catch (const std::exception& e) {
      ++failures;
      std::printf("FAIL %s invalid data UDF constructed late check: %s\n",
                  fn.c_str(), e.what());
    }

    try {
      CompiledModel lm = compile_model(categorical_udf_actual_mir(fn), d);
      check(
          std::any_of(lm.graph.ops.begin(), lm.graph.ops.end(),
                      [](const Op& op) { return op.opcode == OP_CATEGORICAL; }),
          fn + " invalid nested UDF actual retains runtime check");
      Executor ex(std::move(lm.graph));
      lm.bind(ex);
      ex.params_data()[0] = 0.75;
      double grad = 0.0;
      std::string got_message;
      try {
        (void)ex.gradient(&grad);
      } catch (const std::domain_error& e) {
        got_message = e.what();
      }
      check(!got_message.empty(),
            fn + " invalid nested UDF actual rejects at runtime");
    } catch (const std::exception& e) {
      ++failures;
      std::printf(
          "FAIL %s invalid nested UDF actual constructed late check: "
          "%s\n",
          fn.c_str(), e.what());
    }
  }

  // A generic UDF's return scalar type is selected from every actual, not
  // just from the expression on its return statement. Here the returned data
  // vector becomes a vector<var> because an otherwise-unused actual is var,
  // so an outer propto categorical call retains its summand.
  for (const std::string& fn : {"categorical_lpmf", "categorical_logit_lpmf"}) {
    DataMap d;
    d.set_int("outcome", 2);
    const std::vector<double> arg = fn == "categorical_lpmf"
                                        ? std::vector<double>{0.2, 0.3, 0.5}
                                        : std::vector<double>{-1.0, 0.0, 1.0};
    d.set_real_array("arg", arg);
    CompiledModel lm = compile_model(categorical_udf_return_mir(fn), d);
    Executor ex(std::move(lm.graph));
    lm.bind(ex);
    ex.params_data()[0] = 0.75;
    double grad = 0.0;
    const double got = ex.gradient(&grad);
    Eigen::Map<const Eigen::VectorXd> v(arg.data(), (Eigen::Index)arg.size());
    const double density =
        fn == "categorical_lpmf"
            ? stan::math::categorical_lpmf<false>(2, v)
            : stan::math::categorical_logit_lpmf<false>(2, v);
    expect_ulp(fn + " UDF instantiated return value", got, 0.75 + density);
    expect_eq(fn + " UDF instantiated return gradient", grad, 1.0);
  }

  // Each generic UDF formal keeps its own scalar type. An unrelated var
  // actual promotes locals and the return, but it must not turn a ternary
  // whose two arms derive from a data-instantiated vector formal into var.
  for (const std::string& fn : {"categorical_lpmf", "categorical_logit_lpmf"}) {
    DataMap d;
    d.set_int("outcome", 2);
    d.set_real_array("arg", fn == "categorical_lpmf"
                                ? std::vector<double>{0.2, 0.3, 0.5}
                                : std::vector<double>{-1.0, 0.0, 1.0});
    CompiledModel lm = compile_model(categorical_udf_ternary_type_mir(fn), d);
    Executor ex(std::move(lm.graph));
    lm.bind(ex);
    ex.params_data()[0] = 0.75;
    double grad = 0.0;
    expect_eq(fn + " per-formal ternary value", ex.gradient(&grad), 0.75);
    expect_eq(fn + " per-formal ternary gradient", grad, 1.0);

    CompiledModel promoted_lm =
        compile_model(categorical_udf_promoted_actual_mir(fn), d);
    Executor promoted_ex(std::move(promoted_lm.graph));
    promoted_lm.bind(promoted_ex);
    promoted_ex.params_data()[0] = 0.75;
    grad = 0.0;
    expect_eq(fn + " promoted int UDF actual value",
              promoted_ex.gradient(&grad), 0.75);
    expect_eq(fn + " promoted int UDF actual gradient", grad, 1.0);
  }

  // A data condition selects one arm at model construction, but the C++ type
  // of a mixed data/autodiff ternary is promoted before that selection. Its
  // propto term is therefore retained even when the data arm wins, directly
  // and when the ternary supplies an unqualified UDF formal.
  for (const std::string& fn : {"categorical_lpmf", "categorical_logit_lpmf"}) {
    for (bool through_udf : {false, true}) {
      for (int flag : {0, 1}) {
        DataMap d;
        d.set_int("flag", flag);
        d.set_int("outcome", 2);
        const std::vector<double> arg =
            fn == "categorical_lpmf" ? std::vector<double>{0.2, 0.3, 0.5}
                                     : std::vector<double>{-1.0, 0.0, 1.0};
        d.set_real_array("arg", arg);
        CompiledModel lm =
            compile_model(categorical_promoted_ternary_mir(fn, through_udf), d);
        Executor ex(std::move(lm.graph));
        lm.bind(ex);
        ex.params_data()[0] = 0.75;
        double grad = 0.0;
        const double got = ex.gradient(&grad);
        Eigen::Map<const Eigen::VectorXd> v(arg.data(),
                                            (Eigen::Index)arg.size());
        const double density =
            fn == "categorical_lpmf"
                ? stan::math::categorical_lpmf<false>(2, v)
                : stan::math::categorical_logit_lpmf<false>(2, v);
        const std::string tag =
            fn + std::string(through_udf ? " nested" : " direct") +
            (flag ? " data arm" : " autodiff arm");
        expect_ulp(tag + " promoted ternary value", got, 0.75 + density);
        expect_eq(tag + " promoted ternary gradient", grad, 1.0);
      }
    }
  }

  // An effectful data-only outcome is a runtime value, not a compile-time
  // immediate. It executes once, then the exact categorical op consumes that
  // value and supplies the same pullback as Stan Math.
  for (const std::string& fn : {"categorical_lpmf", "categorical_logit_lpmf"}) {
    std::string mir = slurp("tests/fixtures/cat.tmir.sexp");
    if (fn == "categorical_logit_lpmf") {
      for (size_t pos = mir.find("categorical_lpmf"); pos != std::string::npos;
           pos = mir.find("categorical_lpmf", pos + fn.size()))
        mir.replace(pos, std::strlen("categorical_lpmf"), fn);
      for (size_t pos = mir.find("(constrain Simplex)");
           pos != std::string::npos;
           pos = mir.find("(constrain Simplex)", pos + 20))
        mir.replace(pos, std::strlen("(constrain Simplex)"),
                    "(constrain Identity)");
    }
    mir = categorical_parameter_effect_mir(std::move(mir), fn);
    DataMap d = DataMap::from_json(R"({"K":3,"y":2,"ys":[3,1,3]})");
    std::optional<CompiledModel> lm;
    {
      stanli_test::StdoutCapture captured;
      lm = compile_model(mir, d);
      check(captured.finish().empty(), fn + " effect absent while compiling");
    }
    Executor ex(std::move(lm->graph));
    lm->bind(ex);

    std::vector<double> point;
    std::vector<double> want_grad;
    double want = 0.0;
    using stan::math::var;
    if (fn == "categorical_lpmf") {
      point = {0.3, -0.8};
      Eigen::Matrix<var, -1, 1> q(2);
      q << point[0], point[1];
      var jac = 0.0;
      const auto theta = stan::math::simplex_constrain(q, jac);
      const std::vector<int> ys = {3, 1, 3};
      var lp = jac + stan::math::categorical_lpmf<false>(2, theta) +
               stan::math::categorical_lpmf<false>(ys, theta);
      lp.grad();
      want = lp.val();
      want_grad = {q(0).adj(), q(1).adj()};
    } else {
      point = {-0.4, 0.2, 0.9};
      Eigen::Matrix<var, -1, 1> beta(3);
      for (int i = 0; i < 3; ++i) beta(i) = point[(size_t)i];
      const std::vector<int> ys = {3, 1, 3};
      var lp = stan::math::categorical_logit_lpmf<false>(2, beta) +
               stan::math::categorical_logit_lpmf<false>(ys, beta);
      lp.grad();
      want = lp.val();
      want_grad = {beta(0).adj(), beta(1).adj(), beta(2).adj()};
    }
    stan::math::recover_memory();
    std::copy(point.begin(), point.end(), ex.params_data());
    for (int run = 0; run < 2; ++run) {
      std::vector<double> grad(point.size());
      stanli_test::StdoutCapture captured;
      expect_eq(fn + " effect value " + std::to_string(run),
                ex.gradient(grad.data()), want);
      check(captured.finish() == "categorical effect\n",
            fn + " outcome effect executes once " + std::to_string(run));
      for (size_t i = 0; i < grad.size(); ++i)
        expect_eq(fn + " effect gradient " + std::to_string(i), grad[i],
                  want_grad[i]);
    }
  }

  // write_array is instantiated on doubles even though its parameter slots
  // vary with q. Normalized calls retain their value; propto calls validate
  // and return zero. The same rule holds when parameter control flow produces
  // the vector, because graph dependency and instantiated scalar type differ.
  for (const std::string& fn : {"categorical_lpmf", "categorical_logit_lpmf"}) {
    for (bool propto : {false, true}) {
      for (bool island : {false, true}) {
        DataMap d = DataMap::from_json(R"({"K":3,"y":2,"ys":[3,1,3]})");
        CompiledModel lm = compile_model(
            categorical_write_array_mir(slurp("tests/fixtures/cat.tmir.sexp"),
                                        fn, propto, island),
            d);
        const std::string tag =
            fn + std::string(propto ? " propto" : " normalized") +
            (island ? " island" : " direct");
        check(lm.write_array && lm.write_array->truncated.empty(),
              tag + " write_array compiles");
        if (!lm.write_array || !lm.write_array->truncated.empty()) continue;
        Executor ex(std::move(lm.write_array->graph));
        lm.write_array->bind(ex);
        ex.params_data()[0] = 0.0;
        ex.params_data()[1] = 0.0;
        ex.run_forward_only();
        bool found = false;
        for (const auto& col : lm.write_array->columns) {
          if (col.name != "categorical_value") continue;
          found = true;
          expect_ulp(tag + " write_array value", ex.value_ptr(col.slot)[0],
                     propto ? 0.0 : -std::log(3.0));
        }
        check(found, tag + " write_array column");
      }
    }
  }

  // An RNG anywhere in generated quantities selects the interpreted
  // write_array for the whole section. Categorical calls before that fallback
  // retain their scalar/array and normalized/propto semantics there too.
  for (const std::string& fn : {"categorical_lpmf", "categorical_logit_lpmf"}) {
    for (bool propto : {false, true}) {
      for (int outcome_count : {1, 3}) {
        DataMap d = DataMap::from_json(R"({"K":3,"y":2,"ys":[3,1,3]})");
        const std::string interpreted_mir = categorical_write_array_mir(
            slurp("tests/fixtures/cat.tmir.sexp"), fn, propto, false, false,
            true, outcome_count);
        CompiledModel lm = compile_model(interpreted_mir, d);
        const std::string tag =
            fn + std::string(propto ? " propto" : " normalized") +
            (outcome_count == 1 ? " scalar" : " array");
        check(lm.write_array && lm.write_array->interp,
              tag + " interpreted write_array selected");
        if (!lm.write_array || !lm.write_array->interp) continue;
        Executor ex(std::move(lm.graph));
        lm.bind(ex);
        std::fill(ex.params_data(), ex.params_data() + ex.n_params(), 0.0);
        ex.run_forward_only();
        WaRng rng(1234);
        const std::vector<double> row =
            lm.write_array->interp->eval(lm.constrained_env(ex), rng);
        bool found = false;
        for (const auto& col : lm.write_array->interp->columns()) {
          if (col.name != "categorical_value") continue;
          found = true;
          Eigen::VectorXd arg(3);
          if (fn == "categorical_lpmf")
            arg.setConstant(1.0 / 3.0);
          else
            arg.setZero();
          const std::vector<int> outcomes((size_t)outcome_count, 1);
          double want = 0.0;
          if (fn == "categorical_lpmf") {
            if (outcome_count == 1)
              want = propto ? stan::math::categorical_lpmf<true>(1, arg)
                            : stan::math::categorical_lpmf<false>(1, arg);
            else
              want = propto
                         ? stan::math::categorical_lpmf<true>(outcomes, arg)
                         : stan::math::categorical_lpmf<false>(outcomes, arg);
          } else if (outcome_count == 1) {
            want = propto ? stan::math::categorical_logit_lpmf<true>(1, arg)
                          : stan::math::categorical_logit_lpmf<false>(1, arg);
          } else {
            want =
                propto
                    ? stan::math::categorical_logit_lpmf<true>(outcomes, arg)
                    : stan::math::categorical_logit_lpmf<false>(outcomes, arg);
          }
          expect_eq(tag + " interpreted value", row.at((size_t)col.slot), want);
        }
        check(found, tag + " interpreted column");

        // The outcome expression is evaluated exactly once. Compare the next
        // draw with a stream on which the same callbacks were invoked once.
        WaRng reference_rng(1234);
        for (int i = 0; i < outcome_count; ++i)
          (void)stan::math::binomial_rng(1, 1.0, reference_rng.gen());
        expect_eq(tag + " interpreted RNG position",
                  stan::math::uniform_rng(0.0, 1.0, rng.gen()),
                  stan::math::uniform_rng(0.0, 1.0, reference_rng.gen()));
      }
    }
  }

  // Propto still validates on the interpreted route. A deterministic RNG
  // keeps the section in WaInterp while producing an out-of-range category.
  for (const std::string& fn : {"categorical_lpmf", "categorical_logit_lpmf"}) {
    std::string mir = categorical_write_array_mir(
        slurp("tests/fixtures/cat.tmir.sexp"), fn, true, false, false, true);
    const size_t gq = mir.find("(generate_quantities");
    const size_t density = mir.find("(FunApp (StanLib " + fn, gq);
    const size_t rng = mir.find("(FunApp (StanLib binomial_rng", density);
    const std::string one = "(pattern (Lit Int 1))";
    const size_t trials = mir.find(one, rng);
    check(gq != std::string::npos && density != std::string::npos &&
              rng != std::string::npos && trials != std::string::npos,
          fn + " invalid interpreted mutation found target");
    if (trials != std::string::npos)
      mir.replace(trials, one.size(), "(pattern (Lit Int 4))");
    DataMap d = DataMap::from_json(R"({"K":3,"y":2,"ys":[3,1,3]})");
    CompiledModel lm = compile_model(mir, d);
    check(lm.write_array && lm.write_array->interp,
          fn + " invalid interpreted write_array selected");
    if (!lm.write_array || !lm.write_array->interp) continue;
    Executor ex(std::move(lm.graph));
    lm.bind(ex);
    std::fill(ex.params_data(), ex.params_data() + ex.n_params(), 0.0);
    ex.run_forward_only();
    WaRng rng_state(1234);
    std::string got_message;
    try {
      (void)lm.write_array->interp->eval(lm.constrained_env(ex), rng_state);
    } catch (const std::domain_error& e) {
      got_message = e.what();
    }
    Eigen::VectorXd arg = Eigen::VectorXd::Constant(3, 1.0 / 3.0);
    std::string want_message;
    try {
      if (fn == "categorical_lpmf")
        (void)stan::math::categorical_lpmf<true>(4, arg);
      else
        (void)stan::math::categorical_logit_lpmf<true>(4, arg);
    } catch (const std::domain_error& e) {
      want_message = e.what();
    }
    check(!got_message.empty(), fn + " interpreted propto validates");
    check(got_message == want_message,
          fn + " interpreted propto exception message");
  }

  // Equal flattened widths do not make a scalar the shape owner. With K=1,
  // scalar-left multiplication must retain the vector's logical geometry in
  // the interpreter before the categorical runtime check.
  for (const std::string& fn : {"categorical_lpmf", "categorical_logit_lpmf"}) {
    std::string mir = categorical_write_array_mir(
        slurp("tests/fixtures/cat.tmir.sexp"), fn, false, false, false, true);
    const size_t gq = mir.find("(generate_quantities");
    const size_t density = mir.find("(FunApp (StanLib " + fn, gq);
    const std::string vector_arg =
        "((pattern (Var theta))\n"
        "             (meta ((type_ UVector) (adlevel DataOnly)))))";
    const size_t arg = mir.find(vector_arg, density);
    check(gq != std::string::npos && density != std::string::npos &&
              arg != std::string::npos,
          fn + " scalar-left vector mutation found target");
    if (arg != std::string::npos) {
      const std::string scaled_arg = R"(((pattern
             (FunApp (StanLib Times__ FnPlain AoS)
              (((pattern (Lit Real 1.0))
                (meta ((type_ UReal) (adlevel DataOnly))))
               ((pattern (Var theta))
                (meta ((type_ UVector) (adlevel DataOnly)))))))
            (meta ((type_ UVector) (adlevel DataOnly))))))";
      mir.replace(arg, vector_arg.size(), scaled_arg);
    }
    DataMap d = DataMap::from_json(R"({"K":1,"y":1,"ys":[1,1,1]})");
    CompiledModel lm = compile_model(mir, d);
    check(lm.write_array && lm.write_array->interp,
          fn + " scalar-left interpreted write_array selected");
    if (!lm.write_array || !lm.write_array->interp) continue;
    Executor ex(std::move(lm.graph));
    lm.bind(ex);
    ex.run_forward_only();
    WaRng rng(1234);
    const std::vector<double> row =
        lm.write_array->interp->eval(lm.constrained_env(ex), rng);
    bool found = false;
    for (const auto& col : lm.write_array->interp->columns()) {
      if (col.name != "categorical_value") continue;
      found = true;
      expect_eq(fn + " scalar-left vector value", row.at((size_t)col.slot),
                0.0);
    }
    check(found, fn + " scalar-left vector column");
  }

  // A shaped zero-width operand also owns the broadcast geometry. The RNG
  // forces WaInterp; Stan's empty-outcome logit overload returns zero without
  // indexing either the outcomes or the empty vector.
  {
    const std::string fn = "categorical_logit_lpmf";
    std::string mir =
        categorical_write_array_mir(slurp("tests/fixtures/cat.tmir.sexp"), fn,
                                    false, false, false, true, 0);
    const size_t gq = mir.find("(generate_quantities");
    const size_t density = mir.find("(FunApp (StanLib " + fn, gq);
    const std::string vector_arg =
        "((pattern (Var theta))\n"
        "             (meta ((type_ UVector) (adlevel DataOnly)))))";
    const size_t arg = mir.find(vector_arg, density);
    check(gq != std::string::npos && density != std::string::npos &&
              arg != std::string::npos,
          "empty scalar-left vector mutation found target");
    if (arg != std::string::npos) {
      const std::string scaled_empty = R"(((pattern
             (FunApp (StanLib Times__ FnPlain AoS)
              (((pattern
                 (FunApp (StanLib normal_rng FnRng AoS)
                  (((pattern (Lit Real 0.0))
                    (meta ((type_ UReal) (adlevel DataOnly))))
                   ((pattern (Lit Real 1.0))
                    (meta ((type_ UReal) (adlevel DataOnly)))))))
                (meta ((type_ UReal) (adlevel DataOnly))))
               ((pattern
                 (FunApp (StanLib head FnPlain AoS)
                  (((pattern (Var theta))
                    (meta ((type_ UVector) (adlevel DataOnly))))
                   ((pattern (Lit Int 0))
                    (meta ((type_ UInt) (adlevel DataOnly)))))))
                (meta ((type_ UVector) (adlevel DataOnly)))))))
            (meta ((type_ UVector) (adlevel DataOnly))))))";
      mir.replace(arg, vector_arg.size(), scaled_empty);
    }
    DataMap d = DataMap::from_json(R"({"K":3,"y":2,"ys":[3,1,3]})");
    CompiledModel lm = compile_model(mir, d);
    check(lm.write_array && lm.write_array->interp,
          "empty scalar-left interpreted write_array selected");
    if (lm.write_array && lm.write_array->interp) {
      Executor ex(std::move(lm.graph));
      lm.bind(ex);
      ex.run_forward_only();
      WaRng rng(1234);
      const std::vector<double> row =
          lm.write_array->interp->eval(lm.constrained_env(ex), rng);
      bool found = false;
      for (const auto& col : lm.write_array->interp->columns()) {
        if (col.name != "categorical_value") continue;
        found = true;
        expect_eq("empty scalar-left vector value", row.at((size_t)col.slot),
                  0.0);
      }
      check(found, "empty scalar-left vector column");
    }
  }

  // rep_matrix on parameters (row-vector across rows, scalar fill) with
  // to_vector flattening.
  {
    DataMap d;
    d.set_int("R", 2);
    CompiledModel lm = compile_model(slurp("tests/fixtures/repm.tmir.sexp"), d);
    check(lm.n_unconstrained == 4, "repm 4 unconstrained");
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    const double q[4] = {0.4, -0.2, 0.9, 0.1};
    for (int i = 0; i < 4; ++i) lex.params_data()[i] = q[i];
    double grad[4];
    const double lp = lex.gradient(grad);

    using stan::math::var;
    Eigen::Matrix<var, -1, 1> a(3);
    a << q[0], q[1], q[2];
    var s = q[3];
    // L[i, j] = a[j]; col-major flat: column j is R copies of a[j].
    Eigen::Matrix<var, -1, 1> Lf(6), Sf(6);
    for (int j = 0; j < 3; ++j)
      for (int i = 0; i < 2; ++i) {
        Lf(j * 2 + i) = a(j);
        Sf(j * 2 + i) = s;
      }
    var t1 = stan::math::normal_lpdf<false>(Lf, 0.0, 1.0);
    var t2 = stan::math::normal_lpdf<false>(Sf, 0.0, 2.0);
    var acc = (t1 + t2);
    acc.grad();
    expect_eq("repm lp", lp, acc.val());
    for (int i = 0; i < 3; ++i)
      expect_eq("repm ga" + std::to_string(i), grad[i], a(i).adj());
    expect_eq("repm gs", grad[3], s.adj());
    stan::math::recover_memory();
  }

  // append_row stacking parameter matrices: col-major storage interleaves
  // columns, so the lowering concatenates then gathers into place.
  {
    DataMap d;
    d.set_int("C", 2);
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/aprow.tmir.sexp"), d);
    check(lm.n_unconstrained == 8, "aprow 8 unconstrained");
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    const double q[8] = {0.5, -0.4, 1.1, 0.2, -0.9, 0.7, 0.3, -0.6};
    for (int i = 0; i < 8; ++i) lex.params_data()[i] = q[i];
    double grad[8];
    const double lp = lex.gradient(grad);

    using stan::math::var;
    // A is 2xC col-major (q0..q3), B is 1xC (q4,q5), r is C (q6,q7).
    Eigen::Matrix<var, -1, 1> p(8);
    for (int i = 0; i < 8; ++i) p(i) = q[i];
    auto A = [&](int i, int j) { return p(j * 2 + i); };  // 2 rows
    auto B = [&](int j) { return p(4 + j); };
    auto r = [&](int j) { return p(6 + j); };
    Eigen::Matrix<var, -1, 1> S(6), T(6);
    for (int j = 0; j < 2; ++j) {
      S(j * 3 + 0) = A(0, j);
      S(j * 3 + 1) = A(1, j);
      S(j * 3 + 2) = B(j);
      T(j * 3 + 0) = r(j);
      T(j * 3 + 1) = A(0, j);
      T(j * 3 + 2) = A(1, j);
    }
    var acc = stan::math::normal_lpdf<false>(S, 0.0, 1.0) +
              stan::math::normal_lpdf<false>(T, 0.0, 2.0);
    acc.grad();
    expect_eq("aprow lp", lp, acc.val());
    for (int i = 0; i < 8; ++i)
      expect_eq("aprow g" + std::to_string(i), grad[i], p(i).adj());
    stan::math::recover_memory();
  }

  // Simplex + dirichlet: gradient vs the var path (simplex_constrain and
  // dirichlet_lpdf composed exactly as the lowering emits them).
  {
    DataMap d;
    d.set_int("K", 3);
    CompiledModel sm = compile_model(slurp("tests/fixtures/simp.tmir.sexp"), d);
    check(sm.n_unconstrained == 2, "simplex K-1 unconstrained");
    Executor sex(std::move(sm.graph));
    sm.bind(sex);
    sex.params_data()[0] = 0.3;
    sex.params_data()[1] = -0.8;
    double sg[2], slp = sex.gradient(sg);

    using stan::math::var;
    Eigen::Matrix<var, -1, 1> y(2);
    y(0) = 0.3;
    y(1) = -0.8;
    var jac = 0.0;
    auto theta = stan::math::simplex_constrain(y, jac);
    Eigen::VectorXd alpha(3);
    for (int i = 0; i < 3; ++i) alpha(i) = 2.0;
    var lp = stan::math::dirichlet_lpdf<true>(theta, alpha) + jac;
    lp.grad();
    expect_eq("simplex lp", slp, lp.val());
    expect_eq("simplex g0", sg[0], y(0).adj());
    expect_eq("simplex g1", sg[1], y(1).adj());
    stan::math::recover_memory();
  }

  // cholesky_factor_corr[K] lowers to K*(K-1)/2 unconstrained parameters.
  // The unsupported-transform error path is covered by the
  // unsupported-function check below.
  {
    CompiledModel cm =
        compile_model(slurp("tests/fixtures/chol.tmir.sexp"), [] {
          DataMap d;
          d.set_int("K", 3);
          return d;
        }());
    check(cm.n_unconstrained == 3, "cholesky_corr unconstrained size is 3");
  }
  // Control flow on a parameter: an `if` and a ternary whose conditions
  // depend on mu and sigma cannot pick an arm at load time, so lowering
  // compiles each into a necessity island. The gradient must be the one
  // the taken arm implies -- the reference below branches on the same
  // values, which is exactly what CmdStan's generated C++ does.
  //
  // Both arms of both constructs are covered: case 0 takes the if's THEN
  // and the ternary's THEN, case 1 takes both ELSEs.
  {
    // An optimization pass may be switched off; this is not one. Without
    // the island the model does not compile at all, so the disable flag
    // must not reach it.
    test_setenv("STANLI_NO_ISLAND", "1", 1);
    DataMap d;
    d.set_real("y", 1.75);
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/paramcond.tmir.sexp"), d);
    int islands = 0;
    for (const Op& op : lm.graph.ops)
      if (op.opcode == OP_ISLAND) ++islands;
    check(islands == 3, "paramcond: one island per parameter condition");

    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    const double pts[2][2] = {{0.3, -0.2}, {-0.4, 0.5}};
    for (int c = 0; c < 2; ++c) {
      lex.params_data()[0] = pts[c][0];
      lex.params_data()[1] = pts[c][1];
      double grad[2] = {0, 0};
      const double lp = lex.gradient(grad);

      using stan::math::var;
      var u_mu = pts[c][0], u_sig = pts[c][1];
      var sigma = stan::math::exp(u_sig);
      var acc = u_sig;  // lower=0 jacobian
      var m = stan::math::value_of(u_mu) > 0 ? u_mu * 2.0 : -u_mu;
      var w = stan::math::value_of(u_mu) > 0 ? 1.0 + sigma : 3.0 * sigma;
      var s = stan::math::value_of(sigma) < 1 ? 1.0 / sigma : sigma;
      // `~` lowers propto, so the reference drops the same constant.
      acc += stan::math::normal_lpdf<true>(1.75, m, s * w);
      // The target-increment branch: propto OFF, as written.
      acc += stan::math::value_of(u_mu) > stan::math::value_of(sigma)
                 ? stan::math::normal_lpdf<false>(1.75, u_mu, 1.0)
                 : stan::math::normal_lpdf<false>(1.75, sigma, 1.0);
      acc.grad();
      const std::string tag = "paramcond" + std::to_string(c);
      // lp sums three target terms; the reference accumulates them in one
      // var chain, so the last bit is an ordering artifact. Gradients are
      // exact.
      expect_ulp(tag + " lp", lp, acc.val());
      expect_eq(tag + " dmu", grad[0], u_mu.adj());
      expect_eq(tag + " dsigma", grad[1], u_sig.adj());
      stan::math::recover_memory();
    }
    test_unsetenv("STANLI_NO_ISLAND");
  }

  // The same region written with `~`: refused, by name, with the fix in
  // the message. Before the check existed this compiled and was wrong in
  // lp by exactly the dropped constant, with a correct gradient.
  {
    DataMap d;
    d.set_real("y", 1.75);
    bool threw = false;
    try {
      compile_model(slurp("tests/fixtures/paramcond_tilde.tmir.sexp"), d);
    } catch (const CompileError& e) {
      const std::string msg = e.what();
      threw = msg.find("`~` inside a parameter-dependent region") !=
                  std::string::npos &&
              msg.find("target +=") != std::string::npos;
    }
    check(threw, "propto ~ in a parameter region refused with the fix");
  }

  // propto through a user-defined density. CmdStan compiles one as a
  // template on propto__ and hands the CALLER's value to the body, so a
  // body written with `_lupdf` normalizes when it was reached through
  // `_lpdf`. Reading the body's own flag made every user density
  // unnormalized: the gradient is unchanged, lp__ is wrong by the
  // constant, and so is any transformed parameter computed that way.
  //
  // Four terms, four rules, and the whole lp in closed form.
  {
    DataMap d;
    CompiledModel cm =
        compile_model(slurp("tests/fixtures/proptoudf.tmir.sexp"), d);
    Executor ex(std::move(cm.graph));
    cm.bind(ex);
    const double mu = 0.375;
    ex.params_data()[0] = mu;
    std::vector<double> grad(1);
    const double lp = ex.gradient(grad.data());
    const double kLog2Pi = std::log(2.0 * std::acos(-1.0));
    const double want =
        // target += f_lpdf(mu | 1.0)   normalized
        (-0.5 * kLog2Pi - 0.5 * (mu - 1.0) * (mu - 1.0))
        // target += g_lpdf(mu | )      normalized through a second UDF
        + (-0.5 * kLog2Pi - 0.5 * (mu - 0.5) * (mu - 0.5))
        // target += f_lupdf(mu | 2.0)  unnormalized, as asked
        + (-0.5 * (mu - 2.0) * (mu - 2.0))
        // mu ~ f(3.0)                  unnormalized by definition
        + (-0.5 * (mu - 3.0) * (mu - 3.0));
    check(std::fabs(lp - want) < 1e-12, "user density propto: lp " +
                                            std::to_string(lp) + " want " +
                                            std::to_string(want));
    // The gradient is the same either way, which is why lp had to be the
    // test: d/dmu of the four quadratics.
    const double want_g = -(mu - 1.0) - (mu - 0.5) - (mu - 2.0) - (mu - 3.0);
    check(std::fabs(grad[0] - want_g) < 1e-12,
          "user density propto: gradient " + std::to_string(grad[0]));
  }

  // Densities the register machine did not used to speak, inside
  // parameter-dependent branches. The region has to compile to the
  // register machine or not at all, so a density missing from its
  // vocabulary was a compile error for a model the runtime otherwise
  // handles everywhere -- `student_t_lpdf` most sharply, because four
  // arguments did not fit an instruction with three operand fields.
  {
    DataMap d;
    d.set_real("y", 1.75);
    CompiledModel dm =
        compile_model(slurp("tests/fixtures/paramcond_density.tmir.sexp"), d);
    Executor dex(std::move(dm.graph));
    dm.bind(dex);
    // Both arms, so every density is evaluated and differentiated.
    const double pts[2][2] = {{0.35, 0.6}, {-0.2, -0.45}};
    for (int c = 0; c < 2; ++c) {
      dex.params_data()[0] = pts[c][0];
      dex.params_data()[1] = pts[c][1];
      double grad[2] = {0, 0};
      const double lp = dex.gradient(grad);

      using stan::math::var;
      var u_nu = pts[c][0], u_mu = pts[c][1];
      var nu = stan::math::exp(u_nu), mu = u_mu;
      var acc = u_nu;  // lower=0 jacobian
      if (stan::math::value_of(mu) > 0) {
        acc += stan::math::chi_square_lpdf<false>(1.75, nu);
        acc += stan::math::student_t_lpdf<false>(1.75, nu, mu, 1.0);
      } else {
        acc += stan::math::gumbel_lpdf<false>(1.75, mu, 1.0);
        acc += stan::math::rayleigh_lpdf<false>(1.75, nu);
      }
      acc.grad();
      const std::string tag = "paramcond_density" + std::to_string(c);
      expect_eq(tag + " dnu", grad[0], u_nu.adj());
      expect_eq(tag + " dmu", grad[1], u_mu.adj());
      expect_ulp(tag + " lp", lp, acc.val());
      stan::math::recover_memory();
    }
  }

  // Truncation, against an independent var-path reference. CI has no
  // CmdStan, so harnesses/fn_sweep.py -- which checks all 72 distribution
  // functions bitwise -- cannot run there; this is what guards the
  // rewrite and the cdf kernels on every push.
  {
    DataMap d;
    d.set_real("y", 1.75);
    CompiledModel tm =
        compile_model(slurp("tests/fixtures/trunc.tmir.sexp"), d);
    Executor tex(std::move(tm.graph));
    tm.bind(tex);
    const double pts[2][2] = {{0.6, -0.3}, {-0.25, 0.4}};
    for (int c = 0; c < 2; ++c) {
      tex.params_data()[0] = pts[c][0];
      tex.params_data()[1] = pts[c][1];
      double grad[2] = {0, 0};
      const double lp = tex.gradient(grad);

      using stan::math::var;
      var u_mu = pts[c][0], u_sig = pts[c][1];
      var mu = u_mu, sigma = stan::math::exp(u_sig);
      var acc = u_sig;  // lower=0 jacobian
      // The propto instantiation CmdStan would pick: y is data, the
      // parameters are var. Getting this wrong shows up as a constant.
      acc += stan::math::normal_lpdf<true>(1.75, mu, sigma);
      acc -= stan::math::log_diff_exp(stan::math::normal_lcdf(10.0, mu, sigma),
                                      stan::math::normal_lcdf(0.0, mu, sigma));
      acc += stan::math::normal_lcdf(1.5, mu, sigma);
      acc += stan::math::normal_lccdf(0.5, mu, sigma);
      acc += stan::math::gamma_lcdf(1.2, 2.0, sigma);
      acc.grad();

      // Gradients are held to the bit; lp is not. The truncation rewrite
      // adds its terms in a different order than this reference does, and
      // than CmdStan's lp_accum__ does -- measured at 1 ULP against
      // CmdStan on this model, with every gradient exact. Reassociating a
      // sum is the one difference allowed to survive here.
      check(grad[0] == u_mu.adj() && grad[1] == u_sig.adj(),
            "trunc: gradients bitwise against the var path");
      const double tol = 8 * 2.220446049250313e-16 * std::abs(acc.val());
      check(std::abs(lp - acc.val()) <= tol, "trunc: lp matches the var path");
    }
  }

  // Vectorized truncation. The fixture above truncates a scalar, which is
  // the shape that kept working while both vectorized forms failed to
  // compile at all: one needs the FnLength compiler-internal, the other a
  // decl stanc3 leaves unsized. Checked against CmdStan at 0 ULP on lp and
  // every gradient when this went in; the var path is what CI can run.
  {
    const std::vector<double> yv = {0.5, 1.75, 2.25};
    DataMap d;
    d.set_int("N", 3);
    d.set_real_array("y", yv);
    CompiledModel tm =
        compile_model(slurp("tests/fixtures/truncvec.tmir.sexp"), d);
    Executor tex(std::move(tm.graph));
    tm.bind(tex);
    // Declaration order: mu, theta[3], sigma.
    const double pts[2][5] = {{0.6, -0.2, 0.35, 0.1, -0.3},
                              {-0.25, 0.4, -0.15, 0.5, 0.2}};
    for (int c = 0; c < 2; ++c) {
      for (int i = 0; i < 5; ++i) tex.params_data()[i] = pts[c][i];
      double grad[5] = {0, 0, 0, 0, 0};
      const double lp = tex.gradient(grad);

      using stan::math::var;
      var u_mu = pts[c][0], u_sig = pts[c][4];
      Eigen::Matrix<var, Eigen::Dynamic, 1> theta(3);
      for (int i = 0; i < 3; ++i) theta(i) = pts[c][1 + i];
      var mu = u_mu, sigma = stan::math::exp(u_sig);
      Eigen::VectorXd y = Eigen::Map<const Eigen::VectorXd>(yv.data(), 3);

      var acc = u_sig;  // lower=0 jacobian
      // Scalar location: one log_diff_exp scaled by the element count.
      acc += stan::math::normal_lpdf<true>(y, mu, sigma);
      acc -= 3.0 *
             stan::math::log_diff_exp(stan::math::normal_lcdf(10.0, mu, sigma),
                                      stan::math::normal_lcdf(0.0, mu, sigma));
      // Container location: one log_diff_exp per element, accumulated.
      acc += stan::math::normal_lpdf<true>(y, theta, 1.0);
      for (int i = 0; i < 3; ++i)
        acc -= stan::math::log_diff_exp(
            stan::math::normal_lcdf(10.0, theta(i), 1.0),
            stan::math::normal_lcdf(0.0, theta(i), 1.0));
      acc.grad();

      bool gok = grad[0] == u_mu.adj() && grad[4] == u_sig.adj();
      for (int i = 0; i < 3; ++i) gok = gok && grad[1 + i] == theta(i).adj();
      check(gok, "truncvec: gradients bitwise against the var path");
      const double tol = 8 * 2.220446049250313e-16 * std::abs(acc.val());
      check(std::abs(lp - acc.val()) <= tol,
            "truncvec: lp matches the var path");
    }
  }

  // A scalar int outcome against vectorized real arguments. See
  // tests/fixtures/intbcast.stan: the outcome rides in idata, which had no
  // way to say "this was one int, not an array of one", so the kernel handed
  // stan-math a size-1 container and every such call threw a size-consistency
  // error against the longer real arguments. The reference here is the call
  // CmdStan would instantiate -- a bare int -- so it checks the broadcast
  // itself, not just that the throw stopped.
  {
    const int n = 3;
    DataMap d;
    d.set_int("n", n);
    CompiledModel bm =
        compile_model(slurp("tests/fixtures/intbcast.tmir.sexp"), d);
    Executor bex(std::move(bm.graph));
    bm.bind(bex);
    // Declaration order: theta[2], lambda.
    const double pts[2][3] = {{0.3, -0.45, 0.7}, {-0.6, 0.15, -1.1}};
    for (int c = 0; c < 2; ++c) {
      for (int i = 0; i < 3; ++i) bex.params_data()[i] = pts[c][i];
      double grad[3] = {0, 0, 0};
      const double lp = bex.gradient(grad);

      using stan::math::var;
      Eigen::Matrix<var, Eigen::Dynamic, 1> th(2);
      th(0) = pts[c][0];
      th(1) = pts[c][1];
      var lam = pts[c][2];
      Eigen::Matrix<var, Eigen::Dynamic, 1> r = stan::math::exp(th);
      Eigen::VectorXd a(2), b(2), cut(3);
      a << 2.0, 3.0;
      b << 1.3, 1.4;
      cut << -1.0, 0.5, 2.0;
      var acc = stan::math::beta_neg_binomial_lpmf<false>(n, r, a, b);
      acc += stan::math::beta_neg_binomial_lcdf(n, r, a, b);
      acc += stan::math::poisson_log_lpmf<false>(n, th);
      // The cutpoints are one whole argument, not lanes: broadcasting the
      // outcome to their length would multiply this term by three.
      acc += stan::math::ordered_logistic_lpmf<false>(2, lam, cut);
      acc.grad();

      // Measured bitwise on arm64/clang at both points, which is what the
      // replication predicts: N copies of the outcome reduce in the same
      // order as one broadcast scalar. The few-ULP window is the headroom
      // the other var-path comparisons in this file carry, for a toolchain
      // that reassociates the shared subexpressions differently.
      const double tol = 8 * 2.220446049250313e-16;
      const double want[3] = {th(0).adj(), th(1).adj(), lam.adj()};
      bool gok = true;
      for (int i = 0; i < 3; ++i)
        gok = gok && std::abs(grad[i] - want[i]) <=
                         tol * std::max(1.0, std::abs(want[i]));
      check(gok, "intbcast: gradients match the scalar-outcome var path");
      check(
          std::abs(lp - acc.val()) <= tol * std::max(1.0, std::abs(acc.val())),
          "intbcast: lp matches the scalar-outcome var path");
    }
  }

  // The cumulative-distribution side of the discrete densities. See
  // tests/fixtures/dcdf.stan: the lpmfs had been listed for a long time
  // and their cdfs had not, so every one of these was an unsupported
  // function. neg_binomial_2_cdf is the one-integer-group shape the
  // generated int cdfs already had; binomial and beta_binomial carry two
  // groups (outcome and trials) and needed the [len, vals...] layout
  // their lpmfs use. The reference is the call CmdStan would instantiate,
  // so the scalar lines check the broadcast rather than just the absence
  // of a throw.
  {
    DataMap d;
    d.set_int("n", 3);
    d.set_int_array("ns", {1, 2, 3});
    d.set_int_array("N", {5, 7, 9});
    CompiledModel bm = compile_model(slurp("tests/fixtures/dcdf.tmir.sexp"), d);
    Executor bex(std::move(bm.graph));
    bm.bind(bex);
    // Both points keep every parameter inside (0, 1), which is a
    // probability for binomial and a positive mean or shape for the other
    // two, so one unconstrained vector serves all three families.
    const double pts[2][3] = {{0.3, 0.45, 0.7}, {0.6, 0.15, 0.9}};
    for (int c = 0; c < 2; ++c) {
      for (int i = 0; i < 3; ++i) bex.params_data()[i] = pts[c][i];
      double grad[3] = {0, 0, 0};
      const double lp = bex.gradient(grad);

      using stan::math::var;
      Eigen::Matrix<var, Eigen::Dynamic, 1> th(3);
      for (int i = 0; i < 3; ++i) th(i) = pts[c][i];
      Eigen::Matrix<var, Eigen::Dynamic, 1> b = stan::math::exp(th);
      // The integer groups reach the kernels as Eigen vectors of int.
      // The real arguments all reach them as the recording scalar, data
      // or not: the cdfs are tier 0, so there is one all-active
      // instantiation rather than one per activity mask, and the
      // reference has to bind the same way to be the same computation.
      Eigen::VectorXi ns(3), NN(3);
      ns << 1, 2, 3;
      NN << 5, 7, 9;
      Eigen::Matrix<var, Eigen::Dynamic, 1> phi(3), bb(3);
      phi << 2.0, 3.0, 4.0;
      bb << 1.3, 1.4, 1.5;
      const int n = 3;
      var acc = stan::math::neg_binomial_2_cdf(ns, b, phi);
      acc += stan::math::binomial_lcdf(ns, NN, th);
      acc += stan::math::binomial_lccdf(n, 9, th);
      acc += stan::math::binomial_cdf(ns, NN, th);
      acc += stan::math::beta_binomial_lcdf(ns, NN, b, bb);
      acc += stan::math::beta_binomial_lccdf(n, 9, b, bb);
      acc += stan::math::beta_binomial_cdf(ns, NN, b, bb);
      acc.grad();

      // Same activity on both sides, so this is bitwise: no tolerance.
      for (int i = 0; i < 3; ++i)
        expect_eq("dcdf g" + std::to_string(i), grad[i], th(i).adj());
      expect_eq("dcdf lp", lp, acc.val());
    }
  }

  // The five distribution functions the recorder cannot evaluate at all.
  // See tests/fixtures/tcdf.stan: these go to the nested var tape in
  // matrix_fns.cpp, which is the tier ordered_probit and wiener already
  // use, so what this checks past the value is the plumbing -- argument
  // counts, the one integer group, and a length-1 slot entering stan-math
  // as a scalar rather than a one-element vector.
  {
    DataMap d;
    d.set_int("n", 3);
    d.set_int_array("ns", {1, 2, 3});
    CompiledModel tm = compile_model(slurp("tests/fixtures/tcdf.tmir.sexp"), d);
    Executor tex(std::move(tm.graph));
    tm.bind(tex);
    // Declaration order: y1[3], k1[3], y2[3], k2[3], y3, k3, m4[3], p4[3],
    // m5, m6[3].
    const int kNP = 24;
    const double pts[2][kNP] = {
        {0.30, 0.45, 0.70,  -0.20, 0.15, 0.35, -0.40, 0.25,
         0.60, 0.10, -0.30, 0.50,  0.20, 0.55, -0.10, 0.40,
         0.05, 0.28, -0.12, 0.34,  0.18, 0.22, -0.18, 0.46},
        {-0.25, 0.60,  -0.15, 0.30, -0.45, 0.20, 0.35, -0.05,
         0.50,  -0.35, 0.15,  0.65, -0.20, 0.45, 0.25, -0.30,
         0.55,  -0.08, 0.42,  0.12, -0.22, 0.38, 0.08, -0.28}};
    for (int c = 0; c < 2; ++c) {
      for (int i = 0; i < kNP; ++i) tex.params_data()[i] = pts[c][i];
      double grad[kNP] = {0};
      const double lp = tex.gradient(grad);

      using stan::math::var;
      using VecV = Eigen::Matrix<var, Eigen::Dynamic, 1>;
      std::vector<var> th((size_t)kNP);
      for (int i = 0; i < kNP; ++i) th[(size_t)i] = pts[c][i];
      const auto seg = [&](int at) {
        VecV v(3);
        for (int i = 0; i < 3; ++i) v(i) = th[(size_t)(at + i)];
        return v;
      };
      // The tail kernels bind every argument as var, data or not, so the
      // reference binds the literals as var too and this stays bitwise.
      VecV mu3(3);
      mu3 << 0.1, 0.2, 0.3;
      Eigen::VectorXi ns(3);
      ns << 1, 2, 3;
      Eigen::VectorXi n1(1);
      n1 << 3;
      var acc = stan::math::von_mises_cdf(seg(0), var(0.1),
                                          VecV(stan::math::exp(seg(3))));
      acc += stan::math::von_mises_lcdf(seg(6), mu3,
                                        VecV(stan::math::exp(seg(9))));
      acc += stan::math::von_mises_lccdf(th[12], var(0.1),
                                         stan::math::exp(th[13]));
      acc += stan::math::neg_binomial_2_lcdf(ns, VecV(stan::math::exp(seg(14))),
                                             VecV(stan::math::exp(seg(17))));
      acc += stan::math::neg_binomial_2_lccdf(n1, stan::math::exp(th[20]),
                                              var(2.0));
      // The scalar outcome replicated to the lane count, which is what the
      // lowering does before the kernel ever sees the integer group.
      Eigen::VectorXi n3(3);
      n3 << 3, 3, 3;
      acc += stan::math::neg_binomial_2_lccdf(
          n3, VecV(stan::math::exp(seg(21))), var(2.0));
      acc.grad();

      // Same activity on both sides and no parameter shared between two
      // densities, so this is bitwise: no tolerance.
      for (int i = 0; i < kNP; ++i)
        expect_eq("tcdf g" + std::to_string(i), grad[i], th[(size_t)i].adj());
      expect_eq("tcdf lp", lp, acc.val());
    }
  }

  // Two-argument log_sum_exp / log_diff_exp on containers. See
  // tests/fixtures/lsepair.stan: Stan vectorizes both elementwise with
  // scalar broadcast, and the lowering used to emit them at width 1, so
  // every container form died at the assignment that consumed the result.
  {
    // mat is column-major: the JSON reader stores the first index fastest.
    const std::vector<double> matv = {0.5, 1.5, 1.25, 0.25, 2.0, 2.5};
    DataMap d;
    d.set_int_array("counts", {1, 2, 3});
    d.set_real_array("mat", matv, {2, 3});
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/lsepair.tmir.sexp"), d);
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    // Declaration order: a[3], b.
    const double pts[2][4] = {{0.4, -0.7, 0.25, 0.6},
                              {-0.3, 0.9, -0.15, -0.45}};
    for (int c = 0; c < 2; ++c) {
      for (int i = 0; i < 4; ++i) lex.params_data()[i] = pts[c][i];
      double grad[4] = {0, 0, 0, 0};
      const double lp = lex.gradient(grad);

      using stan::math::log_diff_exp;
      using stan::math::log_sum_exp;
      using stan::math::var;
      std::vector<var> a = {pts[c][0], pts[c][1], pts[c][2]};
      var b = pts[c][3];
      std::vector<var> hi = {a[0] + 8, a[1] + 8, a[2] + 8};
      // Each `sum` is one reduction over its own result, so the reference
      // reassociates the same way the graph does and can be held bitwise.
      var acc = 0;
      var s = 0;
      for (int i = 0; i < 3; ++i) s += log_diff_exp(hi[i], a[i]);
      acc += s;
      s = 0;
      for (int i = 0; i < 3; ++i) s += log_diff_exp(hi[i], b);
      acc += s;
      s = 0;
      for (int i = 0; i < 3; ++i) s += log_sum_exp(b, hi[i]);
      acc += s;
      s = 0;
      for (int i = 0; i < 3; ++i) s += log_sum_exp(var(i + 1), b);
      acc += s;
      s = 0;
      for (size_t i = 0; i < matv.size(); ++i)
        s += log_diff_exp(matv[i] + 8, b);
      acc += s;
      // `sum(na[1]) + sum(na[2])` is one target term, so the two element
      // sums join each other before they reach the accumulator.
      var n1 = 0, n2 = 0;
      for (int i = 0; i < 3; ++i) n1 += log_sum_exp(b, hi[i]);
      for (int i = 0; i < 3; ++i) n2 += log_sum_exp(b, a[i]);
      acc += n1 + n2;
      acc.grad();

      bool gok = true;
      for (int i = 0; i < 3; ++i) gok = gok && grad[i] == a[i].adj();
      check(gok, "lsepair: element gradients bitwise against the var path");
      // b is the broadcast operand of six of these ops. mixture.cpp sums
      // an op's N broadcast contributions into a local before touching the
      // adjoint, where the scalar var path folds them in one at a time, so
      // this one lands inside the project's 2 ULP budget rather than on it.
      expect_ulp("lsepair b grad", grad[3], b.adj());
      const double tol = 8 * 2.220446049250313e-16 * std::abs(acc.val());
      check(std::abs(lp - acc.val()) <= tol,
            "lsepair: lp matches the var path");
    }
  }

  // The named spellings of the binary operators, plus squared_distance.
  // See tests/fixtures/opalias.stan: every call is made twice, once on
  // data in transformed data and once on parameters in the model block,
  // and the transformed-data results are summed into the target. That is
  // what makes this test cover both halves -- the graph lowering and the
  // MIR interpreter -- rather than only the one the log density walks.
  // Teaching a function to one of them and not the other is silent: a
  // vectorized log_sum_exp taught only to the lowering reported lp
  // 2.6252 where the answer was 2.9302, with no exception raised.
  {
    const std::vector<double> dvv = {0.4, -0.7};
    const std::vector<double> drvv = {0.25, 0.6};
    // Column-major: dm = [[1.5, 0.5], [-0.25, 2.0]].
    const std::vector<double> dmv = {1.5, -0.25, 0.5, 2.0};
    DataMap d;
    d.set_real_array("dv", dvv, {2});
    d.set_real_array("drv", drvv, {2});
    d.set_real_array("dm", dmv, {2, 2});
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/opalias.tmir.sexp"), d);
    Executor lex(std::move(lm.graph));
    lm.bind(lex);

    using stan::math::var;
    using MatV = Eigen::Matrix<var, Eigen::Dynamic, Eigen::Dynamic>;
    using VecV = Eigen::Matrix<var, Eigen::Dynamic, 1>;
    Eigen::VectorXd DV(2), DRVc(2);
    Eigen::RowVectorXd DRV(2);
    Eigen::MatrixXd DM(2, 2);
    DV << dvv[0], dvv[1];
    DRV << drvv[0], drvv[1];
    DRVc << drvv[0], drvv[1];
    DM << dmv[0], dmv[2], dmv[1], dmv[3];

    // Declaration order: p[2], q.
    const double pts[2][3] = {{0.4, -0.7, 0.6}, {-0.3, 0.9, -0.45}};
    for (int c = 0; c < 2; ++c) {
      for (int i = 0; i < 3; ++i) lex.params_data()[i] = pts[c][i];
      double grad[3] = {0, 0, 0};
      const double lp = lex.gradient(grad);

      using stan::math::add;
      using stan::math::divide;
      using stan::math::elt_divide;
      using stan::math::elt_multiply;
      using stan::math::multiply;
      using stan::math::squared_distance;
      using stan::math::subtract;
      using stan::math::sum;
      // stan-math's own answer for both halves. Transformed data is
      // doubles on either side, so it is the same call with the same
      // types the interpreter makes.
      double td = sum(add(DV, 0.5)) + sum(add(DV, DV)) +
                  sum(subtract(DV, 0.25)) + sum(subtract(0.75, DV)) +
                  sum(multiply(DM, DV)) + multiply(DRV, DV) +
                  sum(multiply(DV, DRV)) + sum(multiply(DM, DM)) +
                  sum(elt_multiply(DV, DV)) + sum(elt_multiply(2.0, DV)) +
                  sum(divide(DV, 2.0)) + sum(divide(1.5, DM)) +
                  sum(elt_divide(DV, DV)) + sum(elt_divide(1.0, DV)) +
                  squared_distance(DV, DRV) + squared_distance(dvv[0], dvv[1]);

      VecV P(2);
      P << pts[c][0], pts[c][1];
      var q = pts[c][2];
      var acc = td;
      acc += sum(add(P, q));
      acc += sum(add(P, DV));
      acc += sum(subtract(q, P));
      acc += sum(elt_multiply(P, P));
      acc += sum(elt_multiply(q, P));
      acc += sum(divide(P, q));
      acc += sum(elt_divide(q, P));
      acc += sum(divide(q, DM));
      acc += sum(multiply(DM, P));
      acc += multiply(P.transpose().eval(), P);
      acc += sum(MatV(multiply(P, P.transpose().eval())));
      acc += squared_distance(P, DRVc);
      acc += squared_distance(P, DV);
      acc += squared_distance(q, P(0));
      acc += add(P(0), q) - divide(P(1), q);
      acc.grad();

      const std::string tag = "opalias c" + std::to_string(c);
      expect_ulp(tag + " gp0", grad[0], P(0).adj());
      expect_ulp(tag + " gp1", grad[1], P(1).adj());
      expect_ulp(tag + " gq", grad[2], q.adj());
      const double tol = 8 * 2.220446049250313e-16 * std::abs(acc.val());
      check(std::abs(lp - acc.val()) <= tol, tag + " lp");
    }
  }

  // Two-argument scalar math with an int argument. See
  // tests/fixtures/binint.stan. The int side has no derivative, so the
  // whole gradient belongs to the real side, and the interesting case is
  // the pairing: a matrix is column-major, an int array's trailing extents
  // are row-major, and stan-math pairs n[i][j] with m(i, j).
  {
    const int kk = 2;
    const std::vector<int> counts = {0, 1, 2};
    // {{0, 3}, {1, 2}}: distinct, so a swapped pairing is a factor of two.
    const int expo[2][2] = {{0, 3}, {1, 2}};
    const int expo3[2][2][2] = {{{0, 3}, {1, 2}}, {{2, 0}, {3, 1}}};
    // Through the JSON reader, because expo3 is rank three and the data
    // path is the only thing that stores an array of that rank. `nn` is
    // the same shape as `expo` below but read rather than built from a
    // literal: the interpreter reaches a data variable and a literal by
    // different paths, and only one of them is covered by `expo`.
    DataMap d = DataMap::from_json(R"({"k": )" + std::to_string(kk) + R"(,
            "counts": [0, 1, 2],
            "expo3": [[[0, 3], [1, 2]], [[2, 0], [3, 1]]],
            "nn": [[2, 0], [3, 1]]})");
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/binint.tmir.sexp"), d);
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    // Declaration order: a[3], b.
    const double pts[2][4] = {{0.4, -0.7, 0.25, 0.6},
                              {-0.3, 0.9, -0.15, -0.45}};
    for (int c = 0; c < 2; ++c) {
      for (int i = 0; i < 4; ++i) lex.params_data()[i] = pts[c][i];
      double grad[4] = {0, 0, 0, 0};
      const double lp = lex.gradient(grad);

      using stan::math::var;
      std::vector<var> a = {pts[c][0], pts[c][1], pts[c][2]};
      var b = pts[c][3];
      std::vector<var> p;
      for (int i = 0; i < 3; ++i) p.push_back(stan::math::exp(a[i]) + 1);
      // One reduction per target statement, in statement order, so the
      // reference reassociates the way the graph does.
      var acc = 0, s = 0;
      for (int i = 0; i < 3; ++i) s += stan::math::bessel_first_kind(kk, a[i]);
      acc += s;
      s = 0;
      for (int i = 0; i < 3; ++i)
        s += stan::math::modified_bessel_first_kind(kk, a[i]);
      acc += s;
      s = 0;
      for (int i = 0; i < 3; ++i) s += stan::math::bessel_second_kind(kk, p[i]);
      acc += s;
      s = 0;
      for (int i = 0; i < 3; ++i)
        s += stan::math::modified_bessel_second_kind(kk, p[i]);
      acc += s;
      s = 0;
      for (int i = 0; i < 3; ++i) s += stan::math::lmgamma(kk, p[i]);
      acc += s;
      acc += stan::math::binary_log_loss(1, stan::math::inv_logit(b));
      s = 0;
      for (int i = 0; i < 3; ++i) s += stan::math::falling_factorial(p[i], kk);
      acc += s;
      s = 0;
      for (int i = 0; i < 3; ++i)
        s += stan::math::rising_factorial(p[i], counts[i]);
      acc += s;
      s = 0;
      for (int i = 0; i < 3; ++i) s += stan::math::ldexp(a[i], counts[i]);
      acc += s;
      // m = [[a1, a2], [a3, b]], summed column-major, each entry against
      // the int array entry at the SAME logical position.
      const std::vector<var> mcol = {a[0], a[2], a[1], b};
      const int ecol[4] = {expo[0][0], expo[1][0], expo[0][1], expo[1][1]};
      s = 0;
      for (int i = 0; i < 4; ++i) s += stan::math::ldexp(mcol[i], ecol[i]);
      acc += s;
      // An array of matrices against a deeper int array. The extents that
      // set the pairing are the leaf's, so each element repeats the case
      // above with its own slice of expo3; `sum(am[1]) + sum(am[2])` is one
      // target statement, so the two element sums join before the
      // accumulator sees them.
      var e1 = 0, e2 = 0;
      for (int i = 0; i < 4; ++i) {
        const int lo[4] = {expo3[0][0][0], expo3[0][1][0], expo3[0][0][1],
                           expo3[0][1][1]};
        const int hi[4] = {expo3[1][0][0], expo3[1][1][0], expo3[1][0][1],
                           expo3[1][1][1]};
        e1 += stan::math::ldexp(mcol[i], lo[i]);
        e2 += stan::math::ldexp(mcol[i] + 1, hi[i]);
      }
      acc += e1 + e2;
      s = 0;
      for (int i = 0; i < 3; ++i) s += stan::math::ldexp(b, counts[i]);
      acc += s;
      // Transformed data: the MIR interpreter's own copies of the same
      // shapes. 39 is [[1, 2], [3, 4]] against the literal {{0, 3}, {1, 2}}
      // and 38 the same matrix against the data array {{2, 0}, {3, 1}},
      // both paired n[i][j] with m(i, j). A flat pairing would answer 45
      // and 31.
      acc += stan::math::ldexp(1.5, 0) + stan::math::ldexp(2.5, 1) +
             stan::math::ldexp(3.5, 2) + stan::math::lmgamma(2, 1.75) + 39.0 +
             38.0;
      acc.grad();

      // Measured bitwise at both points: the graph builds the same lanes
      // in the same order the reference tape does.
      bool gok = grad[3] == b.adj();
      for (int i = 0; i < 3; ++i) gok = gok && grad[i] == a[i].adj();
      check(gok, "binint: gradients bitwise against the var path");
      const double tol = 8 * 2.220446049250313e-16 * std::abs(acc.val());
      check(std::abs(lp - acc.val()) <= tol, "binint: lp matches the var path");
      stan::math::recover_memory();
    }
  }

  // The one-argument scalar math library over containers. See
  // tests/fixtures/unaryfns.stan: transformed data runs the MIR interpreter
  // on doubles and the model block runs the graph kernels, so a name wired
  // into one and not the other is caught here rather than as a wrong lp.
  {
    const std::vector<double> matv = {0.5, 1.5, 1.25, 0.25, 2.0, 2.5};
    DataMap d;
    d.set_real_array("mat", matv, {2, 3});
    CompiledModel um =
        compile_model(slurp("tests/fixtures/unaryfns.tmir.sexp"), d);
    Executor uex(std::move(um.graph));
    um.bind(uex);
    // Transformed data: evaluated once by the interpreter, on doubles. Its
    // reference is spelled the way the block is, left to right, each sum
    // ascending from zero, because that is the arithmetic the interpreter
    // performs.
    const double tdx[3] = {0.5, 1.5, 2.5};
    const double tdm[3] = {1.5, 2.5, 3.5};
    double s_gamma = 0, s_tri = 0, s_minus = 0, s_plus = 0;
    for (int i = 0; i < 3; ++i) {
      s_gamma += stan::math::tgamma(tdx[i]);
      s_tri += stan::math::trigamma(tdx[i]);
      s_minus += -tdm[i];
      s_plus += tdm[i];
    }
    const double td_sum =
        s_gamma + s_tri + stan::math::inv_Phi(0.6) +
        stan::math::std_normal_log_qf(-0.5) + stan::math::lambert_w0(0.5) +
        stan::math::lambert_wm1(-0.2) + stan::math::inv_erfc(0.75) +
        stan::math::Phi_approx(0.25) + s_minus + s_plus;

    const double pts[2][3] = {{0.4, -0.7, 0.25}, {-0.3, 0.9, -0.15}};
    for (int c = 0; c < 2; ++c) {
      for (int i = 0; i < 3; ++i) uex.params_data()[i] = pts[c][i];
      double grad[3] = {0, 0, 0};
      const double lp = uex.gradient(grad);

      using stan::math::var;
      std::vector<var> a = {pts[c][0], pts[c][1], pts[c][2]};
      std::vector<var> u, lu, wm, pos;
      for (int i = 0; i < 3; ++i) {
        u.push_back(stan::math::inv_logit(a[i]));
        lu.push_back(stan::math::log(u[i]));
        wm.push_back(-0.2 * u[i]);
        pos.push_back(stan::math::exp(a[i]));
      }
      var acc = 0, s = 0;
#define TERM(expr)                         \
  s = 0;                                   \
  for (int i = 0; i < 3; ++i) s += (expr); \
  acc += s;
      TERM(stan::math::tgamma(pos[i]))
      TERM(stan::math::trigamma(pos[i]))
      TERM(stan::math::lambert_w0(pos[i]))
      TERM(stan::math::lambert_wm1(wm[i]))
      TERM(stan::math::inv_Phi(u[i]))
      TERM(stan::math::std_normal_log_qf(lu[i]))
      TERM(stan::math::inv_erfc(u[i]))
      TERM(stan::math::Phi_approx(a[i]))
      TERM(-a[i])
      TERM(a[i])
#undef TERM
      s = 0;
      for (double m : matv) s += stan::math::Phi_approx(m + a[0]);
      acc += s;
      // `sum(na[1]) + sum(na[2])` is one target term, so the two element
      // sums join each other before they reach the accumulator.
      var n1 = 0, n2 = 0;
      for (int i = 0; i < 3; ++i) n1 += stan::math::tgamma(pos[i]);
      for (int i = 0; i < 3; ++i) n2 += stan::math::tgamma(u[i]);
      acc += n1 + n2;
      acc += td_sum;
      acc.grad();

      // Each element of `a` feeds a dozen terms through `u`, `pos` and
      // `lu`, and the graph folds a slot's contributions in op order where
      // the var chain folds them one vari at a time, so this lands inside
      // the project's 2 ULP budget rather than on it -- measured at 1 ULP
      // on one element of one point and 0 everywhere else. The per-function
      // pullbacks themselves are pinned bitwise in
      // tests/test_mir_unary_fallback.cpp.
      for (int i = 0; i < 3; ++i)
        expect_ulp("unaryfns grad", grad[i], a[i].adj());
      const double tol = 8 * 2.220446049250313e-16 * std::abs(acc.val());
      check(std::abs(lp - acc.val()) <= tol,
            "unaryfns: lp matches the var path");
    }
  }

  // Stan's bound transforms called as functions. See
  // tests/fixtures/boundfn.stan: all twelve names, over the container
  // shapes and both bound widths, inside the `_jacobian` user function the
  // conformance sweep generates. The lowering refused every one of them,
  // and the jacobian direction is the half that can be silently wrong --
  // it has to add the log absolute jacobian determinant in log_prob and
  // nothing at all in write_array.
  {
    // md in slot order (first index fastest), which is also what a 2x2
    // Eigen matrix stores.
    const std::vector<double> mdv = {0.5, 1.5, 1.25, 0.25};
    DataMap d;
    d.set_real_array("md", mdv, {2, 2});
    CompiledModel bm =
        compile_model(slurp("tests/fixtures/boundfn.tmir.sexp"), d);
    Executor bex(std::move(bm.graph));
    bm.bind(bex);
    // Declaration order: a[2], s.
    const double pts[2][3] = {{0.4, -0.7, 1.25}, {-0.3, 0.9, -0.45}};
    for (int c = 0; c < 2; ++c) {
      for (int i = 0; i < 3; ++i) bex.params_data()[i] = pts[c][i];
      double grad[3] = {0, 0, 0};
      const double lp = bex.gradient(grad);

      using stan::math::var;
      using Vec = Eigen::Matrix<var, -1, 1>;
      Vec a(2);
      a << pts[c][0], pts[c][1];
      var s = pts[c][2];
      Vec lo(2), hi(2);
      for (int i = 0; i < 2; ++i) {
        lo(i) = a(i) - 4.0;
        hi(i) = stan::math::exp(a(i)) + 1.0;
      }
      Eigen::Matrix<var, -1, -1> m(2, 2);
      for (int i = 0; i < 4; ++i) m(i) = mdv[i];
      // Every `sum` in the fixture is one ascending reduction over its own
      // result, and OP_SUM_VEC accumulates the same way.
      const auto vsum = [](const auto& v) {
        var t = 0;
        for (int i = 0; i < v.size(); ++i) t += v(i);
        return t;
      };
      // The four jacobian increments land on lp__, which the model adds to
      // the returned contribution.
      var jac = 0;
      var cc = 0;
      cc += vsum(stan::math::lb_constrain(a, s));
      cc += vsum(stan::math::lb_constrain<true>(a, lo, jac));
      cc += vsum(stan::math::lb_free(stan::math::lb_constrain(a, s), s));
      cc += vsum(stan::math::ub_constrain(m, s));
      cc += vsum(stan::math::ub_constrain<true>(a, hi, jac));
      cc += vsum(stan::math::ub_free(stan::math::ub_constrain(a, s), s));
      cc += vsum(stan::math::lub_constrain(a, lo, hi));
      cc += vsum(stan::math::lub_constrain<true>(a, var(-3.0), var(3.0), jac));
      cc += vsum(
          stan::math::lub_free(stan::math::lub_constrain(a, lo, hi), lo, hi));
      cc += vsum(stan::math::offset_multiplier_constrain(a, s, var(2.5)));
      cc += vsum(stan::math::offset_multiplier_constrain(a, s, var(2.5)));
      cc += vsum(stan::math::offset_multiplier_constrain<true>(a, lo, hi, jac));
      cc += vsum(stan::math::offset_multiplier_free(a, lo, hi));
      var acc = cc + jac;
      acc.grad();

      for (int i = 0; i < 2; ++i)
        expect_ulp("boundfn a grad", grad[i], a(i).adj());
      expect_ulp("boundfn s grad", grad[2], s.adj());
      const double tol = 8 * 2.220446049250313e-16 * std::abs(acc.val());
      check(std::abs(lp - acc.val()) <= tol,
            "boundfn: lp matches the var path");
      stan::math::recover_memory();
    }
    // write_array runs the same calls with `jacobian__ = false`, so the
    // `total` column is the returned contribution alone. log_prob adds the
    // four jacobian increments to it, and each of those is nonzero here.
    check(bm.write_array && bm.write_array->truncated.empty(),
          "boundfn write_array compiled");
    if (bm.write_array && bm.write_array->truncated.empty()) {
      Executor wex(std::move(bm.write_array->graph));
      bm.write_array->bind(wex);
      for (int i = 0; i < 3; ++i) wex.params_data()[i] = pts[0][i];
      wex.run_forward_only();
      for (int i = 0; i < 3; ++i) bex.params_data()[i] = pts[0][i];
      const double lp = bex.forward();
      bool found = false;
      for (const auto& col : bm.write_array->columns) {
        if (col.name != "total") continue;
        found = true;
        check(*wex.value_ptr(col.slot) != lp,
              "boundfn: write_array drops the jacobian increments");
      }
      check(found, "boundfn write_array has total");
    }
  }

  // array[N] vector[K] data into a vectorized multivariate density. See
  // tests/fixtures/mnarr.stan: the data path stores this shape the way it
  // stores a matrix, and the kernel wants each element contiguous, so the
  // slot has to be repacked. Wrong is silent, and no corpus model has the
  // shape.
  {
    // Logical y = {{1,2},{3,4},{5,6}}, stored first index fastest, which
    // is what the JSON reader produces for [[1,2],[3,4],[5,6]].
    DataMap d;
    d.set_int("N", 3);
    d.set_int("K", 2);
    d.set_real_array("y", {1, 3, 5, 2, 4, 6}, {3, 2});
    d.set_real_array("Sigma", {2.0, 0.5, 0.5, 1.0}, {2, 2});
    CompiledModel am =
        compile_model(slurp("tests/fixtures/mnarr.tmir.sexp"), d);
    Executor aex(std::move(am.graph));
    am.bind(aex);
    const double pts[2][2] = {{0.4, -0.7}, {-0.3, 0.9}};
    for (int c = 0; c < 2; ++c) {
      aex.params_data()[0] = pts[c][0];
      aex.params_data()[1] = pts[c][1];
      double grad[2] = {0, 0};
      const double lp = aex.gradient(grad);

      using stan::math::var;
      // y and Sigma are data, mu is a parameter: the same instantiation
      // the kernel picks, so this is exact rather than merely close.
      std::vector<Eigen::VectorXd> ys(3, Eigen::VectorXd(2));
      ys[0] << 1, 2;
      ys[1] << 3, 4;
      ys[2] << 5, 6;
      Eigen::MatrixXd Sd(2, 2);
      Sd << 2.0, 0.5, 0.5, 1.0;
      Eigen::Matrix<var, Eigen::Dynamic, 1> mu(2);
      mu(0) = pts[c][0];
      mu(1) = pts[c][1];
      var acc = stan::math::multi_normal_lpdf<true>(ys, mu, Sd);
      acc.grad();

      check(grad[0] == mu(0).adj() && grad[1] == mu(1).adj(),
            "mnarr: gradients bitwise against the var path");
      const double tol = 8 * 2.220446049250313e-16 * std::abs(acc.val());
      check(std::abs(lp - acc.val()) <= tol, "mnarr: lp matches the var path");
    }
  }

  // `p ~ dirichlet(a)` over an array of simplexes. See
  // tests/fixtures/dirvec.stan: the kernel read the whole slot as one
  // theta, so the vectorized form threw on a length mismatch and only the
  // explicit loop worked.
  {
    // Logical p = {{0.3,0.7},{0.4,0.6},{0.2,0.8}}, first index fastest.
    DataMap d;
    d.set_int("N", 3);
    d.set_int("K", 2);
    d.set_real_array("p", {0.3, 0.4, 0.2, 0.7, 0.6, 0.8}, {3, 2});
    CompiledModel dm =
        compile_model(slurp("tests/fixtures/dirvec.tmir.sexp"), d);
    Executor dex(std::move(dm.graph));
    dm.bind(dex);
    const double pts[2][2] = {{0.3, -0.4}, {-0.6, 0.8}};
    for (int c = 0; c < 2; ++c) {
      dex.params_data()[0] = pts[c][0];
      dex.params_data()[1] = pts[c][1];
      double grad[2] = {0, 0};
      const double lp = dex.gradient(grad);

      using stan::math::var;
      var u0 = pts[c][0], u1 = pts[c][1];
      Eigen::Matrix<var, Eigen::Dynamic, 1> alpha(2);
      alpha(0) = stan::math::exp(u0);
      alpha(1) = stan::math::exp(u1);
      std::vector<Eigen::VectorXd> th(3, Eigen::VectorXd(2));
      th[0] << 0.3, 0.7;
      th[1] << 0.4, 0.6;
      th[2] << 0.2, 0.8;
      // lower=0 jacobian on both elements of a, then the vectorized lpdf.
      var acc = u0 + u1;
      acc += stan::math::dirichlet_lpdf<true>(th, alpha);
      acc.grad();

      check(grad[0] == u0.adj() && grad[1] == u1.adj(),
            "dirvec: gradients bitwise against the var path");
      const double tol = 8 * 2.220446049250313e-16 * std::abs(acc.val());
      check(std::abs(lp - acc.val()) <= tol, "dirvec: lp matches the var path");
    }
  }

  // Every shape 0.4.0 got wrong, in one model and one gradient. See
  // tests/fixtures/shapes.stan. The three fixtures above each pin one
  // construct and name the fix that broke; this one shares a graph, a data
  // block and a parameter vector between them, so it is the check that one
  // construct does not disturb another. All nine gradients have to come out
  // for it to pass.
  {
    DataMap d;
    d.set_int("N", 3);
    d.set_int("K", 2);
    d.set_real_array("t", {0.5, 1.75, 2.25});
    // Both 2-D data blocks are stored first index fastest, which is what
    // the JSON reader produces: y = {{1,2},{3,4},{5,6}} and
    // p = {{0.3,0.7},{0.4,0.6},{0.2,0.8}}.
    d.set_real_array("y", {1, 3, 5, 2, 4, 6}, {3, 2});
    d.set_real_array("p", {0.3, 0.4, 0.2, 0.7, 0.6, 0.8}, {3, 2});
    d.set_real_array("Sigma", {2.0, 0.5, 0.5, 1.0}, {2, 2});
    CompiledModel sm =
        compile_model(slurp("tests/fixtures/shapes.tmir.sexp"), d);
    Executor sex(std::move(sm.graph));
    sm.bind(sex);
    // Declaration order: mu, theta[3], sigma, m[2], a[2].
    const double pts[2][9] = {
        {0.35, -0.2, 0.4, 0.15, -0.3, 0.5, -0.45, 0.25, -0.1},
        {-0.4, 0.55, -0.15, 0.3, 0.2, -0.35, 0.6, -0.2, 0.45}};
    for (int c = 0; c < 2; ++c) {
      for (int i = 0; i < 9; ++i) sex.params_data()[i] = pts[c][i];
      double grad[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
      const double lp = sex.gradient(grad);

      using stan::math::var;
      var u_mu = pts[c][0], u_sig = pts[c][4];
      var au0 = pts[c][7], au1 = pts[c][8];
      Eigen::Matrix<var, Eigen::Dynamic, 1> theta(3), m(2), alpha(2);
      for (int i = 0; i < 3; ++i) theta(i) = pts[c][1 + i];
      m(0) = pts[c][5];
      m(1) = pts[c][6];
      var mu = u_mu, sigma = stan::math::exp(u_sig);
      alpha(0) = stan::math::exp(au0);
      alpha(1) = stan::math::exp(au1);

      Eigen::VectorXd t(3);
      t << 0.5, 1.75, 2.25;
      std::vector<Eigen::VectorXd> ys(3, Eigen::VectorXd(2));
      ys[0] << 1, 2;
      ys[1] << 3, 4;
      ys[2] << 5, 6;
      std::vector<Eigen::VectorXd> ps(3, Eigen::VectorXd(2));
      ps[0] << 0.3, 0.7;
      ps[1] << 0.4, 0.6;
      ps[2] << 0.2, 0.8;
      Eigen::MatrixXd Sd(2, 2);
      Sd << 2.0, 0.5, 0.5, 1.0;

      // Jacobians in declaration order, then the four statements in the
      // order the model writes them.
      var acc = u_sig + au0 + au1;
      acc += stan::math::normal_lpdf<true>(t, mu, sigma);
      acc -= 3.0 *
             stan::math::log_diff_exp(stan::math::normal_lcdf(10.0, mu, sigma),
                                      stan::math::normal_lcdf(0.0, mu, sigma));
      acc += stan::math::normal_lpdf<true>(t, theta, 1.0);
      for (int i = 0; i < 3; ++i)
        acc -= stan::math::log_diff_exp(
            stan::math::normal_lcdf(10.0, theta(i), 1.0),
            stan::math::normal_lcdf(0.0, theta(i), 1.0));
      acc += stan::math::multi_normal_lpdf<true>(ys, m, Sd);
      acc += stan::math::dirichlet_lpdf<true>(ps, alpha);
      acc.grad();

      const double want[9] = {u_mu.adj(),     theta(0).adj(), theta(1).adj(),
                              theta(2).adj(), u_sig.adj(),    m(0).adj(),
                              m(1).adj(),     au0.adj(),      au1.adj()};
      // Eight of the nine are bitwise. d/d_mu carries the truncation
      // normalizer, which the graph computes as one scaled log_diff_exp
      // where this reference and CmdStan both accumulate, so it lands
      // inside the 2 ULP budget rather than on it. The failure this test
      // exists for is not subtle: permuting the observations against the
      // components moves a gradient by whole digits.
      for (int i = 0; i < 9; ++i)
        expect_ulp("shapes g" + std::to_string(i), grad[i], want[i]);
      const double tol = 8 * 2.220446049250313e-16 * std::abs(acc.val());
      check(std::abs(lp - acc.val()) <= tol, "shapes: lp matches the var path");
    }
  }

  // Ordinal regression, against an independent var-path reference. Same
  // reason as the truncation case: CI has no CmdStan, and this is the one
  // density whose cutpoint argument is a shared vector rather than a
  // per-lane value, so it is the only check that VecMask and the
  // vector-of-vectors partials edge stay wired.
  {
    DataMap d;
    d.set_int("N", 4);
    d.set_int("K", 4);
    d.set_int_array("y", {1, 3, 2, 4});
    CompiledModel om =
        compile_model(slurp("tests/fixtures/ordlog.tmir.sexp"), d);
    Executor oex(std::move(om.graph));
    om.bind(oex);
    // 4 lambdas + 3 unconstrained cutpoints.
    const double q[7] = {0.3, -0.2, 0.55, -0.4, -0.5, 0.2, 0.1};
    for (int i = 0; i < 7; ++i) oex.params_data()[i] = q[i];
    double grad[7] = {0, 0, 0, 0, 0, 0, 0};
    const double lp = oex.gradient(grad);

    using stan::math::var;
    Eigen::Matrix<var, -1, 1> lambda(4);
    for (int i = 0; i < 4; ++i) lambda(i) = q[i];
    // ordered[3] from the unconstrained triple, with its jacobian.
    Eigen::Matrix<var, -1, 1> c(3);
    var acc = 0;
    c(0) = q[4];
    for (int k = 1; k < 3; ++k) {
      c(k) = c(k - 1) + stan::math::exp(var(q[4 + k]));
      acc += q[4 + k];
    }
    const std::vector<int> y{1, 3, 2, 4};
    acc += stan::math::normal_lpdf<true>(lambda, 0, 2);
    acc += stan::math::ordered_logistic_lpmf<true>(y, lambda, c);
    acc.grad();

    bool grads_ok = true;
    for (int i = 0; i < 4; ++i)
      if (grad[i] != lambda(i).adj()) grads_ok = false;
    check(grads_ok, "ordered_logistic: lambda gradients bitwise");
    check(std::abs(lp - acc.val()) <=
              8 * 2.220446049250313e-16 * std::abs(acc.val()),
          "ordered_logistic: lp matches the var path");
  }

  // The GLM fast paths. fn_sweep cannot reach these -- it generates
  // all-scalar models and a GLM needs a data matrix -- so this is the only
  // check that their propto bit and activity mask arrive.
  {
    // Through the JSON reader, so the data-matrix layout convention is
    // part of what this checks: x[i][j] is row i, column j, and the
    // reference below fills X the same way.
    DataMap d = DataMap::from_json(
        R"({"N": 4, "K": 2,
            "x": [[0.3, -0.2], [1.1, 0.4], [-0.5, 0.9], [0.2, 0.7]],
            "y": [2, 5, 1, 3]})");
    CompiledModel gm =
        compile_model(slurp("tests/fixtures/glmpois.tmir.sexp"), d);
    Executor gex(std::move(gm.graph));
    gm.bind(gex);
    const double q[4] = {0.15, 0.3, -0.25, -0.4};  // alpha, beta[2], log phi
    for (int i = 0; i < 4; ++i) gex.params_data()[i] = q[i];
    double grad[4] = {0, 0, 0, 0};
    const double lp = gex.gradient(grad);

    using stan::math::var;
    var alpha = q[0];
    Eigen::Matrix<var, -1, 1> beta(2);
    beta(0) = q[1];
    beta(1) = q[2];
    var u_phi = q[3], phi = stan::math::exp(u_phi);
    Eigen::MatrixXd X(4, 2);
    X << 0.3, -0.2, 1.1, 0.4, -0.5, 0.9, 0.2, 0.7;
    const std::vector<int> y{2, 5, 1, 3};
    var acc = u_phi;  // lower=0 jacobian on phi
    acc += stan::math::normal_lpdf<true>(alpha, 0, 2);
    acc += stan::math::normal_lpdf<true>(beta, 0, 2);
    acc += stan::math::exponential_lpdf<true>(phi, 1);
    acc += stan::math::poisson_log_glm_lpmf<true>(y, X, alpha, beta);
    acc +=
        stan::math::neg_binomial_2_log_glm_lpmf<true>(y, X, alpha, beta, phi);
    acc.grad();

    const bool g_ok = grad[0] == alpha.adj() && grad[1] == beta(0).adj() &&
                      grad[2] == beta(1).adj() && grad[3] == u_phi.adj();
    check(g_ok, "glm: gradients bitwise against the var path");
    check(std::abs(lp - acc.val()) <=
              8 * 2.220446049250313e-16 * std::abs(acc.val()),
          "glm: lp matches the var path (propto reached the kernel)");
  }

  // A GLM whose outcome is a language-level scalar rather than an array of
  // one value per row. The kernels map `rows` integers out of idata, so a
  // one-element group used to be read past the end and answered with
  // whatever followed it in the vector -- on this fixture -29.48 where the
  // equivalent array outcome gives -16.22, no diagnostic. It is refused at
  // lowering rather than replicated because for poisson_log_glm stan-math's
  // own broadcast is not the replicated call: its <false> form subtracts
  // lgamma(y+1) once for a scalar and once per row for an array, so
  // replicating would put stanli's lp a constant off CmdStan's. See
  // docs/coverage.md.
  {
    DataMap d = DataMap::from_json(
        R"({"N": 4, "K": 2,
            "x": [[0.3, -0.2], [1.1, 0.4], [-0.5, 0.9], [0.2, 0.7]],
            "y": 3})");
    std::string msg;
    try {
      compile_model(slurp("tests/fixtures/glmscalary.tmir.sexp"), d);
    } catch (const CompileError& e) {
      msg = e.what();
    }
    check(msg.find("poisson_log_glm_lpmf") != std::string::npos &&
              msg.find("4 rows") != std::string::npos,
          "glm scalar outcome refused by name, not read past the end");
  }

  // normal_id_glm with the OUTCOME as a parameter. stanc3's --O1 partial
  // evaluator rewrites `theta ~ normal(x * b, 1)` into
  // normal_id_glm_lupdf(theta | x, 0, b, 1), a shape the source language
  // never hands the kernel directly: y is the thing being sampled, so its
  // gradient must come back too, not only alpha/beta/sigma's.
  {
    DataMap d = DataMap::from_json(
        R"({"N": 3, "K": 2,
            "x": [[1.0, 2.0], [0.5, -1.0], [2.0, 0.25]]})");
    CompiledModel gm = compile_model(slurp("tests/fixtures/glmy.tmir.sexp"), d);
    Executor gex(std::move(gm.graph));
    gm.bind(gex);
    const double q[5] = {0.3, -0.2, 1.1, 0.7, -0.4};  // theta[3], b[2]
    for (int i = 0; i < 5; ++i) gex.params_data()[i] = q[i];
    double grad[5] = {0, 0, 0, 0, 0};
    const double lp = gex.gradient(grad);

    using stan::math::var;
    Eigen::Matrix<var, -1, 1> theta(3), b(2);
    for (int i = 0; i < 3; ++i) theta(i) = q[i];
    for (int i = 0; i < 2; ++i) b(i) = q[3 + i];
    Eigen::MatrixXd X(3, 2);
    X << 1.0, 2.0, 0.5, -1.0, 2.0, 0.25;
    var acc = stan::math::normal_id_glm_lpdf<true>(theta, X, 0, b, 1);
    acc.grad();

    bool g_ok = true;
    for (int i = 0; i < 3; ++i)
      if (grad[i] != theta(i).adj()) g_ok = false;
    for (int i = 0; i < 2; ++i)
      if (grad[3 + i] != b(i).adj()) g_ok = false;
    check(g_ok, "glm param y: gradients bitwise against the var path");
    check(lp == acc.val(), "glm param y: lp matches the var path");
  }

  // An inlined user function's return variable: --O1 declares it
  // zero-length (`vector[0] inline_..._return_sym__`) and sizes it by
  // assignment, so the lowering adopts the assigned shape instead of
  // rejecting the width mismatch.
  {
    DataMap d = DataMap::from_json(R"({"N": 4, "y": [1.0, 2.0, 4.0, -0.5]})");
    CompiledModel im =
        compile_model(slurp("tests/fixtures/inlret.tmir.sexp"), d);
    Executor iex(std::move(im.graph));
    im.bind(iex);
    iex.params_data()[0] = 0.4;  // mu
    double grad[1] = {0};
    const double lp = iex.gradient(grad);

    using stan::math::var;
    var mu = 0.4;
    Eigen::VectorXd y(4);
    y << 1.0, 2.0, 4.0, -0.5;
    Eigen::VectorXd c = y.array() - y.mean();
    var acc = stan::math::normal_lpdf<false>(c, mu, 1);
    acc.grad();
    const double tol = 64 * 2.220446049250313e-16;
    check(std::abs(lp - acc.val()) <= tol * std::abs(acc.val()),
          "inlined return: lp matches the var path");
    check(
        std::abs(grad[0] - mu.adj()) <= tol * std::max(1.0, std::abs(mu.adj())),
        "inlined return: gradient matches the var path");
  }

  // A declaration sized by a shape query on a COMPUTED value:
  // `vector[rows(segment(beta, 1, 2)) + 1]` after --O1 inlines a callee
  // and substitutes the call argument into its size expressions.
  {
    DataMap d = DataMap::from_json(R"({"N": 3})");
    CompiledModel sm =
        compile_model(slurp("tests/fixtures/inlseg.tmir.sexp"), d);
    Executor sex(std::move(sm.graph));
    sm.bind(sex);
    const double q[3] = {0.6, -0.3, 0.2};
    for (int i = 0; i < 3; ++i) sex.params_data()[i] = q[i];
    double grad[3] = {0, 0, 0};
    const double lp = sex.gradient(grad);

    using stan::math::var;
    Eigen::Matrix<var, -1, 1> beta(3);
    for (int i = 0; i < 3; ++i) beta(i) = q[i];
    Eigen::Matrix<var, -1, 1> u(3);
    u << 0.0, beta(0), beta(1);
    var acc = stan::math::sum(stan::math::cumulative_sum(u));
    acc += stan::math::normal_lpdf<true>(beta, 0, 1);
    acc.grad();
    const double tol = 64 * 2.220446049250313e-16;
    check(std::abs(lp - acc.val()) <= tol * std::max(1.0, std::abs(acc.val())),
          "shape query on expression: lp matches the var path");
    bool g_ok = true;
    for (int i = 0; i < 3; ++i)
      if (std::abs(grad[i] - beta(i).adj()) >
          tol * std::max(1.0, std::abs(beta(i).adj())))
        g_ok = false;
    check(g_ok, "shape query on expression: gradients match the var path");
  }

  // Vector fma from --O1 partial evaluation (`k .* t + c` becomes
  // fma(k, t, c)): OP_FMA is FUSED, so the reference is stan::math::fma,
  // and the data avoids powers of two -- with t = 2.0/-1.0/0.5 the
  // products are exact and fused equals unfused bitwise, which once let
  // both implementations pass this test.
  {
    DataMap d = DataMap::from_json(R"({"N": 3, "t": [1.7, -0.3, 0.9]})");
    CompiledModel fm =
        compile_model(slurp("tests/fixtures/vecfma.tmir.sexp"), d);
    Executor fex(std::move(fm.graph));
    fm.bind(fex);
    const double q[6] = {0.3, -0.2, 1.1, 0.7, -0.4, 0.9};  // k[3], c[3]
    for (int i = 0; i < 6; ++i) fex.params_data()[i] = q[i];
    double grad[6] = {0, 0, 0, 0, 0, 0};
    const double lp = fex.gradient(grad);

    using stan::math::var;
    Eigen::Matrix<var, -1, 1> k(3), c(3);
    for (int i = 0; i < 3; ++i) k(i) = q[i];
    for (int i = 0; i < 3; ++i) c(i) = q[3 + i];
    Eigen::VectorXd t(3);
    t << 1.7, -0.3, 0.9;
    Eigen::Matrix<var, -1, 1> mu(3);
    for (int i = 0; i < 3; ++i) mu(i) = stan::math::fma(k(i), t(i), c(i));
    var acc = stan::math::normal_lpdf<false>(mu, 0, 1);
    acc.grad();
    check(lp == acc.val(), "vector fma: lp bitwise against the var path");
    bool g_ok = true;
    for (int i = 0; i < 3; ++i)
      if (grad[i] != k(i).adj() || grad[3 + i] != c(i).adj()) g_ok = false;
    check(g_ok, "vector fma: gradients bitwise against the var path");
  }

  // Where the CSV's three sections meet. stanc marks the boundaries with
  // early-return guards on emit_transformed_parameters__ /
  // emit_generated_quantities__; lowering pins both flags on, so the
  // recorded indices are the only thing left that says which column is a
  // constrained parameter and which a generated quantity.
  {
    // conj's header, verified against CmdStan in tests/test_sampling.cpp,
    // is mu_c,sigma,prec,mu,sd_from_prec,resid.<i>: two constrained
    // parameters, one transformed parameter, three generated quantities.
    DataMap d = DataMap::from_json_file("tests/fixtures/conj.json");
    CompiledModel wm = compile_model(slurp("tests/fixtures/conj.tmir.sexp"), d);
    check(wm.write_array.has_value(), "conj: write_array compiled");
    if (wm.write_array) {
      expect_eq("conj columns", (double)wm.write_array->columns.size(), 6);
      expect_eq("conj n_tp_start", (double)wm.write_array->n_tp_start, 2);
      expect_eq("conj n_gq_start", (double)wm.write_array->n_gq_start, 3);
    }
  }
  {
    // Parameters only: every column is a constrained parameter, so both
    // boundaries sit at the end.
    DataMap d;
    d.set_int("K", 3);
    CompiledModel wm = compile_model(slurp("tests/fixtures/simp.tmir.sexp"), d);
    check(wm.write_array.has_value(), "simp: write_array compiled");
    if (wm.write_array) {
      expect_eq("simp columns", (double)wm.write_array->columns.size(), 1);
      expect_eq("simp n_tp_start", (double)wm.write_array->n_tp_start, 1);
      expect_eq("simp n_gq_start", (double)wm.write_array->n_gq_start, 1);
    }

    // The unconstrained layout: simplex[3] is two free values behind three
    // constrained ones, and its metadata carries that exact naming shape.
    check(wm.unc_params.size() == 1, "simp: one unconstrained parameter");
    if (wm.unc_params.size() == 1) {
      const auto& u = wm.unc_params[0];
      check(u.name == "theta", "simp unc name");
      expect_eq("simp unc len", (double)u.len, 2);
      check(u.dims == std::vector<int64_t>{2}, "simp unc dims");
      check(u.transform == mir::Transform::Simplex, "simp unc transform");
    }
  }
  {
    // One of every transform: the unconstrained lengths are what the
    // reader has to slice the draw with, and only the declared dims tell
    // corr_matrix[3] from cov_matrix[3].
    DataMap d = DataMap::from_json_file("tests/fixtures/newtrans.json");
    CompiledModel nm =
        compile_model(slurp("tests/fixtures/newtrans.tmir.sexp"), d);
    check(nm.unc_params.size() == nm.param_names.size(),
          "newtrans: one unc_param per declared parameter");
    int64_t total = 0;
    bool names_ok = nm.unc_params.size() == nm.param_names.size();
    for (size_t i = 0; i < nm.unc_params.size(); ++i) {
      total += nm.unc_params[i].len;
      if (i < nm.param_names.size() &&
          nm.unc_params[i].name != nm.param_names[i])
        names_ok = false;
    }
    check(names_ok, "newtrans: unc_params agree with param_names");
    expect_eq("newtrans unc total", (double)total, (double)nm.n_unconstrained);
    struct Want {
      const char* name;
      int64_t len;
      std::vector<int64_t> dims;
      mir::Transform::Kind kind;
    };
    const std::vector<Want> want{
        {"a", 1, {}, mir::Transform::OffsetMultiplier},
        {"b", 3, {3}, mir::Transform::OffsetMultiplier},
        {"c", 1, {}, mir::Transform::Offset},
        {"d", 1, {}, mir::Transform::Multiplier},
        {"mu_p", 3, {3}, mir::Transform::Identity},
        {"sg_p", 3, {3}, mir::Transform::Lower},
        {"e", 3, {3}, mir::Transform::OffsetMultiplier},
        {"u", 3, {3}, mir::Transform::UnitVector},
        {"z", 3, {3}, mir::Transform::SumToZero},
        {"R", 3, {3}, mir::Transform::Correlation},
        {"S", 6, {6}, mir::Transform::Covariance},
        {"Lc", 6, {6}, mir::Transform::CholeskyCov},
        {"Lr", 9, {9}, mir::Transform::CholeskyCov}};
    check(nm.unc_params.size() == want.size(), "newtrans: 13 parameters");
    for (size_t i = 0; i < want.size() && i < nm.unc_params.size(); ++i) {
      const auto& u = nm.unc_params[i];
      const std::string tag = std::string("newtrans ") + want[i].name;
      check(u.name == want[i].name, tag + " name");
      expect_eq(tag + " len", (double)u.len, (double)want[i].len);
      check(u.dims == want[i].dims, tag + " dims");
      check(u.transform == want[i].kind, tag + " transform");
    }
  }

  // stanc's generated constructor executes data constraints before it
  // constructs the model. These are construction errors, not bad draws that
  // may survive until a density or transform happens to notice them.
  {
    const std::string mir = slurp("tests/fixtures/newtrans.tmir.sexp");
    auto rejected_s = [&](double s) {
      DataMap d;
      d.set_real("m", 0.3);
      d.set_real("s", s);
      try {
        (void)compile_model(mir, d);
      } catch (const std::domain_error& e) {
        return std::string(e.what()).find("s") != std::string::npos;
      }
      return false;
    };
    check(rejected_s(-1.0), "data lower bound rejects before construction");
    check(rejected_s(std::numeric_limits<double>::quiet_NaN()),
          "data lower bound rejects NaN");

    DataMap at_bound;
    at_bound.set_real("m", 0.3);
    at_bound.set_real("s", 0.5);
    bool accepted = true;
    try {
      (void)compile_model(mir, at_bound);
    } catch (const std::exception&) {
      accepted = false;
    }
    check(accepted, "data lower bound is inclusive");
  }
  {
    // An upper check over an array must inspect every element. The fourth
    // value is outside 1:K; flattening or checking only the first lane accepts
    // data generated Stan rejects in its constructor.
    DataMap d;
    d.set_int("K", 3);
    d.set_int("y", 2);
    d.set_int_array("ys", {1, 2, 4});
    bool rejected = false;
    try {
      (void)compile_model(slurp("tests/fixtures/cat.tmir.sexp"), d);
    } catch (const std::domain_error& e) {
      rejected = std::string(e.what()).find("ys") != std::string::npos;
    }
    check(rejected, "container data upper bound rejects a later element");
  }

  {
    // Generated Stan checks constrained transformed parameters while it
    // evaluates a draw, not while it constructs the model. The data check
    // above and this runtime check share FnCheck in MIR but must retain their
    // distinct execution phases.
    DataMap d = bound_check_data();
    CompiledModel checked =
        compile_model(slurp("tests/fixtures/data_and_tp_checks.tmir.sexp"), d);
    Executor cex(std::move(checked.graph));
    checked.bind(cex);
    cex.params_data()[0] = -1.0;
    double grad = 0.0;
    bool rejected = false;
    try {
      (void)cex.gradient(&grad);
    } catch (const std::domain_error& e) {
      rejected = std::string(e.what()).find("z") != std::string::npos;
    }
    check(rejected, "transformed-parameter bound rejects the draw");

    cex.params_data()[0] = 0.0;
    bool accepted = true;
    double lp = 0.0;
    try {
      lp = cex.gradient(&grad);
    } catch (const std::exception&) {
      accepted = false;
    }
    check(accepted && lp == 0.0 && grad == 3.0,
          "transformed-parameter bound is inclusive");
  }
  {
    // Parameter-free does not mean construction-time: a transformed
    // parameter derived only from unconstrained data is still checked at the
    // generated log_prob/write_array statement where it was declared.
    DataMap d = bound_check_data(-1.0);
    bool constructed = true;
    bool rejected = false;
    try {
      CompiledModel checked = compile_model(
          slurp("tests/fixtures/data_and_tp_checks.tmir.sexp"), d);
      Executor cex(std::move(checked.graph));
      checked.bind(cex);
      cex.params_data()[0] = 0.0;
      double grad = 0.0;
      try {
        (void)cex.gradient(&grad);
      } catch (const std::domain_error& e) {
        rejected = std::string(e.what()).find("from_data") != std::string::npos;
      }
    } catch (const std::exception&) {
      constructed = false;
    }
    check(constructed && rejected,
          "data-derived transformed bound rejects only at evaluation");
  }
  {
    // Runtime-sized container bounds fail in the generated statement, not
    // while the graph is being compiled. Equal flat widths are likewise not
    // enough to make matrix orientations compatible.
    auto rejects_at_evaluation = [&](DataMap d, double x,
                                     const std::string& name) {
      bool constructed = true;
      bool rejected = false;
      try {
        CompiledModel checked = compile_model(
            slurp("tests/fixtures/data_and_tp_checks.tmir.sexp"), d);
        Executor cex(std::move(checked.graph));
        checked.bind(cex);
        cex.params_data()[0] = x;
        double grad = 0.0;
        try {
          (void)cex.gradient(&grad);
        } catch (const std::invalid_argument& e) {
          rejected = std::string(e.what()).find(name) != std::string::npos;
        }
      } catch (const std::exception&) {
        constructed = false;
      }
      return constructed && rejected;
    };
    check(rejects_at_evaluation(bound_check_data(0.0, 1, 2), 0.0, "bounded"),
          "vector-bound shape mismatch is a runtime error");
    check(rejects_at_evaluation(bound_check_data(0.0, 1, 2), -1.0, "bounded"),
          "dimension check precedes later scalar bound checks");
    check(rejects_at_evaluation(bound_check_data(0.0, 1, 1, 1, 2, 2, 1), 0.0,
                                "bounded_matrix"),
          "equal-width matrix-bound mismatch is a runtime error");
  }

  {
    // Stan Math's hypergeometric implementation performs support checks
    // before dropping an all-data propto term. Without a native density
    // implementation, accepting the model as a constant zero would therefore
    // be exact only for valid data and silently accept invalid data. The
    // honest boundary is to refuse both.
    const std::string hypergeometric_mir = R"(
((functions_block ())
 (input_vars
  ((n <opaque> SInt) (N <opaque> SInt) (a <opaque> SInt)
   (b <opaque> SInt)))
 (prepare_data ())
 (log_prob
  (((pattern
     (TargetPE
      ((pattern
        (FunApp (StanLib hypergeometric_lpmf (FnLpmf true) AoS)
         (((pattern (Var n))
           (meta ((type_ UInt) (adlevel DataOnly))))
          ((pattern (Var N))
           (meta ((type_ UInt) (adlevel DataOnly))))
          ((pattern (Var a))
           (meta ((type_ UInt) (adlevel DataOnly))))
          ((pattern (Var b))
           (meta ((type_ UInt) (adlevel DataOnly)))))))
       (meta ((type_ UReal) (adlevel DataOnly))))))
    (meta <opaque>)))))
)";

    check(stan::math::hypergeometric_lpmf<true>(1, 2, 3, 4) == 0.0,
          "all-data propto oracle drops a valid density");
    bool oracle_rejected = false;
    try {
      (void)stan::math::hypergeometric_lpmf<true>(4, 4, 3, 3);
    } catch (const std::domain_error&) {
      oracle_rejected = true;
    }
    check(oracle_rejected,
          "all-data propto oracle still checks invalid support");

    auto lowering_refuses = [&](int n, int N, int a, int b) {
      DataMap d;
      d.set_int("n", n);
      d.set_int("N", N);
      d.set_int("a", a);
      d.set_int("b", b);
      try {
        (void)compile_model(hypergeometric_mir, d);
      } catch (const CompileError& e) {
        return std::string(e.what()).find("hypergeometric_lpmf") !=
               std::string::npos;
      }
      return false;
    };
    check(lowering_refuses(1, 2, 3, 4),
          "unsupported valid all-data propto density is refused");
    check(lowering_refuses(4, 4, 3, 3),
          "unsupported invalid all-data propto density is refused");
  }

  {
    // An all-data propto categorical term returns zero, but only after Stan
    // Math validates the outcome and probability/logit vector. Compare the
    // whole observable contract, including which competing error wins and
    // the scalar-vs-array overload selected for a length-one outcome.
    struct Outcome {
      std::string kind;
      std::string message;
      double value = 0.0;
    };
    auto observe = [](auto&& call) {
      Outcome out;
      try {
        out.value = call();
        out.kind = "value";
      } catch (const std::domain_error& e) {
        out.kind = "domain_error";
        out.message = e.what();
      } catch (const std::invalid_argument& e) {
        out.kind = "invalid_argument";
        out.message = e.what();
      } catch (const std::exception& e) {
        out.kind = "other";
        out.message = e.what();
      }
      return out;
    };
    auto oracle = [&](const std::string& fn, bool scalar,
                      const std::vector<int>& outcome,
                      const std::vector<double>& arg, bool propto) {
      return observe([&] {
        Eigen::Map<const Eigen::VectorXd> v(arg.data(),
                                            (Eigen::Index)arg.size());
        if (fn == "categorical_lpmf") {
          if (propto)
            return scalar ? stan::math::categorical_lpmf<true>(outcome.at(0), v)
                          : stan::math::categorical_lpmf<true>(outcome, v);
          return scalar ? stan::math::categorical_lpmf<false>(outcome.at(0), v)
                        : stan::math::categorical_lpmf<false>(outcome, v);
        }
        if (propto)
          return scalar ? stan::math::categorical_logit_lpmf<true>(
                              outcome.at(0), v)
                        : stan::math::categorical_logit_lpmf<true>(outcome, v);
        return scalar
                   ? stan::math::categorical_logit_lpmf<false>(outcome.at(0), v)
                   : stan::math::categorical_logit_lpmf<false>(outcome, v);
      });
    };
    auto lowered = [&](const std::string& fn, bool scalar,
                       const std::vector<int>& outcome,
                       const std::vector<double>& arg, bool propto) {
      return observe([&] {
        DataMap d;
        if (scalar)
          d.set_int("outcome", outcome.at(0));
        else
          d.set_int_array("outcome", outcome);
        d.set_real_array("arg", arg);
        CompiledModel model =
            compile_model(categorical_check_mir(fn, scalar, (int)outcome.size(),
                                                (int)arg.size(), propto),
                          d);
        Executor ex(std::move(model.graph));
        model.bind(ex);
        return ex.forward();
      });
    };
    auto same_as_stan = [&](const std::string& label, const std::string& fn,
                            bool scalar, std::vector<int> outcome,
                            std::vector<double> arg, bool propto = true) {
      const Outcome want = oracle(fn, scalar, outcome, arg, propto);
      const Outcome got = lowered(fn, scalar, outcome, arg, propto);
      check(got.kind == want.kind, label + " exception class");
      check(got.message == want.message, label + " exception message");
      if (want.kind == "value")
        expect_eq(label + " value", got.value, want.value);
    };
    auto local_autodiff_oracle = [&](const std::string& fn, bool scalar,
                                     const std::vector<int>& outcome,
                                     const std::vector<double>& arg) {
      return observe([&] {
        Eigen::Matrix<stan::math::var, -1, 1> v((Eigen::Index)arg.size());
        for (size_t i = 0; i < arg.size(); ++i) v((Eigen::Index)i) = arg[i];
        try {
          stan::math::var lp =
              fn == "categorical_lpmf"
                  ? (scalar
                         ? stan::math::categorical_lpmf<true>(outcome.at(0), v)
                         : stan::math::categorical_lpmf<true>(outcome, v))
                  : (scalar ? stan::math::categorical_logit_lpmf<true>(
                                  outcome.at(0), v)
                            : stan::math::categorical_logit_lpmf<true>(outcome,
                                                                       v));
          const double value = lp.val();
          stan::math::recover_memory();
          return value;
        } catch (...) {
          stan::math::recover_memory();
          throw;
        }
      });
    };
    auto local_autodiff_lowered = [&](const std::string& fn, bool scalar,
                                      const std::vector<int>& outcome,
                                      const std::vector<double>& arg) {
      return observe([&] {
        DataMap d;
        if (scalar)
          d.set_int("outcome", outcome.at(0));
        else
          d.set_int_array("outcome", outcome);
        d.set_real_array("arg", arg);
        CompiledModel model =
            compile_model(categorical_check_mir(fn, scalar, (int)outcome.size(),
                                                (int)arg.size(), true, true),
                          d);
        Executor ex(std::move(model.graph));
        model.bind(ex);
        ex.params_data()[0] = 0.0;
        return ex.forward();
      });
    };
    auto same_local_autodiff_as_stan =
        [&](const std::string& label, const std::string& fn, bool scalar,
            std::vector<int> outcome, std::vector<double> arg) {
          const Outcome want = local_autodiff_oracle(fn, scalar, outcome, arg);
          const Outcome got = local_autodiff_lowered(fn, scalar, outcome, arg);
          check(got.kind == want.kind, label + " exception class");
          check(got.message == want.message, label + " exception message");
          if (want.kind == "value")
            expect_eq(label + " value", got.value, want.value);
        };

    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    same_as_stan("categorical scalar valid", "categorical_lpmf", true, {2},
                 {0.2, 0.3, 0.5});
    same_as_stan("categorical zero probability", "categorical_lpmf", true, {2},
                 {1.0, 0.0, 0.0});
    same_as_stan("categorical scalar outcome", "categorical_lpmf", true, {4},
                 {0.2, 0.3, 0.5});
    same_as_stan("categorical outcome before simplex", "categorical_lpmf", true,
                 {4}, {0.2, 0.2, 0.2});
    same_as_stan("categorical later array outcome", "categorical_lpmf", false,
                 {1, 4, 2}, {0.2, 0.3, 0.5});
    same_as_stan("categorical length-one array", "categorical_lpmf", false, {4},
                 {0.2, 0.3, 0.5});
    same_as_stan("categorical empty outcome", "categorical_lpmf", false, {},
                 {0.2, 0.3, 0.5});
    same_as_stan("categorical empty still checks simplex", "categorical_lpmf",
                 false, {}, {0.2, 0.2, 0.2});
    same_as_stan("categorical scalar empty probabilities", "categorical_lpmf",
                 true, {1}, {});
    same_as_stan("categorical empty probabilities", "categorical_lpmf", false,
                 {}, {});
    same_as_stan("categorical normalized valid", "categorical_lpmf", false,
                 {1, 3}, {0.2, 0.3, 0.5}, false);
    same_as_stan("categorical normalized bad simplex", "categorical_lpmf", true,
                 {1}, {0.2, 0.2, 0.2}, false);
    same_as_stan("categorical normalized invalid outcome", "categorical_lpmf",
                 false, {1, 4}, {0.2, 0.3, 0.5}, false);
    same_as_stan("categorical normalized empty outcome", "categorical_lpmf",
                 false, {}, {0.2, 0.3, 0.5}, false);

    same_as_stan("categorical logit scalar valid", "categorical_logit_lpmf",
                 true, {2}, {-1.0, 0.0, 1.0});
    same_as_stan("categorical logit scalar outcome", "categorical_logit_lpmf",
                 true, {4}, {-1.0, 0.0, 1.0});
    same_as_stan("categorical logit outcome before finite",
                 "categorical_logit_lpmf", true, {4}, {0.0, nan, 1.0});
    same_as_stan("categorical logit later array outcome",
                 "categorical_logit_lpmf", false, {1, 4, 2}, {-1.0, 0.0, 1.0});
    same_as_stan("categorical logit length-one array", "categorical_logit_lpmf",
                 false, {4}, {-1.0, 0.0, 1.0});
    same_as_stan("categorical logit empty checks NaN", "categorical_logit_lpmf",
                 false, {}, {0.0, nan, 1.0});
    same_as_stan("categorical logit checks infinity", "categorical_logit_lpmf",
                 true, {1}, {0.0, inf, 1.0});
    same_as_stan("categorical logit empty vectors", "categorical_logit_lpmf",
                 false, {}, {});
    same_as_stan("categorical logit scalar empty vector",
                 "categorical_logit_lpmf", true, {1}, {});
    same_as_stan("categorical logit normalized valid", "categorical_logit_lpmf",
                 false, {1, 3}, {-1.0, 0.0, 1.0}, false);
    same_as_stan("categorical logit normalized infinite",
                 "categorical_logit_lpmf", true, {1}, {0.0, inf, 1.0}, false);
    same_as_stan("categorical logit normalized invalid outcome",
                 "categorical_logit_lpmf", false, {1, 4}, {-1.0, 0.0, 1.0},
                 false);
    same_as_stan("categorical logit normalized empty outcome",
                 "categorical_logit_lpmf", false, {}, {-1.0, 0.0, 1.0}, false);
    same_as_stan("categorical logit normalized empty vectors",
                 "categorical_logit_lpmf", false, {}, {}, false);

    // A local declared AutoDiffable remains a var in generated C++ even when
    // assigned entirely from data. Its graph slot is parameter-free, so the
    // exact op can own the value, but propto must still retain the summand.
    same_local_autodiff_as_stan("categorical local autodiff valid",
                                "categorical_lpmf", true, {2}, {0.2, 0.3, 0.5});
    same_local_autodiff_as_stan("categorical local autodiff length-one array",
                                "categorical_lpmf", false, {2},
                                {0.2, 0.3, 0.5});
    {
      DataMap local_data;
      local_data.set_int_array("outcome", {2});
      local_data.set_real_array("arg", {0.2, 0.3, 0.5});
      CompiledModel local_model = compile_model(
          categorical_check_mir("categorical_lpmf", false, 1, 3, true, true),
          local_data);
      Executor local_ex(std::move(local_model.graph));
      local_model.bind(local_ex);
      local_ex.params_data()[0] = 0.0;
      expect_eq("categorical propto value-only double instantiation",
                local_ex.forward_value_only(), 0.0);
    }
    same_local_autodiff_as_stan("categorical local autodiff invalid",
                                "categorical_lpmf", true, {4}, {0.2, 0.3, 0.5});
    same_local_autodiff_as_stan("categorical logit local autodiff valid",
                                "categorical_logit_lpmf", false, {1, 3},
                                {-1.0, 0.0, 1.0});
    same_local_autodiff_as_stan("categorical logit local autodiff invalid",
                                "categorical_logit_lpmf", false, {1, 4},
                                {-1.0, 0.0, 1.0});
    auto check_local_exact_graph = [&](const std::string& label,
                                       const std::string& fn, bool scalar,
                                       const std::vector<int>& outcome,
                                       const std::vector<double>& arg) {
      DataMap local_data;
      if (scalar)
        local_data.set_int("outcome", outcome.at(0));
      else
        local_data.set_int_array("outcome", outcome);
      local_data.set_real_array("arg", arg);
      CompiledModel local_model =
          compile_model(categorical_check_mir(fn, scalar, (int)outcome.size(),
                                              (int)arg.size(), true, true),
                        local_data);
      bool found_exact = false;
      int exact_slot = -1;
      bool found_decomposition = false;
      for (const Op& op : local_model.graph.ops) {
        if (op.opcode == OP_CATEGORICAL) {
          found_exact = true;
          exact_slot = op.out;
        }
        found_decomposition = found_decomposition || op.opcode == OP_INDEX ||
                              op.opcode == OP_GATHER || op.opcode == OP_LOGV ||
                              op.opcode == OP_LOG_SOFTMAX ||
                              op.opcode == OP_SUM_VEC;
      }
      bool exact_value_used = exact_slot == local_model.graph.result_slot;
      for (const Op& op : local_model.graph.ops)
        for (int i = 0; i < op.n_in; ++i)
          exact_value_used = exact_value_used || op.in[i] == exact_slot;
      check(found_exact, label + " exact op");
      check(exact_value_used, label + " exact op owns target value");
      check(!found_decomposition, label + " no unchecked decomposition");
    };
    check_local_exact_graph("categorical local autodiff", "categorical_lpmf",
                            true, {4}, {0.2, 0.3, 0.5});
    check_local_exact_graph("categorical logit local autodiff",
                            "categorical_logit_lpmf", false, {1, 4},
                            {-1.0, 0.0, 1.0});

    auto malformed_refused = [](const std::string& mir, const DataMap& data) {
      try {
        (void)compile_model(mir, data);
      } catch (const std::exception& e) {
        return std::string(e.what()).find("categorical") != std::string::npos;
      }
      return false;
    };
    {
      std::string mir = categorical_check_mir("categorical_lpmf", true, 1, 3);
      const std::string from = "(Var outcome)";
      mir.replace(mir.find(from), from.size(), "(Var anchor)");
      DataMap malformed_data;
      malformed_data.set_int("outcome", 2);
      malformed_data.set_real_array("arg", {0.2, 0.3, 0.5});
      check(malformed_refused(mir, malformed_data),
            "categorical rejects data-only parameter outcome metadata");
    }
    {
      std::string mir = categorical_check_mir("categorical_lpmf", true, 1, 3);
      const std::string from = "outcome <opaque> SInt";
      mir.replace(mir.find(from), from.size(), "outcome <opaque> SReal");
      DataMap malformed_data;
      malformed_data.set_real("outcome", 2.0);
      malformed_data.set_real_array("arg", {0.2, 0.3, 0.5});
      check(malformed_refused(mir, malformed_data),
            "categorical rejects real binding with integer metadata");
    }
    {
      std::string mir = slurp("tests/fixtures/cat.tmir.sexp");
      const std::string from =
          "((pattern (Var theta))\n"
          "               (meta ((type_ UVector) (loc <opaque>) (adlevel "
          "AutoDiffable))))";
      const size_t pos = mir.find(from);
      check(pos != std::string::npos,
            "categorical malformed arg mutation found target");
      if (pos != std::string::npos) {
        std::string to = from;
        to.replace(to.find("AutoDiffable"), std::strlen("AutoDiffable"),
                   "DataOnly");
        mir.replace(pos, from.size(), to);
      }
      DataMap malformed_data =
          DataMap::from_json(R"({"K":3,"y":2,"ys":[3,1,3]})");
      check(malformed_refused(mir, malformed_data),
            "categorical rejects data-only parameter vector metadata");
    }
    {
      const std::string mir = categorical_check_mir("categorical_lpmf", false,
                                                    2, 3, true, false, true);
      DataMap malformed_data =
          DataMap::from_json(R"({"outcome":[[1,2]],"arg":[0.2,0.3,0.5]})");
      check(malformed_refused(mir, malformed_data),
            "categorical rejects flattened rank-two outcome metadata");
    }
    for (bool in_gq : {false, true}) {
      const std::string malformed =
          categorical_transformed_data_mismatch_mir(in_gq);
      check(malformed.find("(Var td_outcome)") != std::string::npos,
            "categorical transformed-data fixture rewrites outcome");
      DataMap malformed_data;
      malformed_data.set_real("source", 2.0);
      malformed_data.set_int("outcome", 2);
      malformed_data.set_real_array("arg", {0.2, 0.3, 0.5});
      check(malformed_refused(malformed, malformed_data),
            std::string("categorical rejects transformed-data binding in ") +
                (in_gq ? "generated quantities" : "log probability"));
    }
    {
      std::string malformed = categorical_write_array_mir(
          slurp("tests/fixtures/cat.tmir.sexp"), "categorical_lpmf", false);
      const size_t gq = malformed.find("(generate_quantities");
      const size_t call =
          malformed.find("(FunApp (StanLib categorical_lpmf", gq);
      const size_t outcome = malformed.find("(Var y)", call);
      check(gq != std::string::npos && call != std::string::npos &&
                outcome != std::string::npos,
            "categorical missing-binding mutation found target");
      if (outcome != std::string::npos)
        malformed.replace(outcome, std::strlen("(Var y)"), "(Var missing)");
      DataMap malformed_data =
          DataMap::from_json(R"({"K":3,"y":2,"ys":[3,1,3]})");
      check(malformed_refused(malformed, malformed_data),
            "categorical rejects missing GQ binding before fallback");
    }
    {
      std::string malformed = categorical_write_array_mir(
          slurp("tests/fixtures/cat.tmir.sexp"), "categorical_lpmf", false,
          false, false, true);
      const size_t gq = malformed.find("(generate_quantities");
      const size_t call =
          malformed.find("(FunApp (StanLib categorical_lpmf", gq);
      const std::string vector_arg =
          "((pattern (Var theta))\n"
          "             (meta ((type_ UVector) (adlevel DataOnly)))))";
      const size_t arg = malformed.find(vector_arg, call);
      check(gq != std::string::npos && call != std::string::npos &&
                arg != std::string::npos,
            "categorical malformed fallback mutation found target");
      if (arg != std::string::npos) {
        const std::string scalar_arg =
            "((pattern (Lit Real 1.0))\n"
            "             (meta ((type_ UReal) (adlevel DataOnly)))))";
        malformed.replace(arg, vector_arg.size(), scalar_arg);
      }
      DataMap malformed_data =
          DataMap::from_json(R"({"K":3,"y":2,"ys":[3,1,3]})");
      check(malformed_refused(malformed, malformed_data),
            "categorical rejects malformed GQ signature before fallback");
    }
    {
      std::string malformed = categorical_write_array_mir(
          slurp("tests/fixtures/cat.tmir.sexp"), "categorical_lpmf", false,
          false, true, true);
      const size_t gq = malformed.find("(generate_quantities");
      const size_t call =
          malformed.find("(FunApp (UserDefined data_categorical", gq);
      const std::string vector_arg =
          "((pattern (Var theta))\n"
          "             (meta ((type_ UVector) (adlevel DataOnly)))))";
      const size_t arg = malformed.find(vector_arg, call);
      check(gq != std::string::npos && call != std::string::npos &&
                arg != std::string::npos,
            "categorical malformed UDF fallback mutation found target");
      if (arg != std::string::npos) {
        const std::string scalar_arg =
            "((pattern (Lit Real 1.0))\n"
            "             (meta ((type_ UReal) (adlevel DataOnly)))))";
        malformed.replace(arg, vector_arg.size(), scalar_arg);
      }
      DataMap malformed_data =
          DataMap::from_json(R"({"K":3,"y":2,"ys":[3,1,3]})");
      check(malformed_refused(malformed, malformed_data),
            "categorical rejects malformed UDF actual before fallback");
    }
    {
      std::string malformed = categorical_write_array_mir(
          slurp("tests/fixtures/cat.tmir.sexp"), "categorical_lpmf", false,
          false, true, true);
      const std::string formal = "(DataOnly p UVector)";
      const size_t formal_at = malformed.find(formal);
      const size_t gq = malformed.find("(generate_quantities");
      const size_t call =
          malformed.find("(FunApp (UserDefined data_categorical", gq);
      const std::string actual =
          "((pattern (Var theta))\n"
          "             (meta ((type_ UVector) (adlevel DataOnly)))))";
      const size_t actual_at = malformed.find(actual, call);
      check(formal_at != std::string::npos && gq != std::string::npos &&
                call != std::string::npos && actual_at != std::string::npos,
            "categorical malformed UDF binding mutation found target");
      if (actual_at != std::string::npos) {
        std::string scalar_actual = actual;
        scalar_actual.replace(scalar_actual.find("UVector"),
                              std::strlen("UVector"), "UReal");
        malformed.replace(actual_at, actual.size(), scalar_actual);
      }
      if (formal_at != std::string::npos)
        malformed.replace(formal_at, formal.size(), "(DataOnly p UReal)");
      DataMap malformed_data =
          DataMap::from_json(R"({"K":3,"y":2,"ys":[3,1,3]})");
      check(malformed_refused(malformed, malformed_data),
            "categorical rejects UDF metadata that contradicts its binding");
    }
    for (const std::string& fn :
         {"categorical_lpmf", "categorical_logit_lpmf"}) {
      DataMap malformed_data;
      malformed_data.set_int("outcome", 2);
      malformed_data.set_real_array("arg",
                                    fn == "categorical_lpmf"
                                        ? std::vector<double>{0.2, 0.3, 0.5}
                                        : std::vector<double>{-1.0, 0.0, 1.0});
      bool direct_refused = false;
      try {
        (void)compile_model(categorical_udf_mir(fn, true, true, false, true),
                            malformed_data);
      } catch (const std::exception& e) {
        const std::string msg = e.what();
        direct_refused = msg.find("data-only") != std::string::npos &&
                         msg.find("parameter") != std::string::npos;
      }
      check(direct_refused, fn + " rejects parameter at data UDF formal");

      DataMap gq_data = DataMap::from_json(R"({"K":3,"y":2,"ys":[3,1,3]})");
      bool gq_accepted = true;
      try {
        CompiledModel gq_model = compile_model(
            categorical_write_array_mir(slurp("tests/fixtures/cat.tmir.sexp"),
                                        fn, false, false, true),
            gq_data);
      } catch (const std::exception& e) {
        (void)e;
        gq_accepted = false;
      }
      check(gq_accepted, fn + " accepts GQ parameter at data UDF formal");
    }

    DataMap d;
    d.set_int("outcome", 2);
    d.set_real_array("arg", {0.2, 0.3, 0.5});
    CompiledModel model =
        compile_model(categorical_check_mir("categorical_lpmf", true, 1, 3), d);
    Executor ex(std::move(model.graph));
    model.bind(ex);
    ex.params_data()[0] = 0.75;
    double grad = 0.0;
    expect_eq("categorical check zero target", ex.gradient(&grad), 0.75);
    expect_eq("categorical check disconnected gradient", grad, 1.0);

    DataMap effect_data;
    effect_data.set_int("outcome", 2);
    effect_data.set_real_array("arg", {0.2, 0.3, 0.5});
    std::optional<CompiledModel> effect_model;
    {
      stanli_test::StdoutCapture captured;
      effect_model = compile_model(categorical_effect_check_mir(), effect_data);
      check(captured.finish().empty(),
            "categorical outcome effect absent while compiling");
    }
    Executor effect_ex(std::move(effect_model->graph));
    effect_model->bind(effect_ex);
    effect_ex.params_data()[0] = 0.25;
    for (int i = 0; i < 2; ++i) {
      stanli_test::StdoutCapture captured;
      expect_eq("categorical effect target " + std::to_string(i),
                effect_ex.gradient(&grad), 0.25);
      check(captured.finish() == "categorical effect\n",
            "categorical outcome effect executes once " + std::to_string(i));
    }
  }

  {
    // Error path: an unsupported function is reported by name, never
    // silently miscompiled. Mutate a known-good fixture's density name.
    std::string txt = slurp("tests/fixtures/es.tmir.sexp");
    const std::string from = "normal_lpdf", to = "not_a_real_fn";
    for (size_t pos = txt.find(from); pos != std::string::npos;
         pos = txt.find(from, pos + to.size()))
      txt.replace(pos, from.size(), to);
    bool threw2 = false;
    try {
      compile_model(txt, data);
    } catch (const CompileError& e) {
      threw2 = std::string(e.what()).find("not_a_real_fn") != std::string::npos;
    }
    check(threw2, "unsupported function rejected with its name");
  }

  {
    // Data whose JSON shape disagrees with its declaration. CmdStan's
    // var_context validates every declared dimension before it reads a
    // value and throws std::invalid_argument naming the variable and both
    // shapes; stanli used to walk off the end of the short array instead,
    // which surfaced as whatever std::vector::at happened to say. A host
    // distinguishing bad data from a broken model reads the exception
    // type, so the type is the contract.
    DataMap d;
    d.set_int("J", 8);
    d.set_real_array("y", std::vector<double>(kY, kY + 7));
    d.set_real_array("sigma", std::vector<double>(kSigma, kSigma + 8));
    bool rejected = false;
    std::string msg;
    try {
      compile_model(slurp("tests/fixtures/es.tmir.sexp"), d);
    } catch (const std::invalid_argument& e) {
      rejected = true;
      msg = e.what();
    } catch (const std::exception& e) {
      msg = e.what();
    }
    check(rejected, "short data array rejected as invalid_argument: " + msg);
    check(msg.find("mismatch in dimension") != std::string::npos &&
              msg.find("name=y") != std::string::npos &&
              msg.find("(8)") != std::string::npos &&
              msg.find("(7)") != std::string::npos,
          "the rejection names the variable and both shapes: " + msg);

    // Too many values is the same disagreement; reading the declared
    // prefix and dropping the rest would be silent data loss.
    DataMap wide;
    wide.set_int("J", 8);
    wide.set_real_array("y", std::vector<double>(kY, kY + 8));
    wide.set_real_array("sigma", std::vector<double>(9, 1.0));
    bool wide_rejected = false;
    try {
      compile_model(slurp("tests/fixtures/es.tmir.sexp"), wide);
    } catch (const std::invalid_argument&) {
      wide_rejected = true;
    } catch (const std::exception&) {
    }
    check(wide_rejected, "over-long data array is rejected too");
  }

  if (failures == 0) std::printf("test_lower OK\n");
  return failures == 0 ? 0 : 1;
}
