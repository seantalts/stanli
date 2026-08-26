// Legacy algebra_solver end to end and at the kernel seam.  This pins the
// easy-to-miss contract that the initial guess can select a root but receives
// no adjoint; only the y/theta vector is differentiated.  It also exercises
// the compiled MIR callback, its interpreter fallback, explicit tolerances,
// data-only y, and map_rect's zero-job early return from stanc3's mother
// model.
#include <stanli/algebra.hpp>
#include <stanli/compile.hpp>
#include <stanli/optable.hpp>
#include <stanli/wa_interp.hpp>

#include "env_helpers.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(const std::string& what, bool ok) {
  if (!ok) {
    ++failures;
    std::printf("FAIL %s\n", what.c_str());
  }
}

void expect_close(const std::string& what, double got, double want,
                  double rel = 2e-8) {
  const double scale = std::max(1.0, std::fabs(want));
  if (!(std::fabs(got - want) <= rel * scale)) {
    ++failures;
    std::printf("FAIL %s: got %.17g want %.17g\n", what.c_str(), got, want);
  }
}

std::string slurp(const std::string& path) {
  std::ifstream f(path);
  std::ostringstream out;
  out << f.rdbuf();
  return out.str();
}

struct KernelRun {
  std::vector<double> value{0.0, 0.0};
  std::vector<double> jacobian{0.0, 0.0, 0.0, 0.0};
  std::vector<double> guess_adj{11.0, 12.0};
  std::vector<double> theta_adj{0.0, 0.0};
};

KernelRun run_kernel(const stanli::AlgebraSpec& spec, bool active,
                     const std::vector<double>& guess,
                     const std::vector<double>& theta,
                     const std::vector<double>& weights) {
  using namespace stanli;
  KernelRun out;
  KernelCtx ctx;
  ctx.n_in = 2;
  ctx.in[0] = Desc{const_cast<double*>(guess.data()), (int64_t)guess.size()};
  ctx.in[1] = Desc{const_cast<double*>(theta.data()), (int64_t)theta.size()};
  ctx.out = Desc{out.value.data(), (int64_t)out.value.size()};
  ctx.variant = active ? 1u : 0u;
  ctx.scratch = out.jacobian.data();
  ctx.udata = &spec;
  ctx.in_adj[0] = Desc{out.guess_adj.data(), (int64_t)out.guess_adj.size()};
  ctx.in_adj[1] = Desc{out.theta_adj.data(), (int64_t)out.theta_adj.size()};
  ctx.out_adj_vec =
      Desc{const_cast<double*>(weights.data()), (int64_t)weights.size()};
  const Kernel* kernel = find_kernel(OP_ALGEBRA_SOLVER);
  expect("algebra kernel is registered", kernel != nullptr);
  if (!kernel) return out;
  kernel->forward(ctx);
  kernel->backward(ctx);
  return out;
}

void check_kernel(const stanli::AlgebraSpec& spec, const char* path) {
  const std::vector<double> guess{0.4, 1.2};
  const std::vector<double> theta{0.3, -0.2};
  const std::vector<double> weights{0.7, -0.2};
  const double x2 = std::exp(theta[1]);
  const double x1 = 6.0 - theta[0] * x2;
  const KernelRun got = run_kernel(spec, true, guess, theta, weights);
  expect_close(std::string(path) + " solution x1", got.value[0], x1);
  expect_close(std::string(path) + " solution x2", got.value[1], x2);
  expect_close(std::string(path) + " d/dtheta1", got.theta_adj[0],
               -weights[0] * x2);
  expect_close(std::string(path) + " d/dtheta2", got.theta_adj[1],
               (-weights[0] * theta[0] + weights[1]) * x2);
  expect(std::string(path) + " never changes guess adjoint 1",
         got.guess_adj[0] == 11.0);
  expect(std::string(path) + " never changes guess adjoint 2",
         got.guess_adj[1] == 12.0);

  const KernelRun inactive = run_kernel(spec, false, guess, theta, weights);
  expect_close(std::string(path) + " inactive solution x1", inactive.value[0],
               x1);
  expect_close(std::string(path) + " inactive solution x2", inactive.value[1],
               x2);
  expect(std::string(path) + " inactive Jacobian is zero",
         std::all_of(inactive.jacobian.begin(), inactive.jacobian.end(),
                     [](double x) { return x == 0.0; }));
  expect(std::string(path) + " inactive theta adjoint stays zero",
         inactive.theta_adj[0] == 0.0 && inactive.theta_adj[1] == 0.0);
}

}  // namespace

