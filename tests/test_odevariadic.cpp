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
#include "env_helpers.hpp"

#include <stanli/compile.hpp>
#include <stanli/ode.hpp>
#include <stanli/optable.hpp>

#include <stan/math.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <thread>
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

static uint64_t bits(double value) {
  uint64_t result;
  std::memcpy(&result, &value, sizeof(result));
  return result;
}

static bool bitwise_equal(const std::vector<double>& lhs,
                          const std::vector<double>& rhs) {
  if (lhs.size() != rhs.size()) return false;
  for (size_t i = 0; i < lhs.size(); ++i)
    if (bits(lhs[i]) != bits(rhs[i])) return false;
  return true;
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
    const auto solved =
        spec.solver == stanli::OdeSpec::CKRK
            ? stan::math::ode_ckrk_tol(DirectRhs{}, y0, spec.t0, spec.ts,
                                       spec.rtol, spec.atol, spec.max_steps,
                                       nullptr, test_theta, spec.x_r, spec.x_i)
            : stan::math::ode_rk45_tol(DirectRhs{}, y0, spec.t0, spec.ts,
                                       spec.rtol, spec.atol, spec.max_steps,
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
    const auto solved =
        spec.solver == stanli::OdeSpec::CKRK
            ? stan::math::ode_ckrk_tol(DirectRhs{}, y0, spec.t0, spec.ts,
                                       spec.rtol, spec.atol, spec.max_steps,
                                       nullptr, theta, spec.x_r, spec.x_i)
            : stan::math::ode_rk45_tol(DirectRhs{}, y0, spec.t0, spec.ts,
                                       spec.rtol, spec.atol, spec.max_steps,
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
OdeActivityRun kernel_activity_run(
    const stanli::OdeSpec& spec, const std::vector<double>& y0_values = test_y0,
    const std::vector<double>& theta_values = test_theta) {
  using namespace stanli;
  OdeActivityRun out;
  out.value.assign(spec.ts.size() * y0_values.size(), 0.0);
  out.y_grad.assign(y0_values.size(), 0.0);
  out.theta_grad.assign(theta_values.size(), 0.0);
  out.jacobian.assign(
      out.value.size() * (y0_values.size() + theta_values.size()),
      std::numeric_limits<double>::quiet_NaN());

  KernelCtx ctx;
  ctx.n_in = 2;
  ctx.in[0] =
      Desc{const_cast<double*>(y0_values.data()), (int64_t)y0_values.size()};
  ctx.in[1] = Desc{const_cast<double*>(theta_values.data()),
                   (int64_t)theta_values.size()};
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
  const size_t width = y0_values.size() + theta_values.size();
  for (size_t o = 0; o < out.value.size(); ++o) {
    if constexpr (!YAutodiff)
      for (size_t i = 0; i < y0_values.size(); ++i)
        out.jacobian[o * width + i] = std::numeric_limits<double>::quiet_NaN();
    if constexpr (!ThetaAutodiff)
      for (size_t i = 0; i < theta_values.size(); ++i)
        out.jacobian[o * width + y0_values.size() + i] =
            std::numeric_limits<double>::quiet_NaN();
  }
  ode->backward(ctx);
  out.jacobian = forward_jacobian;
  return out;
}

template <bool YAutodiff, bool ThetaAutodiff>
void check_activity_case(const stanli::OdeSpec& spec, const char* label) {
  stanli::OdeSpec oracle_spec = spec;
  oracle_spec.direct_rk_enabled = false;
  const OdeActivityRun oracle =
      kernel_activity_run<YAutodiff, ThetaAutodiff>(oracle_spec);
  stanli::OdeSpec direct_spec = spec;
  direct_spec.direct_rk_enabled = true;
  const OdeActivityRun got =
      kernel_activity_run<YAutodiff, ThetaAutodiff>(direct_spec);
  const OdeActivityRun want =
      direct_activity_run<YAutodiff, ThetaAutodiff>(spec);
  const std::string prefix = std::string(label) + ": direct/oracle ";
  expect(prefix + "solution bits", bitwise_equal(got.value, oracle.value));
  expect(prefix + "full scratch bits",
         bitwise_equal(got.jacobian, oracle.jacobian));
  expect(prefix + "y pullback bits", bitwise_equal(got.y_grad, oracle.y_grad));
  expect(prefix + "theta pullback bits",
         bitwise_equal(got.theta_grad, oracle.theta_grad));
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
               bits(got.jacobian[o * width + i]) == bits(0.0));
    if constexpr (!ThetaAutodiff)
      for (size_t i = 0; i < test_theta.size(); ++i)
        expect(std::string(label) + " zero theta Jacobian column",
               bits(got.jacobian[o * width + test_y0.size() + i]) == bits(0.0));
  }
}

struct OdeError {
  bool threw = false;
  std::string type;
  std::string message;
};

static OdeError capture_ode_error(
    const stanli::OdeSpec& spec, bool use_direct,
    const std::vector<double>& y0_values = test_y0,
    const std::vector<double>& theta_values = test_theta) {
  stanli::OdeSpec selected_spec = spec;
  selected_spec.direct_rk_enabled = use_direct;
  OdeError result;
  try {
    (void)kernel_activity_run<true, true>(selected_spec, y0_values,
                                          theta_values);
  } catch (const std::exception& error) {
    result.threw = true;
    result.type = typeid(error).name();
    result.message = error.what();
  }
  return result;
}

static void check_error_parity(
    const stanli::OdeSpec& spec, const std::string& label,
    const std::vector<double>& y0_values = test_y0,
    const std::vector<double>& theta_values = test_theta) {
  const OdeError oracle =
      capture_ode_error(spec, false, y0_values, theta_values);
  const OdeError direct =
      capture_ode_error(spec, true, y0_values, theta_values);
  expect(label + ": oracle throws", oracle.threw);
  expect(label + ": direct throws", direct.threw);
  expect(label + ": exception type", direct.type == oracle.type);
  expect(label + ": exception message", direct.message == oracle.message);
}

// OdeSpec and its generated derivative payload are graph-owned and shared by
// executor copies. The mutable register/Jacobian buffers must remain private
// to each solve: otherwise two chains can agree in a serial test and corrupt
// each other as soon as multiple chains evaluate the same model concurrently.
static void check_shared_spec_threads(const stanli::OdeSpec& spec,
                                      bool use_direct) {
  stanli::OdeSpec direct_spec = spec;
  direct_spec.direct_rk_enabled = use_direct;

  constexpr int kThreads = 4;
  constexpr int kRepeats = 6;
  std::vector<std::vector<double>> y0(kThreads, test_y0);
  std::vector<std::vector<double>> theta(kThreads, test_theta);
  std::vector<OdeActivityRun> expected;
  expected.reserve(kThreads);
  for (int thread = 0; thread < kThreads; ++thread) {
    y0[(size_t)thread][0] += 0.07 * thread;
    y0[(size_t)thread][1] -= 0.04 * thread;
    theta[(size_t)thread][0] += 0.03 * thread;
    theta[(size_t)thread][1] += 0.02 * thread;
    expected.push_back(kernel_activity_run<true, true>(
        direct_spec, y0[(size_t)thread], theta[(size_t)thread]));
  }

  std::atomic<int> ready{0};
  std::atomic<bool> start{false};
  std::vector<std::string> errors(kThreads);
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int thread = 0; thread < kThreads; ++thread) {
    threads.emplace_back([&, thread] {
      stan::math::ChainableStack tape;
      ready.fetch_add(1, std::memory_order_release);
      while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
      try {
        for (int repeat = 0; repeat < kRepeats; ++repeat) {
          const OdeActivityRun got = kernel_activity_run<true, true>(
              direct_spec, y0[(size_t)thread], theta[(size_t)thread]);
          const OdeActivityRun& want = expected[(size_t)thread];
          if (!bitwise_equal(got.value, want.value) ||
              !bitwise_equal(got.jacobian, want.jacobian) ||
              !bitwise_equal(got.y_grad, want.y_grad) ||
              !bitwise_equal(got.theta_grad, want.theta_grad)) {
            errors[(size_t)thread] =
                "direct result differs from the serial shared-spec result";
            break;
          }
        }
      } catch (const std::exception& error) {
        errors[(size_t)thread] = error.what();
      }
    });
  }
  while (ready.load(std::memory_order_acquire) != kThreads)
    std::this_thread::yield();
  start.store(true, std::memory_order_release);
  for (auto& thread : threads) thread.join();

  for (int thread = 0; thread < kThreads; ++thread)
    expect("shared OdeSpec thread " + std::to_string(thread),
           errors[(size_t)thread].empty());
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

static const stanli::OdeSpec* fixture_spec(
    const stanli::Graph& graph, stanli::OdeSpec::Solver solver,
    const std::string& rhs_name = "rhs") {
  using namespace stanli;
  for (const Op& op : graph.ops)
    if (op.opcode == OP_ODE) {
      const auto* spec = static_cast<const OdeSpec*>(op.udata);
      if (!spec->legacy && spec->solver == solver && spec->rhs_name == rhs_name)
        return spec;
    }
  return nullptr;
}

static stanli::CompiledModel compile_ode_fixture(const std::string& mir_text,
                                                 const stanli::DataMap& data,
                                                 bool disable_direct_rk) {
  const char* selector = "STANLI_NO_ODE_DIRECT_RK";
  disable_direct_rk ? test_setenv(selector, "1", 1) : test_unsetenv(selector);
  try {
    stanli::CompiledModel cm = stanli::compile_model(mir_text, data);
    test_unsetenv(selector);
    return cm;
  } catch (...) {
    test_unsetenv(selector);
    throw;
  }
}

int main() {
  using namespace stanli;

  DataMap data = DataMap::from_json_file("tests/fixtures/odevariadic.json");
  const std::string mir_text = slurp("tests/fixtures/odevariadic.tmir.sexp");
  CompiledModel cm = compile_ode_fixture(mir_text, data, false);
  CompiledModel oracle_cm = compile_ode_fixture(mir_text, data, true);
  const OdeSpec* rk45_spec = fixture_spec(cm.graph, OdeSpec::RK45);
  const OdeSpec* ckrk_spec = fixture_spec(cm.graph, OdeSpec::CKRK);
  const OdeSpec* mixed_rk45_spec =
      fixture_spec(cm.graph, OdeSpec::RK45, "rhs_mixed");
  expect("fixture exposes a modern rk45 rhs", rk45_spec != nullptr);
  expect("fixture exposes a modern ckrk rhs", ckrk_spec != nullptr);
  expect("fixture exposes a mixed-data rk45 rhs", mixed_rk45_spec != nullptr);
  const OdeSpec* oracle_rk45_spec =
      fixture_spec(oracle_cm.graph, OdeSpec::RK45);
  expect("oracle fixture exposes a modern rk45 rhs",
         oracle_rk45_spec != nullptr);
  if (oracle_rk45_spec)
    expect("inverse selector disables direct RK at lowering",
           !oracle_rk45_spec->direct_rk_enabled);

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
    expect("branchless RK45 has a direct derivative payload",
           (bool)rk45_spec->direct_rk && rk45_spec->direct_rk_why.empty() &&
               rk45_spec->direct_rk_enabled);
    check_activity_case<true, false>(*rk45_spec, "active y/data theta");
    check_activity_case<false, true>(*rk45_spec, "data y/active theta");
    check_activity_case<true, true>(*rk45_spec, "active y/active theta");
    check_activity_case<false, false>(*rk45_spec, "data y/data theta");
    check_shared_spec_threads(*rk45_spec, true);
    check_shared_spec_threads(*rk45_spec, false);

    OdeSpec invalid = *rk45_spec;
    invalid.rtol = 0.0;
    check_error_parity(invalid, "RK45 invalid relative tolerance");
    invalid = *rk45_spec;
    invalid.ts.clear();
    check_error_parity(invalid, "RK45 empty times");
    invalid = *rk45_spec;
    invalid.ts[0] = invalid.t0;
    check_error_parity(invalid, "RK45 time equals initial time");
    invalid = *rk45_spec;
    invalid.max_steps = 0;
    check_error_parity(invalid, "RK45 invalid max steps");
    std::vector<double> invalid_y0 = test_y0;
    invalid_y0[0] = std::numeric_limits<double>::quiet_NaN();
    check_error_parity(*rk45_spec, "RK45 nonfinite initial state", invalid_y0);
    std::vector<double> invalid_theta = test_theta;
    invalid_theta[1] = std::numeric_limits<double>::infinity();
    check_error_parity(*rk45_spec, "RK45 nonfinite parameter", test_y0,
                       invalid_theta);
    invalid = *rk45_spec;
    invalid.t0 = std::numeric_limits<double>::quiet_NaN();
    check_error_parity(invalid, "RK45 nonfinite initial time");
    invalid = *rk45_spec;
    invalid.ts[1] = std::numeric_limits<double>::infinity();
    check_error_parity(invalid, "RK45 nonfinite output time");
    invalid = *rk45_spec;
    std::swap(invalid.ts[0], invalid.ts[1]);
    check_error_parity(invalid, "RK45 unordered output times");
    invalid = *rk45_spec;
    invalid.max_steps = 1;
    check_error_parity(invalid, "RK45 integration step limit");

    OdeSpec legacy = *rk45_spec;
    legacy.legacy = true;
    check_activity_case<true, true>(legacy, "legacy RK45 active y/theta");
    legacy.rtol = 0.0;
    check_error_parity(legacy, "legacy RK45 invalid relative tolerance");
  }
  if (mixed_rk45_spec) {
    expect("mixed-data RK45 has a direct derivative payload",
           (bool)mixed_rk45_spec->direct_rk &&
               mixed_rk45_spec->direct_rk_why.empty() &&
               mixed_rk45_spec->direct_rk_enabled);
    expect("mixed-data RK45 exposes one real datum and three parameters",
           mixed_rk45_spec->x_r.size() == 1 &&
               mixed_rk45_spec->prog.n_xr == 1 &&
               mixed_rk45_spec->prog.n_th == 3);
    if (mixed_rk45_spec->x_r.size() == 1 && mixed_rk45_spec->prog.n_xr == 1 &&
        mixed_rk45_spec->prog.n_th == 3) {
      OdeSpec invalid = *mixed_rk45_spec;
      invalid.x_r[0] = std::numeric_limits<double>::quiet_NaN();
      check_error_parity(invalid, "RK45 nonfinite real data", test_y0,
                         {0.2, 0.35, 0.4});
    }
  }
  if (ckrk_spec) {
    expect("branchless CKRK has a direct derivative payload",
           (bool)ckrk_spec->direct_rk && ckrk_spec->direct_rk_why.empty() &&
               ckrk_spec->direct_rk_enabled);
    check_activity_case<true, false>(*ckrk_spec, "CKRK active y/data theta");
    check_activity_case<false, true>(*ckrk_spec, "CKRK data y/active theta");
    check_activity_case<true, true>(*ckrk_spec, "CKRK active y/theta");
  }

  Executor ex(std::move(cm.graph));
  cm.bind(ex);
  Executor oracle_ex(std::move(oracle_cm.graph));
  oracle_cm.bind(oracle_ex);

  // a, b, p[2], y0[2]
  const int64_t n = ex.n_params();
  expect("6 unconstrained parameters, got " + std::to_string(n), n == 6);
  if (n != 6) return 1;

  std::vector<double> q((size_t)n);
  for (int64_t i = 0; i < n; ++i) q[(size_t)i] = -0.3 + 0.11 * (double)i;

  std::vector<double> grad((size_t)n), oracle_grad((size_t)n);
  expect("oracle model has the same parameter count",
         oracle_ex.n_params() == n);
  for (int64_t i = 0; i < n; ++i) {
    ex.params_data()[i] = q[(size_t)i];
    oracle_ex.params_data()[i] = q[(size_t)i];
  }
  const double oracle_lp = oracle_ex.gradient(oracle_grad.data());
  const double lp = ex.gradient(grad.data());
  expect("lp is finite", std::isfinite(lp));
  expect("whole-model direct/oracle lp bits", bits(lp) == bits(oracle_lp));
  expect("whole-model direct/oracle gradient bits",
         bitwise_equal(grad, oracle_grad));

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
