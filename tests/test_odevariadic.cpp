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
#include <stanli/ode.hpp>
#include <stanli/optable.hpp>

#include <stan/math.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <type_traits>
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

static void expect_close(const std::string& what, double got, double want,
                         double rel = 2e-9) {
  const double scale = std::max(1.0, std::fabs(want));
  if (!(std::fabs(got - want) <= rel * scale)) {
    ++failures;
    std::printf("FAIL %s: got %.17g want %.17g\n", what.c_str(), got, want);
  }
}

// An independent spelling of the fixture's `rhs`. It is passed straight to
// stan-math with the same mixed y0/theta scalar types as OP_ODE, so these
// checks catch an accidental promotion of either data side back to var.
struct DirectRhs {
  template <typename T_y, typename T_theta>
  Eigen::Matrix<stan::return_type_t<T_y, T_theta>, Eigen::Dynamic, 1>
  operator()(const double&, const Eigen::Matrix<T_y, Eigen::Dynamic, 1>& y,
             std::ostream*, const std::vector<T_theta>& theta,
             const std::vector<double>&, const std::vector<int>&) const {
    using T = stan::return_type_t<T_y, T_theta>;
    Eigen::Matrix<T, Eigen::Dynamic, 1> dy(2);
    dy(0) = -theta[0] * y(0) + theta[1] * y(1);
    dy(1) = theta[0] * y(0) - theta[1] * y(1);
    return dy;
  }
};

struct OdeActivityRun {
  std::vector<double> value;
  std::vector<double> y_grad;
  std::vector<double> theta_grad;
  std::vector<double> jacobian;
};

static const std::vector<double> test_y0{1.1, 0.7};
static const std::vector<double> test_theta{0.2, 0.35};
static const std::vector<double> test_weights{0.25, -0.4, 0.15,
                                              0.3,  -0.2, 0.45};

template <bool YAutodiff, bool ThetaAutodiff>
OdeActivityRun direct_activity_run(const stanli::OdeSpec& spec) {
  using T_y0 = std::conditional_t<YAutodiff, stan::math::var, double>;
  using T_theta = std::conditional_t<ThetaAutodiff, stan::math::var, double>;
  OdeActivityRun out;
  out.y_grad.assign(test_y0.size(), 0.0);
  out.theta_grad.assign(test_theta.size(), 0.0);

  if constexpr (!YAutodiff && !ThetaAutodiff) {
    Eigen::VectorXd y0((Eigen::Index)test_y0.size());
    for (size_t i = 0; i < test_y0.size(); ++i)
      y0((Eigen::Index)i) = test_y0[i];
    const auto solved = stan::math::ode_rk45_tol(
        DirectRhs{}, y0, spec.t0, spec.ts, spec.rtol, spec.atol, spec.max_steps,
        nullptr, test_theta, spec.x_r, spec.x_i);
    for (const auto& state : solved)
      for (Eigen::Index k = 0; k < state.size(); ++k)
        out.value.push_back(state(k));
  } else {
    stan::math::nested_rev_autodiff nested;
    Eigen::Matrix<T_y0, Eigen::Dynamic, 1> y0((Eigen::Index)test_y0.size());
    for (size_t i = 0; i < test_y0.size(); ++i)
      y0((Eigen::Index)i) = test_y0[i];
    std::vector<T_theta> theta(test_theta.begin(), test_theta.end());
    const auto solved = stan::math::ode_rk45_tol(
        DirectRhs{}, y0, spec.t0, spec.ts, spec.rtol, spec.atol, spec.max_steps,
        nullptr, theta, spec.x_r, spec.x_i);
    stan::math::var weighted = 0.0;
    size_t o = 0;
    for (const auto& state : solved)
      for (Eigen::Index k = 0; k < state.size(); ++k, ++o) {
        out.value.push_back(state(k).val());
        weighted += state(k) * test_weights[o];
      }
    stan::math::grad(weighted.vi_);
    if constexpr (YAutodiff)
      for (size_t i = 0; i < test_y0.size(); ++i)
        out.y_grad[i] = y0((Eigen::Index)i).adj();
    if constexpr (ThetaAutodiff)
      for (size_t i = 0; i < test_theta.size(); ++i)
        out.theta_grad[i] = theta[i].adj();
  }
  return out;
}

