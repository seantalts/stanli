// The recorder scalar must (a) reproduce var-path gradients bitwise through
// unmodified stan-math templates and (b) support zero-copy promotion of a
// double buffer to an rvar view.
#include <stanli/recorder.hpp>

#include <stan/math.hpp>
#include <cmath>
#include <limits>
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
  using stan::math::value_of;
  using stan::math::value_of_rec;
  using stanli::rvar;
  std::vector<double> ys{1.3, -0.4, 2.2, 0.1, -1.7};
  const int N = static_cast<int>(ys.size());

  for (int n : {17, 33, 64, 100})
    for (int offset = 0; offset < 2; ++offset) {
      const int cols = 3;
      std::vector<double> storage(n * cols + 2);
      double* y = storage.data() + offset;
      for (int i = 0; i < n * cols; ++i) y[i] = 2.0 + std::sin(1.7 * i);
      const Eigen::VectorXd vec_copy = Eigen::Map<const Eigen::VectorXd>(y, n);
      const Eigen::MatrixXd mat_copy =
          Eigen::Map<const Eigen::MatrixXd>(y, n, cols);
      const auto vec_view = stanli::as_rvar(stanli::Desc{y, n});
      const auto mat_view =
          stanli::as_rvar_matrix(stanli::Desc{y, n * cols}, n, cols);
      decltype(auto) vec_val = stan::math::to_ref(value_of(vec_view));
      decltype(auto) vec_rec = stan::math::to_ref(value_of_rec(vec_view));
      decltype(auto) mat_val = stan::math::to_ref(value_of(mat_view));
      decltype(auto) mat_rec = stan::math::to_ref(value_of_rec(mat_view));
      const double* views[] = {vec_val.data(), vec_rec.data(), mat_val.data(),
                               mat_rec.data()};
      for (const double* p : views) {
        if (p != y) {
          ++failures;
          std::printf(
              "FAIL value_of of an rvar vector or matrix view copies\n");
        }
      }
      const double vec_sums[] = {vec_val.sum(), vec_rec.sum()};
      const double mat_sums[] = {mat_val.sum(), mat_rec.sum()};
      for (double got : vec_sums) {
        if (offset == 0) {
          expect_eq("aligned vector view sum", got, vec_copy.sum());
        } else if (std::abs(got - vec_copy.sum()) > 4e-16 * std::abs(got)) {
          ++failures;
          std::printf("FAIL offset vector view sum %.17g vs %.17g\n", got,
                      vec_copy.sum());
        }
      }
      for (double got : mat_sums) {
        if (offset == 0) {
          expect_eq("aligned matrix view sum", got, mat_copy.sum());
        } else if (std::abs(got - mat_copy.sum()) > 4e-16 * std::abs(got)) {
          ++failures;
          std::printf("FAIL offset matrix view sum %.17g vs %.17g\n", got,
                      mat_copy.sum());
        }
      }
    }

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
    s.len[0] = N;
    s.len[1] = 1;
    s.len[2] = 1;
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
    s.len[0] = N;
    s.len[1] = 1;
    s.len[2] = 1;
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

  {
    auto ry = stanli::as_rvar(stanli::Desc{ys.data(), N});
    const auto& cry = ry;
    decltype(auto) values =
        stan::math::to_ref(stan::math::as_value_column_array_or_scalar(ry));
    decltype(auto) const_values =
        stan::math::to_ref(stan::math::as_value_column_array_or_scalar(cry));
    for (const void* p : {static_cast<const void*>(values.data()),
                          static_cast<const void*>(const_values.data())}) {
      if (p != static_cast<const void*>(ys.data())) {
        ++failures;
        std::printf("FAIL value_of of an rvar view copies its values\n");
      }
    }
    for (int i = 0; i < N; ++i) {
      expect_eq("view value", values(i), ys[i]);
      expect_eq("const view value", const_values(i), ys[i]);
    }
  }

  // Recorder path 3: null buf[0] on a vector rvar edge.
  double gmu_null = 0, gsig_null = 0;
  {
    stanli::sink s;
    s.buf[0] = nullptr;
    s.buf[1] = &gmu_null;
    s.buf[2] = &gsig_null;
    s.len[0] = N;
    s.len[1] = 1;
    s.len[2] = 1;
    stanli::active_sink() = &s;
    Eigen::Matrix<rvar, -1, 1> ry3(N);
    for (int i = 0; i < N; ++i) ry3(i) = rvar(ys[i]);
    stan::math::normal_lpdf<false>(ry3, rvar(0.25), rvar(1.4));
    stanli::active_sink() = nullptr;
    expect_eq("null-buf value", s.value, vlp.val());
  }
  expect_eq("null-buf d/dmu", gmu_null, vmu.adj());
  expect_eq("null-buf d/dsigma", gsig_null, vsig.adj());

  // Recorder path 4: len[0] mismatched with the operand size.
  double gy_mismatch[5] = {111, 222, 333, 999, 888};
  double gmu_mm = 0, gsig_mm = 0;
  {
    stanli::sink s;
    s.buf[0] = gy_mismatch;
    s.buf[1] = &gmu_mm;
    s.buf[2] = &gsig_mm;
    s.len[0] = 3;
    s.len[1] = 1;
    s.len[2] = 1;
    stanli::active_sink() = &s;
    Eigen::Matrix<rvar, -1, 1> ry4(N);
    for (int i = 0; i < N; ++i) ry4(i) = rvar(ys[i]);
    stan::math::normal_lpdf<false>(ry4, rvar(0.25), rvar(1.4));
    stanli::active_sink() = nullptr;
    expect_eq("mismatch value", s.value, vlp.val());
  }
  expect_eq("mismatch d/dmu", gmu_mm, vmu.adj());
  expect_eq("mismatch d/dsigma", gsig_mm, vsig.adj());
  expect_eq("mismatch canary[3]", gy_mismatch[3], 999.0);
  expect_eq("mismatch canary[4]", gy_mismatch[4], 888.0);

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
    s.len[1] = 1;
    s.len[2] = 1;
    stanli::active_sink() = &s;
    stan::math::gamma_lpdf<false>(ymap, rvar(2.5), rvar(1.3));
    stanli::active_sink() = nullptr;
    expect_eq("gamma value", s.value, glp.val());
  }
  expect_eq("gamma d/dalpha", ga, va.adj());
  expect_eq("gamma d/dbeta", gb, vb.adj());
  stan::math::recover_memory();

  for (int n : {17, 33, 64, 100})
    for (int offset = 0; offset < 2; ++offset) {
      std::vector<double> storage(n + 2);
      double* y = storage.data() + offset;
      Eigen::Matrix<stan::math::var, -1, 1> vchi(n);
      for (int i = 0; i < n; ++i) {
        y[i] = 2.0 + std::sin(1.7 * i);
        vchi(i) = y[i];
      }
      stan::math::var vlp_chi = stan::math::chi_square_lpdf<false>(vchi, 3.5);
      vlp_chi.grad();
      std::vector<double> g(n);
      double gnu = 0;
      stanli::sink s;
      s.buf[0] = g.data();
      s.len[0] = n;
      s.len[1] = 1;
      s.buf[1] = &gnu;
      stanli::active_sink() = &s;
      stan::math::chi_square_lpdf<false>(stanli::as_rvar(stanli::Desc{y, n}),
                                         rvar(3.5));
      stanli::active_sink() = nullptr;
      if (offset == 0) {
        expect_eq("aligned view chi_square value", s.value, vlp_chi.val());
      } else {
        const double ulp =
            std::abs(s.value - vlp_chi.val()) /
            (std::nextafter(std::abs(vlp_chi.val()),
                            std::numeric_limits<double>::infinity()) -
             std::abs(vlp_chi.val()));
        if (ulp > 2) {
          ++failures;
          std::printf("FAIL offset view chi_square value: %g ULP\n", ulp);
        }
      }
      stan::math::recover_memory();
    }

  if (failures == 0) std::printf("test_recorder OK\n");
  return failures == 0 ? 0 : 1;
}
