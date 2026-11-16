// ExecutorModel's var log_prob and the direct stan::model::gradient /
// log_prob_propto specializations against the bare Executor::gradient they
// all wrap: same value, same adjoints, bitwise, plus the rejected-point
// path.
#include "models.hpp"

#include <stanli/compile.hpp>
#include <stanli/model_adapter.hpp>

#include <stan/callbacks/stream_logger.hpp>
#include <stan/math.hpp>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

static int failures = 0;
static void expect(const std::string& what, bool ok) {
  if (!ok) {
    ++failures;
    std::printf("FAIL %s\n", what.c_str());
  }
}
static void expect_eq(const std::string& what, double got, double want) {
  if (got != want) {
    ++failures;
    std::printf("FAIL %-20s got %.17g want %.17g\n", what.c_str(), got, want);
  }
}
static std::string slurp(const std::string& p) {
  std::ifstream f(p);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

int main() {
  using namespace stanli;
  using stan::math::var;

  // A rejected point is -inf. An internal invariant failing is a bug, and
  // it reaches the caller instead of passing as a rejection.
  {
    Graph g;
    const int x = g.add_slot(1, true);
    const int lp = g.add_slot(1, false);
    g.add_op(OP_CATEGORICAL, {x}, lp);  // malformed: one input
    g.result_slot = lp;
    Executor ex(std::move(g));
    ExecutorModel model(ex);
    Eigen::VectorXd q(1);
    q(0) = 0.5;
    bool escaped = false;
    try {
      (void)model.log_prob<false, true, double>(q, nullptr);
    } catch (const std::logic_error&) {
      escaped = true;
    }
    expect("internal error escapes log_prob<double>", escaped);
    Eigen::Matrix<var, -1, 1> qv(1);
    qv(0) = 0.5;
    escaped = false;
    try {
      (void)model.log_prob<false, true, var>(qv, nullptr);
    } catch (const std::logic_error&) {
      escaped = true;
    }
    stan::math::recover_memory();
    expect("internal error escapes log_prob<var>", escaped);
    Eigen::VectorXd grad;
    double f = 0;
    stan::callbacks::stream_logger logger(std::cout, std::cout, std::cout,
                                          std::cerr, std::cerr);
    escaped = false;
    try {
      stan::model::gradient(model, q, f, grad, logger);
    } catch (const std::logic_error&) {
      escaped = true;
    }
    expect("internal error escapes stan::model::gradient", escaped);
  }

  {
    auto m = testmodels::eight_schools();
    Executor ex(std::move(m.graph));
    testmodels::fill_eight_schools_data(m, ex);
    const int NP = 10;
    ExecutorModel model(ex);

    const double qs[3][NP] = {
        {4.0, 1.0, 0.1, -0.2, 0.3, -0.4, 0.5, -0.6, 0.7, -0.8},
        {0.0, -1.5, 1.2, 0.8, -1.1, 0.05, -0.3, 0.9, -1.4, 0.2},
        {-2.5, 0.3, -0.7, 1.5, 0.6, -0.9, 1.1, 0.4, -0.2, -1.3}};

    for (int c = 0; c < 3; ++c) {
      ex.param_ptr(m.mu)[0] = qs[c][0];
      ex.param_ptr(m.log_tau)[0] = qs[c][1];
      for (int i = 0; i < 8; ++i) ex.param_ptr(m.theta_tilde)[i] = qs[c][2 + i];
      double grad_bare[NP];
      const double lp_bare = ex.gradient(grad_bare);

      Eigen::Matrix<var, -1, 1> qv(NP);
      for (int i = 0; i < NP; ++i) qv(i) = qs[c][i];
      var lp = model.log_prob<true, true, var>(qv, &std::cerr);
      lp.grad();

      const std::string tag = "case" + std::to_string(c);
      expect_eq(tag + " lp", lp.val(), lp_bare);
      for (int i = 0; i < NP; ++i)
        expect_eq(tag + " g" + std::to_string(i), qv(i).adj(), grad_bare[i]);
      stan::math::recover_memory();

      Eigen::VectorXd q(NP);
      for (int i = 0; i < NP; ++i) q(i) = qs[c][i];
      double f;
      Eigen::VectorXd g;
      stan::callbacks::logger logger;
      stan::model::gradient(model, q, f, g, logger);
      expect_eq(tag + " direct lp", f, lp_bare);
      for (int i = 0; i < NP; ++i)
        expect_eq(tag + " direct g" + std::to_string(i), g(i), grad_bare[i]);
      const double lp_propto =
          stan::model::log_prob_propto<true>(model, q, &std::cerr);
      expect_eq(tag + " direct propto", lp_propto, lp_bare);
    }
  }

  {
    const std::string mir = slurp("tests/fixtures/rejectprint.tmir.sexp");
    DataMap d = DataMap::from_json("{\"N\": 200, \"lim\": 2.5}");
    CompiledModel cm = compile_model(mir, d);
    Executor ex(std::move(cm.graph));
    cm.bind(ex);
    ExecutorModel model(ex);
    const int64_t n = ex.n_params();

    Eigen::Matrix<var, -1, 1> qv(n);
    for (int64_t i = 0; i < n; ++i) qv(i) = 0.1;
    var lp = model.log_prob<true, true, var>(qv, &std::cerr);
    expect("rejected point is -inf", std::isinf(lp.val()) && lp.val() < 0);
    lp.grad();
    for (int64_t i = 0; i < n; ++i)
      expect_eq("rejected g" + std::to_string(i), qv(i).adj(), 0.0);
    stan::math::recover_memory();

    Eigen::VectorXd q(n);
    for (int64_t i = 0; i < n; ++i) q(i) = 0.1;
    double f;
    Eigen::VectorXd g;
    stan::callbacks::logger logger;
    stan::model::gradient(model, q, f, g, logger);
    expect("rejected direct lp is -inf", std::isinf(f) && f < 0);
    for (int64_t i = 0; i < n; ++i)
      expect_eq("rejected direct g" + std::to_string(i), g(i), 0.0);
    const double lp_propto =
        stan::model::log_prob_propto<true>(model, q, &std::cerr);
    expect("rejected direct propto is -inf",
           std::isinf(lp_propto) && lp_propto < 0);
  }

  if (failures == 0) std::printf("test_model_adapter OK\n");
  return failures == 0 ? 0 : 1;
}
