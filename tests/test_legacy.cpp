// Legacy ops: unmodified stan-math functions behind the op interface via a
// nested var tape replayed at backward time. Scalar output (log_sum_exp) and
// vector output (softmax, exercising output-adjoint seeding).
#include <stanli/graph.hpp>
#include <stanli/optable.hpp>

#include <stan/math.hpp>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

static int failures = 0;
static void expect_eq(const std::string& what, double got, double want) {
  if (got != want) {
    ++failures;
    std::printf("FAIL %-16s got %.17g want %.17g\n", what.c_str(), got, want);
  }
}

// The log_sum_exp partials come off Eigen's packet exp, which rounds a last
// bit away from the var reference's scalar libm.
static void expect_ulp(const std::string& what, double got, double want) {
  if (got == want) return;
  const double ulp = std::nextafter(std::abs(want), 1e308) - std::abs(want);
  if (!(std::abs(got - want) <= ulp)) {
    ++failures;
    std::printf("FAIL %-16s got %.17g want %.17g\n", what.c_str(), got, want);
  }
}

int main() {
  using namespace stanli;
  using stan::math::var;
  const int N = 4;
  double vv[N] = {0.3, -1.2, 2.0, 0.4};

  // ---- scalar output: lp = log_sum_exp(v) --------------------------------
  {
    Graph g;
    const int v = g.add_slot(N, true);
    const int lp = g.add_slot(1, false);
    g.add_op(OP_LOG_SUM_EXP, {v}, lp);
    g.result_slot = lp;
    Executor ex(std::move(g));
    for (int i = 0; i < N; ++i) ex.param_ptr(v)[i] = vv[i];
    double grad[N];
    const double val = ex.gradient(grad);

    Eigen::Matrix<var, -1, 1> xv(N);
    for (int i = 0; i < N; ++i) xv(i) = vv[i];
    var vlp = stan::math::log_sum_exp(xv);
    vlp.grad();
    expect_eq("lse value", val, vlp.val());
    for (int i = 0; i < N; ++i)
      expect_ulp("lse d" + std::to_string(i), grad[i], xv(i).adj());
    stan::math::recover_memory();
  }

  // ---- vector output: lp = normal_lpdf(y | softmax(v), 0.5) --------------
  {
    double yv[N] = {0.3, 0.2, 0.4, 0.1};
    Graph g;
    const int v = g.add_slot(N, true);
    const int ys = g.add_slot(N, false);
    const int half = g.add_slot(1, false);
    const int sm = g.add_slot(N, false);
    const int lp = g.add_slot(1, false);
    g.add_op(OP_SOFTMAX, {v}, sm);
    g.add_op(OP_NORMAL_LPDF, {ys, sm, half}, lp);
    g.result_slot = lp;
    Executor ex(std::move(g));
    for (int i = 0; i < N; ++i) ex.param_ptr(v)[i] = vv[i];
    for (int i = 0; i < N; ++i) ex.value_ptr(ys)[i] = yv[i];
    ex.value_ptr(half)[0] = 0.5;
    double grad[N];
    const double val = ex.gradient(grad);

    // Reference with the same activity: v var, softmax through the var path,
    // then the same all-var-shape normal call the kernel makes.
    Eigen::Matrix<var, -1, 1> xv(N);
    for (int i = 0; i < N; ++i) xv(i) = vv[i];
    Eigen::Matrix<var, -1, 1> vsm = stan::math::softmax(xv);
    Eigen::Map<Eigen::VectorXd> ym(yv, N);
    var vlp = stan::math::normal_lpdf<false>(ym, vsm, 0.5);
    vlp.grad();
    expect_eq("softmax value", val, vlp.val());
    for (int i = 0; i < N; ++i)
      expect_eq("softmax d" + std::to_string(i), grad[i], xv(i).adj());
    stan::math::recover_memory();
  }

  if (failures == 0) std::printf("test_legacy OK\n");
  return failures == 0 ? 0 : 1;
}
