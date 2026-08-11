// The graph compiler on eight schools: MIR text + data -> graph whose
// log_prob gradient matches a var reference that mirrors the lowering's
// evaluation order. Plus the unsupported-construct error path.
#include "env_helpers.hpp"
#include <stanli/compile.hpp>
#include <stanli/graph.hpp>
#include <stanli/optable.hpp>
#include <stanli/packet.hpp>

#include <stan/math.hpp>
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

  if (failures == 0) std::printf("test_lower OK\n");
  return failures == 0 ? 0 : 1;
}
