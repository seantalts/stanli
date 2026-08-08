// brms's monotonic effects, mo().
//
// The whole model is here for one line of it:
//
//   return rows(scale) * sum(scale[1:i]);
//
// `rows` was answered only where an INTEGER was expected -- a size, a
// loop bound, a declaration dimension. brms writes it in the middle of
// real arithmetic, which reached "unsupported function rows" instead, so
// every model with an `mo()` term failed to compile.
//
// harnesses/brms_sweep.py verifies this shape against CmdStan (bitwise);
// this is the CI guard, and it checks the thing a compile alone does not:
// that the constant folded in is the RIGHT one. rows(simo_1) is the
// simplex length, so the gradient of the monotonic term scales with it,
// and folding the wrong dimension would still compile and still produce
// a finite gradient.
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
static std::string slurp(const std::string& p) {
  std::ifstream f(p);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

int main() {
  using namespace stanli;

  DataMap data = DataMap::from_json_file("tests/fixtures/brmsmono.json");
  CompiledModel cm =
      compile_model(slurp("tests/fixtures/brmsmono.tmir.sexp"), data);
  Executor ex(std::move(cm.graph));
  cm.bind(ex);

  // Intercept, sigma, bsp_1, and simo_1 (a simplex[2], so 1 free).
  const int64_t n = ex.n_params();
  expect("4 unconstrained parameters, got " + std::to_string(n), n == 4);
  if (n != 4) return 1;

  std::vector<double> q((size_t)n), grad((size_t)n);
  for (int64_t i = 0; i < n; ++i) q[(size_t)i] = 0.2 - 0.13 * (double)i;
  for (int64_t i = 0; i < n; ++i) ex.params_data()[i] = q[(size_t)i];
  const double lp = ex.gradient(grad.data());
  expect("lp is finite", std::isfinite(lp));

  // Central finite differences. This is what pins the folded constant:
  // rows(simo_1) multiplies the monotonic term, so a wrong value changes
  // the gradient of bsp_1 and simo_1 without changing anything
  // structural.
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
    const double scale = std::max(1.0, std::fabs(grad[(size_t)i]));
    const double err = std::fabs(fd - grad[(size_t)i]) / scale;
    if (err > worst) {
      worst = err;
      worst_i = (int)i;
    }
  }
  // Repeated evaluation must be idempotent. This is the check that
  // found the constant-folding bug behind this fixture: `mu` was folded
  // to a bind-time fill and the in-place element writes then accumulated
  // across evaluations, so the same point gave a different lp every
  // time. Nothing structural showed it, and the corpus rig cannot --
  // it evaluates one point per process.
  for (int64_t k = 0; k < n; ++k) ex.params_data()[k] = q[(size_t)k];
  const double again = ex.forward();
  expect("evaluating the same point twice gives the same lp",
         again == lp);
  if (!(worst < 1e-5)) {
    ++failures;
    std::printf("FAIL finite differences: worst %.3g at parameter %d\n", worst,
                worst_i);
  }

  // bsp_1 is the monotonic slope; if the mo() term were folded to zero
  // (the shape a wrong `rows` could take) its gradient would vanish.
  expect("the monotonic term reaches the target",
         std::fabs(grad[2]) > 1e-8);

  if (failures == 0) std::printf("test_brmsmono: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