int main() {
  using namespace stanli;
  // Attach WaInterp even when this small fixture's write_array graph is
  // complete.  A real late truncation (mother's case) takes the same path:
  // the interpreter restarts generate_quantities at statement zero and must
  // recompute the algebra_solver transformed parameters before writing them.
  expect("force interpreted write_array environment",
         test_setenv("STANLI_WA_FORCE_INTERP", "1", 1) == 0);
  CompiledModel cm =
      compile_model(slurp("tests/fixtures/algebra.tmir.sexp"),
                    DataMap::from_json_file("tests/fixtures/algebra.json"));
  expect("clear interpreted write_array environment",
         test_unsetenv("STANLI_WA_FORCE_INTERP") == 0);

  int n_solver = 0, n_active = 0, n_inactive = 0, n_reject = 0;
  const AlgebraSpec* active_spec = nullptr;
  for (const Op& op : cm.graph.ops) {
    if (op.opcode == OP_REJECT) ++n_reject;
    if (op.opcode != OP_ALGEBRA_SOLVER) continue;
    ++n_solver;
    if (op.variant & 1u) {
      ++n_active;
      if (!active_spec) active_spec = static_cast<const AlgebraSpec*>(op.udata);
    } else {
      ++n_inactive;
    }
  }
  expect("three algebra_solver calls survive lowering", n_solver == 3);
  expect("two theta-active algebra_solver calls", n_active == 2);
  expect("one data-theta algebra_solver call", n_inactive == 1);
  expect("empty map_rect never lowers its rejecting UDF", n_reject == 0);
  expect("active algebra spec is retained", active_spec != nullptr);
  if (active_spec) {
    expect("algebraic MIR system compiles to a register program",
           active_spec->prog.ok);
    check_kernel(*active_spec, "compiled callback");

    AlgebraSpec fallback;
    fallback.adopt(*active_spec->funs());
    fallback.system_name = active_spec->system_name;
    fallback.x_r = active_spec->x_r;
    fallback.x_i = active_spec->x_i;
    fallback.relative_tolerance = active_spec->relative_tolerance;
    fallback.function_tolerance = active_spec->function_tolerance;
    fallback.max_num_steps = active_spec->max_num_steps;
    expect("fallback spec deliberately has no register program",
           !fallback.prog.ok);
    check_kernel(fallback, "interpreter callback");
  }

  Executor ex(std::move(cm.graph));
  cm.bind(ex);
  expect("guess and theta contribute four parameters", ex.n_params() == 4);
  const std::vector<double> q{0.4, 1.2, 0.3, -0.2};
  for (size_t i = 0; i < q.size(); ++i) ex.params_data()[i] = q[i];
  std::vector<double> grad(q.size(), 0.0);
  const double lp = ex.gradient(grad.data());

  const double x2 = std::exp(q[3]);
  const double x1 = 6.0 - q[2] * x2;
  const double data_x2 = std::exp(-0.1);
  const double data_x1 = 6.0 - 0.15 * data_x2;
  const double want_lp = x1 + 0.2 * x2 + 0.1 * (data_x1 + data_x2);
  expect_close("compiled model lp", lp, want_lp);
  expect("initial guess gradient 1 is exactly zero", grad[0] == 0.0);
  expect("initial guess gradient 2 is exactly zero", grad[1] == 0.0);
  expect_close("compiled model d/dtheta1", grad[2], -x2);
  expect_close("compiled model d/dtheta2", grad[3], (-q[2] + 0.2) * x2);

  // A value-only solve leaves the Jacobian scratch unused; the next full
  // gradient must refill it and reproduce the same answer.
  for (size_t i = 0; i < q.size(); ++i) ex.params_data()[i] = q[i] + 0.05;
  expect("value-only algebra solve is finite",
         std::isfinite(ex.forward_value_only()));
  for (size_t i = 0; i < q.size(); ++i) ex.params_data()[i] = q[i];
  std::vector<double> grad2(q.size(), 0.0);
  const double lp2 = ex.gradient(grad2.data());
  expect_close("lp after value-only solve", lp2, lp);
  for (size_t i = 0; i < grad.size(); ++i)
    expect_close("gradient after value-only solve " + std::to_string(i),
                 grad2[i], grad[i]);

  expect("algebra fixture retains interpreted write_array",
         cm.write_array && cm.write_array->interp);
  if (cm.write_array && cm.write_array->interp) {
    WaRng rng(1234);
    const std::vector<double> row =
        cm.write_array->interp->eval(cm.constrained_env(ex), rng);
    const std::vector<std::string> names =
        CompiledModel::csv_names(cm.write_array->interp->columns());
    const auto wa_value = [&](const std::string& name) {
      const auto it = std::find(names.begin(), names.end(), name);
      expect("interpreted write_array has " + name, it != names.end());
      return it == names.end()
                 ? NAN
                 : row.at(static_cast<size_t>(it - names.begin()));
    };
    expect_close("interpreted default solve x1", wa_value("z.1"), x1);
    expect_close("interpreted default solve x2", wa_value("z.2"), x2);
    expect_close("interpreted tolerance solve x1", wa_value("z_tol.1"), x1);
    expect_close("interpreted tolerance solve x2", wa_value("z_tol.2"), x2);
    expect_close("interpreted data solve x1", wa_value("z_data.1"), data_x1);
    expect_close("interpreted data solve x2", wa_value("z_data.2"), data_x2);
  }

  if (failures == 0) std::printf("test_algebra: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
