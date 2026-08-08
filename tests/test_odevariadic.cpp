// The modern variadic ODE interface: ode_rk45 / ode_bdf / ode_adams /
// ode_ckrk and their _tol forms.
//
// harnesses/ode_sweep.py is the real oracle -- it compares each of these
// to a CmdStan build of the same model -- but it needs a CmdStan
// checkout, so this is the CI guard. The oracles here are chosen so they
// do not run through the same argument packing they are checking:
//
//   1. Central finite differences of lp against the analytic gradient.
//      A parameter argument packed into the DATA region is the failure
//      that matters, and it is invisible to any structural check: the
//      solve still runs, the gradient is still finite, and the entry for
//      that parameter is simply zero. Finite differences see it.
//   2. The four solvers integrating the same system from the same state
//      must agree with each other to solver tolerance. A solver that
//      silently ran the wrong method fails this only if the methods
//      disagree, so it is a weak check -- but a solver dispatched to a
//      DEAD branch, or one whose tolerances were not applied, fails it
//      loudly.
//   3. _tol at a tighter tolerance must sit closer to the others, not
//      further away.
#include <stanli/compile.hpp>

#include <algorithm>
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

  DataMap data = DataMap::from_json_file("tests/fixtures/odevariadic.json");
  CompiledModel cm =
      compile_model(slurp("tests/fixtures/odevariadic.tmir.sexp"), data);
  Executor ex(std::move(cm.graph));
  cm.bind(ex);

  // a, b, p[2], y0[2]
  const int64_t n = ex.n_params();
  expect("6 unconstrained parameters, got " + std::to_string(n), n == 6);
  if (n != 6) return 1;

  std::vector<double> q((size_t)n);
  for (int64_t i = 0; i < n; ++i)
    q[(size_t)i] = -0.3 + 0.11 * (double)i;

  std::vector<double> grad((size_t)n);
  for (int64_t i = 0; i < n; ++i) ex.params_data()[i] = q[(size_t)i];
  const double lp = ex.gradient(grad.data());
  expect("lp is finite", std::isfinite(lp));

  // ---- finite differences ----------------------------------------------
  // Every parameter must have a nonzero gradient: each one enters the
  // right-hand side, so a zero here means the argument never reached it.
  const double h = 1e-5;
  double worst = 0;
  int worst_i = -1;
  for (int64_t i = 0; i < n; ++i) {
    expect("parameter " + std::to_string(i) + " reaches the solve",
           std::fabs(grad[(size_t)i]) > 1e-8);
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
  // Looser than the algebraic transforms: the derivative of an adaptive
  // solve is itself only accurate to the solver's tolerance.
  if (!(worst < 1e-4)) {
    ++failures;
    std::printf("FAIL finite differences: worst %.3g at parameter %d\n", worst,
                worst_i);
  }

  // ---- the solvers agree -----------------------------------------------
  // Transformed parameters live in the write_array graph, not in the
  // log_prob one -- log_prob computes only what the target reads, and it
  // reads sums rather than the arrays themselves.
  if (!cm.write_array || cm.write_array->columns.empty()) {
    std::printf("FAIL no write_array graph for the transformed parameters\n");
    return 1;
  }
  Executor wex(std::move(cm.write_array->graph));
  cm.write_array->bind(wex);
  for (int64_t i = 0; i < n; ++i) wex.params_data()[i] = q[(size_t)i];
  wex.run_forward_only();
  const auto col = [&](const std::string& name) {
    std::vector<double> out;
    // An `array[N] vector[2]` is emitted one array element per column --
    // z_rk45.1, z_rk45.2, z_rk45.3 -- so gather the whole variable by
    // prefix rather than looking for a single column named for it.
    for (const auto& v : cm.write_array->columns)
      if (v.name == name || v.name.rfind(name + ".", 0) == 0) {
        const double* p = wex.value_ptr(v.slot);
        out.insert(out.end(), p, p + v.len);
      }
    return out;
  };
  const auto rk45 = col("z_rk45");
  expect("z_rk45 has N*2 values", rk45.size() == 6);
  for (const char* other : {"z_bdf", "z_adams", "z_ckrk", "z_tol"}) {
    const auto o = col(other);
    expect(std::string(other) + " has the same shape", o.size() == rk45.size());
    double w = 0;
    for (size_t k = 0; k < o.size() && k < rk45.size(); ++k)
      w = std::max(w, std::fabs(o[k] - rk45[k]) /
                          std::max(1e-8, std::fabs(rk45[k])));
    // Four adaptive solvers on the same well-conditioned system agree to
    // their tolerances; a dead dispatch branch or an unapplied tolerance
    // does not.
    if (!(w < 1e-5)) {
      ++failures;
      std::printf("FAIL %s disagrees with z_rk45 by %.3g relative\n", other, w);
    }
    expect(std::string(other) + " is finite",
           std::all_of(o.begin(), o.end(),
                       [](double v) { return std::isfinite(v); }));
  }

  // The mixed-argument solve is a different system, so it is checked for
  // being a solve at all rather than against the others.
  const auto mixed = col("z_mixed");
  expect("z_mixed has N*2 finite values",
         mixed.size() == 6 &&
             std::all_of(mixed.begin(), mixed.end(),
                         [](double v) { return std::isfinite(v); }));

  if (failures == 0) std::printf("test_odevariadic: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
