// L-BFGS over the executor's gradient.
//
// The optimizer is stan's own, so what is tested here is the adapter --
// that the model concept the services reach through (log_prob in both its
// vector and Eigen forms, constrained_param_names, write_array) is wired
// to the right buffers. The oracle is a posterior whose answer is known
// in closed form, because "the optimizer returned something" and "the
// optimizer returned the mode" look identical otherwise.
//
// conj: y ~ normal(mu, sigma) with sigma fixed and mu ~ normal(m0, s0)
// has a Gaussian posterior with a mode at the precision-weighted mean.
// Here the model is built directly rather than compiled, so the expected
// value is arithmetic rather than a second implementation.
#include <stanli/estimate.hpp>
#include <stanli/optable.hpp>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

static int failures = 0;
static void expect(const std::string& what, bool ok) {
  if (!ok) {
    ++failures;
    std::printf("FAIL %s\n", what.c_str());
  }
}
static void expect_near(const std::string& what, double got, double want,
                        double tol) {
  if (!(std::fabs(got - want) <= tol)) {
    ++failures;
    std::printf("FAIL %-30s got %.10g want %.10g (tol %g)\n", what.c_str(),
                got, want, tol);
  }
}

// lp(mu) = normal_lpdf(y | mu, sigma) + normal_lpdf(mu | m0, s0), with y
// a length-N data vector. Unconstrained, so there is no Jacobian and the
// mode is the posterior mode either way.
static stanli::Graph conj_graph(int N) {
  using namespace stanli;
  Graph g;
  const int mu = g.add_slot(1, true);
  const int y = g.add_slot(N, false);
  const int sigma = g.add_slot(1, false);
  const int m0 = g.add_slot(1, false);
  const int s0 = g.add_slot(1, false);
  const int lp1 = g.add_slot(1, false);
  const int lp2 = g.add_slot(1, false);
  const int lp = g.add_slot(1, false);
  g.add_op(OP_NORMAL_LPDF, {y, mu, sigma}, lp1);
  g.add_op(OP_NORMAL_LPDF, {mu, m0, s0}, lp2);
  g.add_op(OP_ADD_N, {lp1, lp2}, lp);
  g.result_slot = lp;
  return g;
}

int main() {
  using namespace stanli;

  const int N = 5;
  const double ys[N] = {1.2, 0.8, 1.6, 0.4, 1.0};
  const double sigma = 0.7, m0 = 0.0, s0 = 2.0;

  Executor ex(conj_graph(N));
  for (int i = 0; i < N; ++i) ex.value_ptr(1)[i] = ys[i];
  ex.value_ptr(2)[0] = sigma;
  ex.value_ptr(3)[0] = m0;
  ex.value_ptr(4)[0] = s0;

  // Closed form: the posterior mode of a conjugate normal mean.
  double ybar = 0;
  for (int i = 0; i < N; ++i) ybar += ys[i];
  ybar /= N;
  const double prec_lik = N / (sigma * sigma), prec_pri = 1.0 / (s0 * s0);
  const double want = (prec_lik * ybar + prec_pri * m0) / (prec_lik + prec_pri);

  // ---- L-BFGS -----------------------------------------------------------
  {
    OptimizeConfig cfg;
    cfg.seed = 3;
    OptimizeResult r = run_optimize(ex, nullptr, cfg);
    expect("optimize converged, code " + std::to_string(r.return_code),
           r.return_code == 0);
    expect("optimize returned one parameter", r.unconstrained.size() == 1);
    expect_near("optimize finds the posterior mode", r.unconstrained[0], want,
                1e-6);
    // lp at the mode must be the model's own lp there, not the objective
    // the optimizer minimizes -- a sign slip here is easy and silent.
    ex.params_data()[0] = r.unconstrained[0];
    expect_near("reported lp is the model's lp", r.lp, ex.forward(), 1e-9);

    // Starting from the answer must not move it.
    OptimizeConfig c2 = cfg;
    const double at = want;
    c2.init = &at;
    OptimizeResult r2 = run_optimize(ex, nullptr, c2);
    expect_near("optimize is stable at the mode", r2.unconstrained[0], want,
                1e-6);
  }

  if (failures == 0) std::printf("test_estimate: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
