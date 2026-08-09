// The parameter transforms added for CmdStan parity: offset/multiplier,
// unit_vector, sum_to_zero_vector, corr_matrix, cov_matrix, and
// cholesky_factor_cov (square and rectangular).
//
// harnesses/transform_sweep.py is the real oracle -- it compares each of
// these bitwise against a CmdStan build of the same model -- but it needs
// a CmdStan checkout, so it cannot run in CI. This is the CI guard, and
// it deliberately uses oracles that do NOT go through the same kernels:
//
//   1. The unconstrained SIZE, computed by hand from the declarations.
//      This is the error a gradient check cannot see, because a wrong
//      free-parameter count still produces a perfectly finite gradient of
//      the wrong model.
//   2. Central finite differences of lp against the analytic gradient.
//      Independent of the backward kernels entirely.
//   3. The defining property of each constrained value: a unit_vector has
//      norm 1, a sum_to_zero_vector sums to 0, a correlation matrix has
//      unit diagonal, a Cholesky factor has positive diagonal.
#include <stanli/compile.hpp>

#include <cmath>
#include <cstdio>
#include <fstream>
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
static void expect_near(const std::string& what, double got, double want,
                        double tol) {
  if (!(std::fabs(got - want) <= tol)) {
    ++failures;
    std::printf("FAIL %-28s got %.12g want %.12g (tol %g)\n", what.c_str(), got,
                want, tol);
  }
}
static std::string slurp(const std::string& path) {
  std::ifstream f(path);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

int main() {
  using namespace stanli;

  DataMap data = DataMap::from_json_file("tests/fixtures/newtrans.json");
  CompiledModel cm =
      compile_model(slurp("tests/fixtures/newtrans.tmir.sexp"), data);
  Executor ex(std::move(cm.graph));
  cm.bind(ex);

  // ---- 1. the unconstrained size --------------------------------------
  // a(1) + b(3) + c(1) + d(1) + mu_p(3) + sg_p(3) + e(3)
  //   + unit_vector[3](3) + sum_to_zero_vector[4](3)
  //   + corr_matrix[3](3)          K*(K-1)/2
  //   + cov_matrix[3](6)           K + K*(K-1)/2
  //   + cholesky_factor_cov[3](6)  N*(N+1)/2
  //   + cholesky_factor_cov[4,3](9) N*(N+1)/2 + (M-N)*N
  const int64_t want_n = 1 + 3 + 1 + 1 + 3 + 3 + 3 + 3 + 3 + 3 + 6 + 6 + 9;
  expect("unconstrained size is " + std::to_string(want_n) + ", got " +
             std::to_string(ex.n_params()),
         ex.n_params() == want_n);
  if (ex.n_params() != want_n) return 1;  // everything below assumes it

  const int64_t n = ex.n_params();
  std::vector<double> q((size_t)n);
  for (int64_t i = 0; i < n; ++i)
    q[(size_t)i] = 0.15 + 0.07 * std::sin(0.9 * (double)i);

  // ---- 2. finite differences vs the analytic gradient -------------------
  std::vector<double> grad((size_t)n);
  for (int64_t i = 0; i < n; ++i) ex.params_data()[i] = q[(size_t)i];
  const double lp = ex.gradient(grad.data());
  expect("lp is finite", std::isfinite(lp));

  const double h = 1e-6;
  double worst = 0;
  int worst_i = -1;
  for (int64_t i = 0; i < n; ++i) {
    for (int64_t k = 0; k < n; ++k) ex.params_data()[k] = q[(size_t)k];
    ex.params_data()[i] = q[(size_t)i] + h;
    const double up = ex.forward();
    ex.params_data()[i] = q[(size_t)i] - h;
    const double dn = ex.forward();
    const double fd = (up - dn) / (2 * h);
    // Relative where the gradient is large, absolute where it is small:
    // a central difference on an lp of order 10 carries ~1e-9 of noise.
    const double scale = std::max(1.0, std::fabs(grad[(size_t)i]));
    const double err = std::fabs(fd - grad[(size_t)i]) / scale;
    if (err > worst) {
      worst = err;
      worst_i = (int)i;
    }
  }
  if (!(worst < 1e-5)) {
    ++failures;
    std::printf("FAIL finite differences: worst %.3g at parameter %d\n", worst,
                worst_i);
  }

  // ---- 3. each transform's defining property ---------------------------
  for (int64_t i = 0; i < n; ++i) ex.params_data()[i] = q[(size_t)i];
  ex.run_forward_only();

  const auto view =
      [&](const std::string& name) -> const CompiledModel::ParamView* {
    for (const auto& v : cm.views)
      if (v.name == name) return &v;
    return nullptr;
  };
  const auto vals = [&](const std::string& name, std::vector<double>& out) {
    const auto* v = view(name);
    if (!v) {
      ++failures;
      std::printf("FAIL no view for %s\n", name.c_str());
      return false;
    }
    const double* p = ex.value_ptr(v->slot);
    out.assign(p, p + v->len);
    return true;
  };

  std::vector<double> u, z, R, S, Lc, Lr, a, b, e, mu_p, sg_p;
  if (vals("u", u)) {
    expect("unit_vector has 3 elements", u.size() == 3);
    double nrm = 0;
    for (double t : u) nrm += t * t;
    expect_near("unit_vector norm", std::sqrt(nrm), 1.0, 1e-12);
  }
  if (vals("z", z)) {
    expect("sum_to_zero has 4 elements", z.size() == 4);
    double s = 0;
    for (double t : z) s += t;
    expect_near("sum_to_zero sums to 0", s, 0.0, 1e-12);
  }
  if (vals("R", R)) {
    expect("corr_matrix is 3x3", R.size() == 9);
    for (int k = 0; k < 3; ++k)
      expect_near("corr diag " + std::to_string(k), R[(size_t)(k * 3 + k)], 1.0,
                  1e-12);
    // Symmetric, and every off-diagonal a valid correlation.
    for (int i = 0; i < 3; ++i)
      for (int j = 0; j < 3; ++j) {
        expect_near("corr symmetry", R[(size_t)(j * 3 + i)],
                    R[(size_t)(i * 3 + j)], 1e-12);
        expect("corr in [-1, 1]", std::fabs(R[(size_t)(j * 3 + i)]) <= 1.0);
      }
  }
  if (vals("S", S)) {
    expect("cov_matrix is 3x3", S.size() == 9);
    for (int i = 0; i < 3; ++i)
      for (int j = 0; j < 3; ++j)
        expect_near("cov symmetry", S[(size_t)(j * 3 + i)],
                    S[(size_t)(i * 3 + j)], 1e-12);
    for (int k = 0; k < 3; ++k)
      expect("cov diag positive", S[(size_t)(k * 3 + k)] > 0);
  }
  if (vals("Lc", Lc)) {
    expect("cholesky_factor_cov[3] is 3x3", Lc.size() == 9);
    // Column-major, lower triangular: entry (i, j) with j > i is zero.
    for (int j = 0; j < 3; ++j)
      for (int i = 0; i < 3; ++i) {
        if (j > i)
          expect_near("Lc upper zero", Lc[(size_t)(j * 3 + i)], 0.0, 0.0);
        else if (i == j)
          expect("Lc diag positive", Lc[(size_t)(j * 3 + i)] > 0);
      }
  }
  if (vals("Lr", Lr)) {
    expect("cholesky_factor_cov[4,3] is 4x3", Lr.size() == 12);
    for (int j = 0; j < 3; ++j)
      for (int i = 0; i < 4; ++i) {
        if (j > i)
          expect_near("Lr upper zero", Lr[(size_t)(j * 4 + i)], 0.0, 0.0);
        else if (i == j)
          expect("Lr diag positive", Lr[(size_t)(j * 4 + i)] > 0);
      }
  }

  // offset/multiplier is affine and checkable in closed form: with data
  // m = 0.3 and s = 1.7, the constrained value is exactly s*x + m.
  if (vals("a", a)) {
    const double x = q[0];  // `a` is the first declared parameter
    expect_near("offset/multiplier scalar", a[0], std::fma(1.7, x, 0.3), 1e-13);
  }
  if (vals("b", b)) {
    expect("offset/multiplier vector has 3 elements", b.size() == 3);
    for (int i = 0; i < 3; ++i)
      expect_near("offset/multiplier vector " + std::to_string(i), b[(size_t)i],
                  std::fma(1.7, q[(size_t)(1 + i)], 0.3), 1e-13);
  }
  // The per-element form: offset and multiplier are themselves parameters,
  // so the check has to read their constrained values too.
  if (vals("e", e) && vals("mu_p", mu_p) && vals("sg_p", sg_p)) {
    // Parameters occupy the unconstrained vector in declaration order, so
    // e's raw slice starts after a(1) b(3) c(1) d(1) mu_p(3) sg_p(3).
    const int e0 = 1 + 3 + 1 + 1 + 3 + 3;
    for (int i = 0; i < 3; ++i)
      expect_near(
          "elementwise offset/multiplier " + std::to_string(i), e[(size_t)i],
          std::fma(sg_p[(size_t)i], q[(size_t)(e0 + i)], mu_p[(size_t)i]),
          1e-12);
  }

  if (failures == 0) std::printf("test_newtrans: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
