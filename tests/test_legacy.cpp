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
#include <vector>

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

// dirichlet through the graph, with a non-unit output adjoint: the kernel
// grads its nested tape once in the forward with a seed of 1 and scales in
// the backward, so a unit seed would not exercise the scaling at all.
static void check_dirichlet(const std::string& tag, int reps, bool propto,
                            bool alpha_var) {
  using namespace stanli;
  using stan::math::var;
  const int K = 4;
  const double seed = -0.73;
  const double simplex[4] = {0.1, 0.2, 0.3, 0.4};
  std::vector<double> th((size_t)K * reps), al(K);
  for (int r = 0; r < reps; ++r)
    for (int i = 0; i < K; ++i) th[(size_t)r * K + i] = simplex[(i + r) % K];
  for (int i = 0; i < K; ++i) al[(size_t)i] = 1.3 + 0.6 * i;

  Graph g;
  const int t_slot = g.add_slot((int64_t)K * reps, true);
  const int a_slot = g.add_slot(K, alpha_var);
  const int s_slot = g.add_slot(1, false);
  const int lp = g.add_slot(1, false);
  const int total = g.add_slot(1, false);
  g.add_op(OP_DIRICHLET_LPDF, {t_slot, a_slot}, lp);
  g.ops.back().variant =
      (propto ? 0x80u : 0x00u) | 0x1u | (alpha_var ? 0x2u : 0x0u);
  g.add_op(OP_MUL, {lp, s_slot}, total);
  g.result_slot = total;

  Executor ex(std::move(g));
  for (int i = 0; i < K * reps; ++i) ex.param_ptr(t_slot)[i] = th[(size_t)i];
  double* ap = alpha_var ? ex.param_ptr(a_slot) : ex.value_ptr(a_slot);
  for (int i = 0; i < K; ++i) ap[i] = al[(size_t)i];
  ex.value_ptr(s_slot)[0] = seed;
  std::vector<double> grad((size_t)K * reps + (alpha_var ? K : 0), 0.0);
  const double val = ex.gradient(grad.data());

  stan::math::nested_rev_autodiff nested;
  std::vector<Eigen::Matrix<var, -1, 1>> tv((size_t)reps,
                                            Eigen::Matrix<var, -1, 1>(K));
  for (int r = 0; r < reps; ++r)
    for (int i = 0; i < K; ++i) {
      tv[(size_t)r](i) = th[(size_t)r * K + i];
    }
  Eigen::Matrix<var, -1, 1> av(K);
  for (int i = 0; i < K; ++i) av(i) = al[(size_t)i];
  Eigen::Map<const Eigen::VectorXd> ad(al.data(), K);
  var ref;
  if (reps > 1) {
    if (alpha_var)
      ref = propto ? stan::math::dirichlet_lpdf<true>(tv, av)
                   : stan::math::dirichlet_lpdf<false>(tv, av);
    else
      ref = propto ? stan::math::dirichlet_lpdf<true>(tv, ad)
                   : stan::math::dirichlet_lpdf<false>(tv, ad);
  } else {
    if (alpha_var)
      ref = propto ? stan::math::dirichlet_lpdf<true>(tv[0], av)
                   : stan::math::dirichlet_lpdf<false>(tv[0], av);
    else
      ref = propto ? stan::math::dirichlet_lpdf<true>(tv[0], ad)
                   : stan::math::dirichlet_lpdf<false>(tv[0], ad);
  }
  var scaled = ref * seed;
  stan::math::grad(scaled.vi_);
  expect_eq(tag + " value", val, scaled.val());
  for (int r = 0; r < reps; ++r)
    for (int i = 0; i < K; ++i)
      expect_eq(tag + " dtheta" + std::to_string(r * K + i),
                grad[(size_t)r * K + i], tv[(size_t)r](i).adj());
  if (alpha_var)
    for (int i = 0; i < K; ++i)
      expect_eq(tag + " dalpha" + std::to_string(i), grad[(size_t)K * reps + i],
                av(i).adj());
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

  check_dirichlet("single propto", 1, true, true);
  check_dirichlet("single full", 1, false, true);
  check_dirichlet("single alpha data", 1, false, false);
  check_dirichlet("vectorized propto", 3, true, true);
  check_dirichlet("vectorized alpha data", 3, false, false);

  if (failures == 0) std::printf("test_legacy OK\n");
  return failures == 0 ? 0 : 1;
}
