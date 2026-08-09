// The recorder scalar must (a) reproduce var-path gradients bitwise through
// unmodified stan-math templates and (b) support zero-copy promotion of a
// double buffer to an rvar view.
#include <stanli/recorder.hpp>

#include <stan/math.hpp>
#include <cstdio>
#include <vector>

static int failures = 0;
static void expect_eq(const char* what, double got, double want) {
  if (got != want) {
    ++failures;
    std::printf("FAIL %-24s got %.17g want %.17g\n", what, got, want);
  }
}

int main() {
  using stanli::rvar;
  std::vector<double> ys{1.3, -0.4, 2.2, 0.1, -1.7};
  const int N = static_cast<int>(ys.size());

  // Reference: var path.
  Eigen::Matrix<stan::math::var, -1, 1> vy(N);
  for (int i = 0; i < N; ++i) vy(i) = ys[i];
  stan::math::var vmu = 0.25, vsig = 1.4;
  stan::math::var vlp = stan::math::normal_lpdf<false>(vy, vmu, vsig);
  vlp.grad();

  // Recorder path 1: copied rvar vector.
  double gy_copy[8]{}, gmu_copy = 0, gsig_copy = 0;
  {
    stanli::sink s;
    s.buf[0] = gy_copy;
    s.buf[1] = &gmu_copy;
    s.buf[2] = &gsig_copy;
    stanli::active_sink() = &s;
    Eigen::Matrix<rvar, -1, 1> ry(N);
    for (int i = 0; i < N; ++i) ry(i) = rvar(ys[i]);
    stan::math::normal_lpdf<false>(ry, rvar(0.25), rvar(1.4));
    stanli::active_sink() = nullptr;
    expect_eq("copied value", s.value, vlp.val());
  }

  // Recorder path 2: zero-copy map over the double buffer.
  double gy_map[8]{}, gmu_map = 0, gsig_map = 0;
  {
    stanli::sink s;
    s.buf[0] = gy_map;
    s.buf[1] = &gmu_map;
    s.buf[2] = &gsig_map;
    stanli::active_sink() = &s;
    auto ry = stanli::as_rvar(stanli::Desc{ys.data(), N});
    stan::math::normal_lpdf<false>(ry, rvar(0.25), rvar(1.4));
    stanli::active_sink() = nullptr;
    expect_eq("mapped value", s.value, vlp.val());
  }

  expect_eq("copied d/dmu", gmu_copy, vmu.adj());
  expect_eq("copied d/dsigma", gsig_copy, vsig.adj());
  expect_eq("mapped d/dmu", gmu_map, vmu.adj());
  expect_eq("mapped d/dsigma", gsig_map, vsig.adj());
  for (int i = 0; i < N; ++i) {
    expect_eq("copied d/dy", gy_copy[i], vy(i).adj());
    expect_eq("mapped d/dy", gy_map[i], vy(i).adj());
  }
  stan::math::recover_memory();

  // gamma_lpdf with a data outcome: only alpha/beta partials.
  std::vector<double> pos{0.9, 1.7, 0.35, 2.4, 1.1};
  stan::math::var va = 2.5, vb = 1.3;
  Eigen::Map<Eigen::VectorXd> ymap(pos.data(), N);
  stan::math::var glp = stan::math::gamma_lpdf<false>(ymap, va, vb);
  glp.grad();
  double ga = 0, gb = 0;
  {
    stanli::sink s;
    s.buf[0] = nullptr;
    s.buf[1] = &ga;
    s.buf[2] = &gb;
    stanli::active_sink() = &s;
    stan::math::gamma_lpdf<false>(ymap, rvar(2.5), rvar(1.3));
    stanli::active_sink() = nullptr;
    expect_eq("gamma value", s.value, glp.val());
  }
  expect_eq("gamma d/dalpha", ga, va.adj());
  expect_eq("gamma d/dbeta", gb, vb.adj());
  stan::math::recover_memory();

  if (failures == 0) std::printf("test_recorder OK\n");
  return failures == 0 ? 0 : 1;
}