template <bool YAutodiff, bool ThetaAutodiff>
OdeActivityRun kernel_activity_run(const stanli::OdeSpec& spec) {
  using namespace stanli;
  OdeActivityRun out;
  out.value.assign(spec.ts.size() * test_y0.size(), 0.0);
  out.y_grad.assign(test_y0.size(), 0.0);
  out.theta_grad.assign(test_theta.size(), 0.0);
  out.jacobian.assign(out.value.size() * (test_y0.size() + test_theta.size()),
                      std::numeric_limits<double>::quiet_NaN());

  KernelCtx ctx;
  ctx.n_in = 2;
  ctx.in[0] =
      Desc{const_cast<double*>(test_y0.data()), (int64_t)test_y0.size()};
  ctx.in[1] =
      Desc{const_cast<double*>(test_theta.data()), (int64_t)test_theta.size()};
  ctx.out = Desc{out.value.data(), (int64_t)out.value.size()};
  ctx.scratch = out.jacobian.data();
  ctx.udata = &spec;
  ctx.variant =
      (uint8_t)(0x4u | (YAutodiff ? 0x1u : 0u) | (ThetaAutodiff ? 0x2u : 0u));
  // Both buffers deliberately exist for every case. The variant records the
  // C++ type; adjoint storage does not, and ode_bwd must not scatter through a
  // type-inactive side merely because a buffer happens to be present.
  ctx.in_adj[0] = Desc{out.y_grad.data(), (int64_t)out.y_grad.size()};
  ctx.in_adj[1] = Desc{out.theta_grad.data(), (int64_t)out.theta_grad.size()};
  ctx.out_adj_vec = Desc{const_cast<double*>(test_weights.data()),
                         (int64_t)test_weights.size()};
  const Kernel* ode = find_kernel(OP_ODE);
  ode->forward(ctx);
  const std::vector<double> forward_jacobian = out.jacobian;
  // Forward writes deterministic zero columns for inactive scalar types, but
  // backward must not rely on 0 * adjoint being harmless: a stale/poisoned
  // column can contain NaN. Poison those columns after checking forward and
  // require the type mask, rather than mere buffer presence, to gate scatter.
  const size_t width = test_y0.size() + test_theta.size();
  for (size_t o = 0; o < out.value.size(); ++o) {
    if constexpr (!YAutodiff)
      for (size_t i = 0; i < test_y0.size(); ++i)
        out.jacobian[o * width + i] = std::numeric_limits<double>::quiet_NaN();
    if constexpr (!ThetaAutodiff)
      for (size_t i = 0; i < test_theta.size(); ++i)
        out.jacobian[o * width + test_y0.size() + i] =
            std::numeric_limits<double>::quiet_NaN();
  }
  ode->backward(ctx);
  out.jacobian = forward_jacobian;
  return out;
}

template <bool YAutodiff, bool ThetaAutodiff>
void check_activity_case(const stanli::OdeSpec& spec, const char* label) {
  const OdeActivityRun got =
      kernel_activity_run<YAutodiff, ThetaAutodiff>(spec);
  const OdeActivityRun want =
      direct_activity_run<YAutodiff, ThetaAutodiff>(spec);
  expect(std::string(label) + " output shape",
         got.value.size() == want.value.size());
  for (size_t i = 0; i < got.value.size() && i < want.value.size(); ++i)
    expect_close(std::string(label) + " value " + std::to_string(i),
                 got.value[i], want.value[i]);
  for (size_t i = 0; i < got.y_grad.size(); ++i)
    expect_close(std::string(label) + " y gradient " + std::to_string(i),
                 got.y_grad[i], want.y_grad[i]);
  for (size_t i = 0; i < got.theta_grad.size(); ++i)
    expect_close(std::string(label) + " theta gradient " + std::to_string(i),
                 got.theta_grad[i], want.theta_grad[i]);

  const size_t width = test_y0.size() + test_theta.size();
  for (size_t o = 0; o < got.value.size(); ++o) {
    if constexpr (!YAutodiff)
      for (size_t i = 0; i < test_y0.size(); ++i)
        expect(std::string(label) + " zero y Jacobian column",
               got.jacobian[o * width + i] == 0.0);
    if constexpr (!ThetaAutodiff)
      for (size_t i = 0; i < test_theta.size(); ++i)
        expect(std::string(label) + " zero theta Jacobian column",
               got.jacobian[o * width + test_y0.size() + i] == 0.0);
  }
}

static void check_precomputed_jacobian_harvest() {
  stan::math::nested_rev_autodiff nested;
  std::vector<stan::math::var> inputs{0.2, -0.35, 1.1, 0.7};
  const std::vector<std::vector<double>> jacobian{
      {0.25, -0.5, 0.75, -1.0}, {-1.25, 1.5, -1.75, 2.0},
      {2.25, -2.5, 2.75, -3.0}, {-3.25, 3.5, -3.75, 4.0},
      {4.25, -4.5, 4.75, -5.0}, {-5.25, 5.5, -5.75, 6.0},
  };
  std::vector<stan::math::var> outputs;
  for (size_t o = 0; o < jacobian.size(); ++o)
    outputs.push_back(stan::math::precomputed_gradients(10.0 + (double)o,
                                                        inputs, jacobian[o]));

  std::vector<std::vector<double>> full(outputs.size());
  for (size_t o = outputs.size(); o-- > 0;) {
    stan::math::set_zero_all_adjoints_nested();
    stan::math::grad(outputs[o].vi_);
    for (const auto& input : inputs) full[o].push_back(input.adj());
  }

  stan::math::set_zero_all_adjoints_nested();
  for (size_t o = outputs.size(); o-- > 0;) {
    outputs[o].vi_->adj_ = 1.0;
    outputs[o].vi_->chain();
    for (size_t i = 0; i < inputs.size(); ++i) {
      expect("direct precomputed-gradient harvest matches full reverse sweep",
             inputs[i].adj() == full[o][i]);
      inputs[i].vi_->adj_ = 0.0;
    }
    outputs[o].vi_->adj_ = 0.0;
  }
}

static const stanli::OdeSpec* fixture_rk45_spec(const stanli::Graph& graph) {
  using namespace stanli;
  for (const Op& op : graph.ops)
    if (op.opcode == OP_ODE) {
      const auto* spec = static_cast<const OdeSpec*>(op.udata);
      if (!spec->legacy && spec->solver == OdeSpec::RK45 &&
          spec->rhs_name == "rhs")
        return spec;
    }
  return nullptr;
}

int main() {
  using namespace stanli;

  DataMap data = DataMap::from_json_file("tests/fixtures/odevariadic.json");
  CompiledModel cm =
      compile_model(slurp("tests/fixtures/odevariadic.tmir.sexp"), data);
  const OdeSpec* rk45_spec = fixture_rk45_spec(cm.graph);
  expect("fixture exposes a modern rk45 rhs", rk45_spec != nullptr);

  bool log_has_vv = false, log_has_vd = false, log_has_dv = false;
  for (const Op& op : cm.graph.ops)
    if (op.opcode == OP_ODE) {
      expect("lowered log_prob ODE has an explicit type mask",
             (op.variant & 0x4u) != 0);
      log_has_vv = log_has_vv || op.variant == 0x7u;
      log_has_vd = log_has_vd || op.variant == 0x5u;
      log_has_dv = log_has_dv || op.variant == 0x6u;
    }
  expect("log_prob contains a var/var ODE", log_has_vv);
  expect("log_prob contains an active-y/data-theta ODE", log_has_vd);
  expect("log_prob contains a data-y/active-theta ODE", log_has_dv);

  bool write_has_dd = false;
  if (cm.write_array)
    for (const Op& op : cm.write_array->graph.ops)
      if (op.opcode == OP_ODE) {
        expect("lowered write_array ODE has an explicit type mask",
               (op.variant & 0x4u) != 0);
        write_has_dd = write_has_dd || op.variant == 0x4u;
      }
  expect("write_array contains a double/double ODE", write_has_dd);

  check_precomputed_jacobian_harvest();

  if (rk45_spec) {
    check_activity_case<true, false>(*rk45_spec, "active y/data theta");
    check_activity_case<false, true>(*rk45_spec, "data y/active theta");
    check_activity_case<true, true>(*rk45_spec, "active y/active theta");
    check_activity_case<false, false>(*rk45_spec, "data y/data theta");
  }

  Executor ex(std::move(cm.graph));
  cm.bind(ex);

  // a, b, p[2], y0[2]
  const int64_t n = ex.n_params();
  expect("6 unconstrained parameters, got " + std::to_string(n), n == 6);
  if (n != 6) return 1;

  std::vector<double> q((size_t)n);
  for (int64_t i = 0; i < n; ++i) q[(size_t)i] = -0.3 + 0.11 * (double)i;

  std::vector<double> grad((size_t)n);
  for (int64_t i = 0; i < n; ++i) ex.params_data()[i] = q[(size_t)i];
  const double lp = ex.gradient(grad.data());
  expect("lp is finite", std::isfinite(lp));

  // ---- the value-only forward ------------------------------------------
  // ode_fwd is the only kernel that skips work under forward_value_only:
  // it solves the states without the coupled sensitivity system, leaving
  // ctx.scratch -- the jacobian ode_bwd reads -- unwritten. nuts.cpp and
  // estimate.cpp run exactly this sequence on every ODE model, so a
  // gradient taken after a value-only sweep must be the one taken before
  // it. The value itself agrees only to solver tolerance: the two solves
  // see different error estimates, which is deliberate and is what makes
  // the value path CmdStan's log_prob<double>.
  {
    // Taken at a shifted point, so a solve that silently wrote nothing
    // cannot pass on the previous sweep's leftovers in the arena.
    for (int64_t k = 0; k < n; ++k) ex.params_data()[k] = q[(size_t)k] + 0.05;
    const double lp_vo = ex.forward_value_only();
    const double lp_full = ex.forward();
    const double dev =
        std::fabs(lp_vo - lp_full) / std::max(1.0, std::fabs(lp_full));
    if (!(dev < 1e-5)) {
      ++failures;
      std::printf(
          "FAIL value-only lp differs from the coupled solve by "
          "%.3g relative\n",
          dev);
    }

    for (int64_t k = 0; k < n; ++k) ex.params_data()[k] = q[(size_t)k];
    std::vector<double> grad2((size_t)n);
    const double lp2 = ex.gradient(grad2.data());
    expect("lp after a value-only sweep is bitwise the one before", lp2 == lp);
    for (int64_t i = 0; i < n; ++i)
      expect("gradient " + std::to_string(i) +
                 " after a value-only sweep is bitwise the one before",
             grad2[(size_t)i] == grad[(size_t)i]);
  }

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
      w = std::max(
          w, std::fabs(o[k] - rk45[k]) / std::max(1e-8, std::fabs(rk45[k])));
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
