// Developer-only ceiling benchmark for direct coupled RK sensitivities.
//
// The production ODE path asks Stan Math to discover RHS Jacobians with a
// nested reverse-mode callback.  This tool keeps the same pinned RK45/CKRK
// implementation's Boost tableau, coupled-state error control, tolerances,
// step checker, and output observer, but presents a manually assembled
// all-double coupled system directly to integrate_times. It thereby measures
// the ceiling from removing nested autodiff and the ordinary-RHS adapter
// without changing the numerical solver.
//
// Two developer-only Jacobian providers are available:
//
//   generated  a double forward plus the existing generated register adjoint;
//   fvar       one branch-aware fvar<double> column replay per input.
//
// The generated provider is deliberately *not* the failed Phase 0 bridge: it
// constructs no precomputed-gradient callback nodes and is never installed in
// the runtime.  Generation happens against a copy, leaving OdeSpec::prog as
// the pristine value/oracle program.
//
// Usage:
//   bench_ode_ceiling MODEL.tmir.sexp DATA.json [options]
//
// Options:
//   --provider auto|generated|fvar   (default: auto)
//   --solver model|rk45|ckrk         (default: model)
//   --point 0|1|2                    deterministic unconstrained point
//   --iterations N                   solves per timing batch (default: 200)
//   --batches N                      alternating paired batches (default: 15)
//   --warmup-ms N                    warmup duration per arm (default: 200)
//   --diagnostic                     print perturbative component timers
#include <stan/math/fwd.hpp>

#include <stanli/compile.hpp>
#include <stanli/graph.hpp>
#include <stanli/island.hpp>
#include <stanli/ode.hpp>
#include <stanli/ode_prog.hpp>
#include <stanli/optable.hpp>
#include <stanli/program.hpp>

#include <stan/math.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace stanli {

// ODE RHS programs do not contain log-density instructions.  Supplying this
// developer-tool-only specialization lets the generic register interpreter be
// instantiated for fvar without adding an fvar density ABI to libstanli.  A
// surprising density remains an explicit refusal, not silently differentiated.
template <>
stan::math::fvar<double> program_density<stan::math::fvar<double>>(
    int, const stan::math::fvar<double>*) {
  throw std::logic_error("density instruction in ODE fvar replay");
}

}  // namespace stanli

namespace {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using stan::math::fvar;
using stan::math::var;
using stanli::AdjProgram;
using stanli::IslandProg;
using stanli::OdeSpec;
using stanli::Program;
using stanli::RhsProgram;

volatile double benchmark_sink = 0.0;

std::string slurp(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot open " + path);
  std::ostringstream out;
  out << in.rdbuf();
  if (in.bad()) throw std::runtime_error("cannot read " + path);
  return out.str();
}

uint64_t bits(double x) {
  uint64_t result;
  std::memcpy(&result, &x, sizeof(result));
  return result;
}

int positive_int(const std::string& text, const char* name) {
  char* end = nullptr;
  const long value = std::strtol(text.c_str(), &end, 10);
  if (text.empty() || !end || *end != '\0' || value <= 0 || value > 100000000L)
    throw std::runtime_error(std::string(name) + " must be a positive integer");
  return static_cast<int>(value);
}

int nonnegative_int(const std::string& text, const char* name) {
  char* end = nullptr;
  const long value = std::strtol(text.c_str(), &end, 10);
  if (text.empty() || !end || *end != '\0' || value < 0 || value > 2)
    throw std::runtime_error(std::string(name) + " must be 0, 1, or 2");
  return static_cast<int>(value);
}

int nonnegative_duration(const std::string& text, const char* name) {
  char* end = nullptr;
  const long value = std::strtol(text.c_str(), &end, 10);
  if (text.empty() || !end || *end != '\0' || value < 0 || value > 60000)
    throw std::runtime_error(std::string(name) +
                             " must be between 0 and 60000");
  return static_cast<int>(value);
}

double eval_point(int64_t i, int variant) {
  switch (variant) {
    case 1:
      return 0.02 * static_cast<double>((i % 5) - 2);
    case 2:
      return 0.0;
    default:
      return 0.1 + 0.05 * static_cast<double>(i % 7) -
             0.15 * static_cast<double>(i % 3);
  }
}

int64_t elapsed_ns(TimePoint begin) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
                                                              begin)
      .count();
}

struct Options {
  std::string mir;
  std::string data;
  std::string provider = "auto";
  std::string solver = "model";
  int point = 0;
  int iterations = 200;
  int batches = 15;
  int warmup_ms = 200;
  bool diagnostic = false;
  bool require_exact = false;
};

void usage() {
  std::fprintf(stderr,
               "usage: bench_ode_ceiling MODEL.tmir.sexp DATA.json "
               "[--provider auto|generated|fvar] [--solver model|rk45|ckrk] "
               "[--point 0|1|2] [--iterations N] [--batches N] "
               "[--warmup-ms N] [--diagnostic] [--require-exact]\n");
}

Options parse_options(int argc, char** argv) {
  if (argc == 2 &&
      (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) {
    usage();
    std::exit(0);
  }
  if (argc < 3) {
    usage();
    throw std::runtime_error("missing MIR or data path");
  }
  Options out;
  out.mir = argv[1];
  out.data = argv[2];
  for (int i = 3; i < argc; ++i) {
    const std::string arg = argv[i];
    auto value = [&](const char* flag) -> std::string {
      if (i + 1 >= argc)
        throw std::runtime_error(std::string(flag) + " needs a value");
      return argv[++i];
    };
    if (arg == "--provider")
      out.provider = value("--provider");
    else if (arg == "--solver")
      out.solver = value("--solver");
    else if (arg == "--point")
      out.point = nonnegative_int(value("--point"), "--point");
    else if (arg == "--iterations")
      out.iterations = positive_int(value("--iterations"), "--iterations");
    else if (arg == "--batches")
      out.batches = positive_int(value("--batches"), "--batches");
    else if (arg == "--warmup-ms")
      out.warmup_ms = nonnegative_duration(value("--warmup-ms"), "--warmup-ms");
    else if (arg == "--diagnostic")
      out.diagnostic = true;
    else if (arg == "--require-exact")
      out.require_exact = true;
    else if (arg == "--help" || arg == "-h") {
      usage();
      std::exit(0);
    } else {
      throw std::runtime_error("unknown option " + arg);
    }
  }
  if (out.provider != "auto" && out.provider != "generated" &&
      out.provider != "fvar")
    throw std::runtime_error("--provider must be auto, generated, or fvar");
  if (out.solver != "model" && out.solver != "rk45" && out.solver != "ckrk")
    throw std::runtime_error("--solver must be model, rk45, or ckrk");
  return out;
}

enum class RkSolver { RK45, CKRK };

const char* solver_name(RkSolver solver) {
  return solver == RkSolver::RK45 ? "rk45" : "ckrk";
}

const char* spec_solver_name(OdeSpec::Solver solver) {
  switch (solver) {
    case OdeSpec::RK45:
      return "rk45";
    case OdeSpec::BDF:
      return "bdf";
    case OdeSpec::ADAMS:
      return "adams";
    case OdeSpec::CKRK:
      return "ckrk";
  }
  return "unknown";
}

RkSolver select_solver(const Options& options, const OdeSpec& spec) {
  if (options.solver == "rk45") return RkSolver::RK45;
  if (options.solver == "ckrk") return RkSolver::CKRK;
  if (spec.solver == OdeSpec::RK45) return RkSolver::RK45;
  if (spec.solver == OdeSpec::CKRK) return RkSolver::CKRK;
  throw std::runtime_error("model's first ODE uses " +
                           std::string(spec_solver_name(spec.solver)) +
                           "; pass --solver rk45 or --solver ckrk for the "
                           "developer RK ceiling experiment");
}

struct Breakdown {
  uint64_t callbacks = 0;
  size_t accepted_steps = 0;
  int64_t callback_ns = 0;
  int64_t seed_ns = 0;
  int64_t body_ns = 0;
  int64_t output_ns = 0;
  int64_t jacobian_ns = 0;
  int64_t propagation_ns = 0;
  int64_t solver_ns = 0;
  int64_t final_ns = 0;
};

struct SolveResult {
  std::vector<double> values;
  std::vector<double> jacobian;
};

struct CompareResult {
  size_t total = 0;
  size_t bitwise = 0;
  double max_relative = 0.0;
  size_t worst = 0;
};

CompareResult compare(const std::vector<double>& got,
                      const std::vector<double>& want) {
  if (got.size() != want.size())
    throw std::runtime_error(
        "comparison size mismatch: " + std::to_string(got.size()) + " versus " +
        std::to_string(want.size()));
  CompareResult result;
  result.total = got.size();
  for (size_t i = 0; i < got.size(); ++i) {
    const bool exact = bits(got[i]) == bits(want[i]);
    if (exact) ++result.bitwise;
    if (!std::isfinite(got[i]) || !std::isfinite(want[i])) {
      result.max_relative = std::numeric_limits<double>::infinity();
      result.worst = i;
      continue;
    }
    if (exact) continue;
    double relative = std::numeric_limits<double>::infinity();
    const double scale =
        std::max({1e-12, std::fabs(got[i]), std::fabs(want[i])});
    relative = std::fabs(got[i] - want[i]) / scale;
    if (relative > result.max_relative) {
      result.max_relative = relative;
      result.worst = i;
    }
  }
  return result;
}

void print_compare(const char* label, const CompareResult& result) {
  std::printf("%s bitwise=%zu/%zu max_relative=%.9g worst=%zu\n", label,
              result.bitwise, result.total, result.max_relative, result.worst);
}

std::string structural_refusal(const Program& program) {
  std::vector<std::string> names;
  for (const auto& instruction : program.code) {
    const auto& spec = stanli::program_code_spec(instruction.code);
    if (!spec.has(stanli::kProgramNoAdjoint)) continue;
    if (std::find(names.begin(), names.end(), spec.name) == names.end())
      names.emplace_back(spec.name);
  }
  std::string result;
  for (const auto& name : names) {
    if (!result.empty()) result += ", ";
    result += name;
  }
  return result;
}

// The exact Phase 1 callback, copied into the developer target so the current
// solve can be timed without changing runtime/kernels/ode.cpp.
template <bool Diagnostic>
struct OracleRhs {
  const OdeSpec* spec;
  Breakdown* breakdown;
  mutable stanli::RhsWorkspace workspace;

  template <typename T_y, typename T_param>
  Eigen::Matrix<stan::return_type_t<T_y, T_param>, Eigen::Dynamic, 1>
  operator()(const double& t, const Eigen::Matrix<T_y, Eigen::Dynamic, 1>& y,
             std::ostream*, const std::vector<T_param>& theta,
             const std::vector<double>& x_r, const std::vector<int>&) const {
    using T = stan::return_type_t<T_y, T_param>;
    TimePoint callback_start;
    if constexpr (Diagnostic) {
      ++breakdown->callbacks;
      callback_start = Clock::now();
    }

    Eigen::Matrix<T, Eigen::Dynamic, 1> out(
        static_cast<Eigen::Index>(spec->prog.out_regs.size()));
    std::vector<T>& registers = workspace.get<T>();

    TimePoint phase;
    if constexpr (Diagnostic) phase = Clock::now();
    if (static_cast<int>(registers.size()) < spec->prog.n_regs)
      registers.resize(static_cast<size_t>(spec->prog.n_regs));
    for (int i = 0; i < spec->prog.n_y; ++i)
      registers[static_cast<size_t>(spec->prog.y0 + i)] = T(y(i));
    for (int i = 0; i < spec->prog.n_th; ++i)
      registers[static_cast<size_t>(spec->prog.th0 + i)] = T(theta[i]);
    // Retain Phase 1's promotion of an unread variadic placeholder.
    for (size_t i = static_cast<size_t>(spec->prog.n_th); i < theta.size();
         ++i) {
      [[maybe_unused]] const T promoted(theta[i]);
    }
    registers[static_cast<size_t>(spec->prog.t_reg)] = T(t);
    for (int i = 0; i < spec->prog.n_xr; ++i)
      registers[static_cast<size_t>(spec->prog.xr0 + i)] = T(x_r[i]);
    if constexpr (Diagnostic) {
      breakdown->seed_ns += elapsed_ns(phase);
      phase = Clock::now();
    }

    stanli::run_program(spec->prog, registers);
    if constexpr (Diagnostic) {
      breakdown->body_ns += elapsed_ns(phase);
      phase = Clock::now();
    }
    for (size_t i = 0; i < spec->prog.out_regs.size(); ++i)
      out(static_cast<Eigen::Index>(i)) =
          registers[static_cast<size_t>(spec->prog.out_regs[i])];
    if constexpr (Diagnostic) {
      breakdown->output_ns += elapsed_ns(phase);
      breakdown->callback_ns += elapsed_ns(callback_start);
    }
    return out;
  }
};

template <typename Rhs, typename T_y0, typename T_theta>
auto call_rk_solver(RkSolver solver, const OdeSpec& spec, const Rhs& rhs,
                    const Eigen::Matrix<T_y0, Eigen::Dynamic, 1>& y0,
                    const std::vector<T_theta>& theta) {
  if (solver == RkSolver::RK45) {
    return stan::math::ode_rk45_tol_impl(
        spec.legacy ? "integrate_ode_rk45" : "ode_rk45_tol", rhs, y0, spec.t0,
        spec.ts, spec.rtol, spec.atol, spec.max_steps, nullptr, theta, spec.x_r,
        spec.x_i);
  }
  return stan::math::ode_ckrk_tol_impl(
      "ode_ckrk_tol", rhs, y0, spec.t0, spec.ts, spec.rtol, spec.atol,
      spec.max_steps, nullptr, theta, spec.x_r, spec.x_i);
}

template <bool YAutodiff, bool ThetaAutodiff, bool Diagnostic>
SolveResult run_oracle_typed(RkSolver solver, const OdeSpec& spec,
                             const std::vector<double>& y0_values,
                             const std::vector<double>& theta_values,
                             Breakdown* breakdown) {
  using T_y0 = std::conditional_t<YAutodiff, var, double>;
  using T_theta = std::conditional_t<ThetaAutodiff, var, double>;
  const size_t S = y0_values.size();
  const size_t P = theta_values.size();
  const size_t W = S + P;
  SolveResult result;
  result.values.resize(spec.ts.size() * S);
  result.jacobian.assign(result.values.size() * W, 0.0);

  if constexpr (!YAutodiff && !ThetaAutodiff) {
    Eigen::Matrix<T_y0, Eigen::Dynamic, 1> y0(S);
    for (size_t i = 0; i < S; ++i)
      y0(static_cast<Eigen::Index>(i)) = y0_values[i];
    std::vector<T_theta> theta(theta_values.begin(), theta_values.end());
    OracleRhs<Diagnostic> rhs{&spec, breakdown};
    TimePoint phase;
    if constexpr (Diagnostic) phase = Clock::now();
    const auto solution = call_rk_solver(solver, spec, rhs, y0, theta);
    if constexpr (Diagnostic) {
      breakdown->solver_ns = elapsed_ns(phase);
      phase = Clock::now();
    }
    for (size_t n = 0; n < solution.size(); ++n)
      for (size_t i = 0; i < S; ++i)
        result.values[n * S + i] = solution[n](static_cast<Eigen::Index>(i));
    if constexpr (Diagnostic) breakdown->final_ns = elapsed_ns(phase);
    return result;
  } else {
    stan::math::nested_rev_autodiff nested;
    Eigen::Matrix<T_y0, Eigen::Dynamic, 1> y0(S);
    for (size_t i = 0; i < S; ++i)
      y0(static_cast<Eigen::Index>(i)) = y0_values[i];
    std::vector<T_theta> theta(theta_values.begin(), theta_values.end());
    OracleRhs<Diagnostic> rhs{&spec, breakdown};
    TimePoint phase;
    if constexpr (Diagnostic) phase = Clock::now();
    const auto solution = call_rk_solver(solver, spec, rhs, y0, theta);
    if constexpr (Diagnostic) {
      breakdown->solver_ns = elapsed_ns(phase);
      phase = Clock::now();
    }

    for (size_t n = 0; n < solution.size(); ++n)
      for (size_t i = 0; i < S; ++i)
        result.values[n * S + i] =
            solution[n](static_cast<Eigen::Index>(i)).val();

    stan::math::set_zero_all_adjoints_nested();
    for (size_t o = result.values.size(); o-- > 0;) {
      auto* output = solution[o / S](static_cast<Eigen::Index>(o % S)).vi_;
      output->adj_ = 1.0;
      output->chain();
      if constexpr (YAutodiff) {
        for (size_t i = 0; i < S; ++i) {
          result.jacobian[o * W + i] = y0(static_cast<Eigen::Index>(i)).adj();
          y0(static_cast<Eigen::Index>(i)).vi_->adj_ = 0.0;
        }
      }
      if constexpr (ThetaAutodiff) {
        for (size_t i = 0; i < P; ++i) {
          result.jacobian[o * W + S + i] = theta[i].adj();
          theta[i].vi_->adj_ = 0.0;
        }
      }
      output->adj_ = 0.0;
    }
    if constexpr (Diagnostic) breakdown->final_ns = elapsed_ns(phase);
    return result;
  }
}

template <bool Diagnostic>
SolveResult run_oracle(RkSolver solver, const OdeSpec& spec,
                       const std::vector<double>& y0,
                       const std::vector<double>& theta, uint8_t type_mask,
                       Breakdown* breakdown) {
  if (type_mask == 0x3u)
    return run_oracle_typed<true, true, Diagnostic>(solver, spec, y0, theta,
                                                    breakdown);
  if (type_mask == 0x1u)
    return run_oracle_typed<true, false, Diagnostic>(solver, spec, y0, theta,
                                                     breakdown);
  if (type_mask == 0x2u)
    return run_oracle_typed<false, true, Diagnostic>(solver, spec, y0, theta,
                                                     breakdown);
  return run_oracle_typed<false, false, Diagnostic>(solver, spec, y0, theta,
                                                    breakdown);
}

enum class ProviderKind { Generated, Fvar };

class DerivativeProvider {
 public:
  DerivativeProvider(const RhsProgram& rhs, size_t theta_source,
                     const std::string& requested)
      : rhs_(rhs), theta_source_(theta_source) {
    // Developer-only direct-coupled machinery.  The clone absorbs any
    // checkpoint MOVs; OdeSpec::prog remains canonical and untouched.
    static_cast<Program&>(generated_) = static_cast<const Program&>(rhs_);
    generated_.ins.push_back(IslandProg::LiveIn{rhs_.t_reg, 1, -1, 0, false});
    generated_.ins.push_back(
        IslandProg::LiveIn{rhs_.y0, rhs_.n_y, -1, 0, true});
    generated_.ins.push_back(
        IslandProg::LiveIn{rhs_.th0, rhs_.n_th, -1, 0, true});
    generated_.ins.push_back(
        IslandProg::LiveIn{rhs_.xr0, rhs_.n_xr, -1, 0, false});
    generated_ok_ = stanli::gen_adjoint(generated_);
    if (!generated_ok_) {
      refusal_ = structural_refusal(rhs_);
      if (refusal_.empty()) refusal_ = "adjoint generation failed";
    }

    if (requested == "generated") {
      if (!generated_ok_)
        throw std::runtime_error("generated provider refused: " + refusal_);
      kind_ = ProviderKind::Generated;
    } else if (requested == "fvar") {
      kind_ = ProviderKind::Fvar;
    } else {
      kind_ = generated_ok_ ? ProviderKind::Generated : ProviderKind::Fvar;
    }

    if (generated_ok_) {
      generated_values_.resize(static_cast<size_t>(generated_.n_regs));
      generated_adjoints_.resize(static_cast<size_t>(generated_.adj.n_regs));
    }
    primal_values_.resize(static_cast<size_t>(rhs_.n_regs));
    fvar_values_.resize(static_cast<size_t>(rhs_.n_regs));
  }

  ProviderKind kind() const { return kind_; }
  bool generated_ok() const { return generated_ok_; }
  const std::string& refusal() const { return refusal_; }

  const char* name() const {
    return kind_ == ProviderKind::Generated ? "generated" : "fvar";
  }

  size_t generated_forward_instructions() const {
    return generated_ok_ ? generated_.code.size() : 0;
  }

  size_t generated_reverse_instructions() const {
    return generated_ok_ ? generated_.adj.code.size() : 0;
  }

  template <bool Diagnostic>
  void evaluate(double t, const double* y, const std::vector<double>& theta,
                const double* x_r, bool theta_derivatives, double* f,
                double* J_y, double* J_theta, Breakdown* breakdown) {
    std::fill(J_y, J_y + static_cast<size_t>(rhs_.n_y * rhs_.n_y), 0.0);
    if (theta_source_ != 0)
      std::fill(J_theta,
                J_theta + static_cast<size_t>(rhs_.n_y) * theta_source_, 0.0);
    if (kind_ == ProviderKind::Generated) {
      evaluate_generated<Diagnostic>(t, y, theta, x_r, theta_derivatives, f,
                                     J_y, J_theta, breakdown);
    } else {
      evaluate_fvar<Diagnostic>(t, y, theta, x_r, theta_derivatives, f, J_y,
                                J_theta, breakdown);
    }
  }

  template <bool Diagnostic>
  void evaluate_values(double t, const double* y,
                       const std::vector<double>& theta, const double* x_r,
                       double* f, Breakdown* breakdown) {
    TimePoint phase;
    if constexpr (Diagnostic) phase = Clock::now();
    std::vector<double>* registers = &primal_values_;
    const Program* program = &rhs_;
    if (kind_ == ProviderKind::Generated) {
      std::fill(generated_values_.begin(), generated_values_.end(), 0.0);
      registers = &generated_values_;
      program = &generated_;
    }
    seed(*program, *registers, t, y, theta, x_r, -1);
    if constexpr (Diagnostic) {
      breakdown->seed_ns += elapsed_ns(phase);
      phase = Clock::now();
    }
    stanli::run_program(*program, *registers);
    if constexpr (Diagnostic) {
      breakdown->body_ns += elapsed_ns(phase);
      phase = Clock::now();
    }
    for (size_t i = 0; i < rhs_.out_regs.size(); ++i)
      f[i] = (*registers)[static_cast<size_t>(rhs_.out_regs[i])];
    if constexpr (Diagnostic) breakdown->output_ns += elapsed_ns(phase);
  }

 private:
  template <typename T>
  void seed(const Program& program, std::vector<T>& registers, double t,
            const double* y, const std::vector<double>& theta,
            const double* x_r, int derivative_column) {
    (void)program;
    for (int i = 0; i < rhs_.n_y; ++i) {
      if constexpr (std::is_same_v<T, fvar<double>>)
        registers[static_cast<size_t>(rhs_.y0 + i)] =
            T(y[i], derivative_column == i ? 1.0 : 0.0);
      else
        registers[static_cast<size_t>(rhs_.y0 + i)] = T(y[i]);
    }
    for (int i = 0; i < rhs_.n_th; ++i) {
      if constexpr (std::is_same_v<T, fvar<double>>)
        registers[static_cast<size_t>(rhs_.th0 + i)] =
            T(theta[static_cast<size_t>(i)],
              derivative_column == rhs_.n_y + i ? 1.0 : 0.0);
      else
        registers[static_cast<size_t>(rhs_.th0 + i)] =
            T(theta[static_cast<size_t>(i)]);
    }
    registers[static_cast<size_t>(rhs_.t_reg)] = T(t);
    for (int i = 0; i < rhs_.n_xr; ++i)
      registers[static_cast<size_t>(rhs_.xr0 + i)] = T(x_r[i]);
  }

  template <bool Diagnostic>
  void evaluate_generated(double t, const double* y,
                          const std::vector<double>& theta, const double* x_r,
                          bool theta_derivatives, double* f, double* J_y,
                          double* J_theta, Breakdown* breakdown) {
    TimePoint phase;
    if constexpr (Diagnostic) phase = Clock::now();
    std::fill(generated_values_.begin(), generated_values_.end(), 0.0);
    seed(generated_, generated_values_, t, y, theta, x_r, -1);
    if constexpr (Diagnostic) {
      breakdown->seed_ns += elapsed_ns(phase);
      phase = Clock::now();
    }
    stanli::run_program(static_cast<const Program&>(generated_),
                        generated_values_);
    if constexpr (Diagnostic) {
      breakdown->body_ns += elapsed_ns(phase);
      phase = Clock::now();
    }
    for (size_t i = 0; i < generated_.out_regs.size(); ++i)
      f[i] = generated_values_[static_cast<size_t>(generated_.out_regs[i])];
    if constexpr (Diagnostic) {
      breakdown->output_ns += elapsed_ns(phase);
      phase = Clock::now();
    }

    const AdjProgram& reverse = generated_.adj;
    for (size_t output = 0; output < generated_.out_regs.size(); ++output) {
      std::fill(generated_adjoints_.begin(), generated_adjoints_.end(), 0.0);
      const int output_reg = generated_.out_regs[output];
      generated_adjoints_[static_cast<size_t>(
          reverse.adj_reg[static_cast<size_t>(output_reg)])] += 1.0;
      stanli::run_adjoint(generated_, reverse, generated_values_.data(),
                          generated_adjoints_.data());
      for (int i = 0; i < rhs_.n_y; ++i) {
        const int reg = rhs_.y0 + i;
        J_y[output * static_cast<size_t>(rhs_.n_y) + static_cast<size_t>(i)] =
            generated_adjoints_[static_cast<size_t>(
                reverse.adj_reg[static_cast<size_t>(reg)])];
      }
      if (theta_derivatives) {
        for (int i = 0; i < rhs_.n_th; ++i) {
          const int reg = rhs_.th0 + i;
          J_theta[output * theta_source_ + static_cast<size_t>(i)] =
              generated_adjoints_[static_cast<size_t>(
                  reverse.adj_reg[static_cast<size_t>(reg)])];
        }
      }
    }
    if constexpr (Diagnostic) breakdown->jacobian_ns += elapsed_ns(phase);
  }

  template <bool Diagnostic>
  void evaluate_fvar(double t, const double* y,
                     const std::vector<double>& theta, const double* x_r,
                     bool theta_derivatives, double* f, double* J_y,
                     double* J_theta, Breakdown* breakdown) {
    TimePoint phase;
    if constexpr (Diagnostic) phase = Clock::now();
    seed(rhs_, primal_values_, t, y, theta, x_r, -1);
    if constexpr (Diagnostic) {
      breakdown->seed_ns += elapsed_ns(phase);
      phase = Clock::now();
    }
    stanli::run_program(rhs_, primal_values_);
    if constexpr (Diagnostic) {
      breakdown->body_ns += elapsed_ns(phase);
      phase = Clock::now();
    }
    for (size_t i = 0; i < rhs_.out_regs.size(); ++i)
      f[i] = primal_values_[static_cast<size_t>(rhs_.out_regs[i])];
    if constexpr (Diagnostic) {
      breakdown->output_ns += elapsed_ns(phase);
      phase = Clock::now();
    }

    const int columns = rhs_.n_y + (theta_derivatives ? rhs_.n_th : 0);
    for (int column = 0; column < columns; ++column) {
      seed(rhs_, fvar_values_, t, y, theta, x_r, column);
      stanli::run_program(rhs_, fvar_values_);
      for (size_t output = 0; output < rhs_.out_regs.size(); ++output) {
        const double derivative =
            fvar_values_[static_cast<size_t>(rhs_.out_regs[output])].d_;
        if (column < rhs_.n_y)
          J_y[output * static_cast<size_t>(rhs_.n_y) +
              static_cast<size_t>(column)] = derivative;
        else
          J_theta[output * theta_source_ +
                  static_cast<size_t>(column - rhs_.n_y)] = derivative;
      }
    }
    if constexpr (Diagnostic) breakdown->jacobian_ns += elapsed_ns(phase);
  }

  const RhsProgram& rhs_;
  size_t theta_source_;
  ProviderKind kind_ = ProviderKind::Fvar;
  IslandProg generated_;
  bool generated_ok_ = false;
  std::string refusal_;
  std::vector<double> generated_values_;
  std::vector<double> generated_adjoints_;
  std::vector<double> primal_values_;
  std::vector<fvar<double>> fvar_values_;
};

template <bool Diagnostic>
struct DoubleCoupledSystem {
  DerivativeProvider* provider;
  size_t S;
  size_t P;
  size_t N_y0;
  size_t N_theta;
  const std::vector<double>& theta;
  const std::vector<double>& x_r;
  Breakdown* breakdown;
  mutable std::vector<double> f;
  mutable std::vector<double> J_y;
  mutable std::vector<double> J_theta;

  DoubleCoupledSystem(DerivativeProvider* provider_in, size_t S_in, size_t P_in,
                      bool y0_active, bool theta_active,
                      const std::vector<double>& theta_in,
                      const std::vector<double>& x_r_in,
                      Breakdown* breakdown_in)
      : provider(provider_in),
        S(S_in),
        P(P_in),
        N_y0(y0_active ? S_in : 0),
        N_theta(theta_active ? P_in : 0),
        theta(theta_in),
        x_r(x_r_in),
        breakdown(breakdown_in),
        f(S_in),
        J_y(S_in * S_in),
        J_theta(S_in * P_in) {}

  void operator()(const std::vector<double>& z, std::vector<double>& dz,
                  double t) const {
    TimePoint callback_start;
    if constexpr (Diagnostic) {
      ++breakdown->callbacks;
      callback_start = Clock::now();
    }
    const size_t expected = S * (1 + N_y0 + N_theta);
    if (z.size() != expected)
      throw std::runtime_error("coupled RK state has unexpected size");

    dz.resize(expected);
    if (N_y0 == 0 && N_theta == 0) {
      provider->template evaluate_values<Diagnostic>(
          t, z.data(), theta, x_r.data(), f.data(), breakdown);
    } else {
      provider->template evaluate<Diagnostic>(
          t, z.data(), theta, x_r.data(), N_theta != 0, f.data(), J_y.data(),
          J_theta.data(), breakdown);
    }

    TimePoint phase;
    if constexpr (Diagnostic) phase = Clock::now();
    for (size_t i = 0; i < S; ++i) {
      dz[i] = f[i];
      for (size_t j = 0; j < N_y0; ++j) {
        double derivative = 0.0;
        for (size_t k = 0; k < S; ++k)
          derivative += z[S + S * j + k] * J_y[i * S + k];
        dz[S + S * j + i] = derivative;
      }
      for (size_t j = 0; j < N_theta; ++j) {
        double derivative = J_theta[i * P + j];
        for (size_t k = 0; k < S; ++k)
          derivative += z[S + S * N_y0 + S * j + k] * J_y[i * S + k];
        dz[S + S * N_y0 + S * j + i] = derivative;
      }
    }
    if constexpr (Diagnostic) {
      breakdown->propagation_ns += elapsed_ns(phase);
      breakdown->callback_ns += elapsed_ns(callback_start);
    }
  }
};

template <bool Diagnostic>
std::vector<std::vector<double>> integrate_direct_rk(
    RkSolver solver, const OdeSpec& spec,
    DoubleCoupledSystem<Diagnostic>* system, std::vector<double> initial,
    Breakdown* breakdown) {
  using boost::numeric::odeint::integrate_times;
  using boost::numeric::odeint::make_controlled;
  using boost::numeric::odeint::make_dense_output;
  using boost::numeric::odeint::max_step_checker;
  using boost::numeric::odeint::no_progress_error;
  using boost::numeric::odeint::runge_kutta_cash_karp54;
  using boost::numeric::odeint::runge_kutta_dopri5;

  std::vector<double> times(spec.ts.size() + 1);
  times[0] = spec.t0;
  std::copy(spec.ts.begin(), spec.ts.end(), times.begin() + 1);
  std::vector<std::vector<double>> solution;
  solution.reserve(spec.ts.size());
  bool initial_observed = false;
  auto observer = [&](const std::vector<double>& state, double) {
    if (!initial_observed) {
      initial_observed = true;
      return;
    }
    solution.push_back(state);
  };

  size_t accepted_steps = 0;
  try {
    if (solver == RkSolver::RK45) {
      accepted_steps = integrate_times(
          make_dense_output(spec.atol, spec.rtol,
                            runge_kutta_dopri5<std::vector<double>, double,
                                               std::vector<double>, double>()),
          std::ref(*system), initial, times.begin(), times.end(), 0.1, observer,
          max_step_checker(spec.max_steps));
    } else {
      accepted_steps = integrate_times(
          make_controlled(
              spec.atol, spec.rtol,
              runge_kutta_cash_karp54<std::vector<double>, double,
                                      std::vector<double>, double>()),
          std::ref(*system), initial, times.begin(), times.end(), 0.1, observer,
          max_step_checker(spec.max_steps));
    }
  } catch (const no_progress_error&) {
    throw std::domain_error("direct RK ceiling exceeded max_steps");
  }
  if constexpr (Diagnostic) breakdown->accepted_steps = accepted_steps;
  return solution;
}

template <bool Diagnostic>
SolveResult run_candidate(RkSolver solver, const OdeSpec& spec,
                          const std::vector<double>& y0,
                          const std::vector<double>& theta, uint8_t type_mask,
                          DerivativeProvider* provider, Breakdown* breakdown) {
  const size_t S = y0.size();
  const size_t P = theta.size();
  const size_t W = S + P;
  const bool y0_active = (type_mask & 0x1u) != 0;
  const bool theta_active = (type_mask & 0x2u) != 0;
  const size_t N_y0 = y0_active ? S : 0;
  const size_t N_theta = theta_active ? P : 0;
  const size_t coupled_size = S * (1 + N_y0 + N_theta);

  std::vector<double> initial(coupled_size, 0.0);
  for (size_t i = 0; i < S; ++i) initial[i] = y0[i];
  for (size_t i = 0; i < N_y0; ++i) initial[S + S * i + i] = 1.0;

  DoubleCoupledSystem<Diagnostic> rhs(provider, S, P, y0_active, theta_active,
                                      theta, spec.x_r, breakdown);
  TimePoint phase;
  if constexpr (Diagnostic) phase = Clock::now();
  const auto solution =
      integrate_direct_rk(solver, spec, &rhs, std::move(initial), breakdown);
  if constexpr (Diagnostic) {
    breakdown->solver_ns = elapsed_ns(phase);
    phase = Clock::now();
  }

  SolveResult result;
  result.values.resize(spec.ts.size() * S);
  result.jacobian.assign(result.values.size() * W, 0.0);
  for (size_t n = 0; n < solution.size(); ++n) {
    for (size_t i = 0; i < S; ++i) {
      const size_t output = n * S + i;
      result.values[output] = solution[n][i];
      if (y0_active)
        for (size_t j = 0; j < S; ++j)
          result.jacobian[output * W + j] = solution[n][S + S * j + i];
      if (theta_active)
        for (size_t j = 0; j < P; ++j)
          result.jacobian[output * W + S + j] =
              solution[n][S + S * N_y0 + S * j + i];
    }
  }
  if constexpr (Diagnostic) breakdown->final_ns = elapsed_ns(phase);
  return result;
}

struct LocalResult {
  std::vector<double> values;
  std::vector<double> jacobian;
};

LocalResult local_oracle(const RhsProgram& rhs, double t,
                         const std::vector<double>& y_values,
                         const std::vector<double>& theta_values,
                         const std::vector<double>& x_r) {
  stan::math::nested_rev_autodiff nested;
  std::vector<var> y;
  std::vector<var> theta;
  y.reserve(y_values.size());
  theta.reserve(theta_values.size());
  for (double value : y_values) y.emplace_back(value);
  for (double value : theta_values) theta.emplace_back(value);
  std::vector<var> outputs(rhs.out_regs.size());
  std::vector<var> rhs_registers;
  stanli::run_rhs_into<var>(rhs, t, y.data(), theta.data(), theta.size(),
                            x_r.data(), outputs.data(), rhs_registers);

  LocalResult result;
  result.values.resize(outputs.size());
  result.jacobian.resize(outputs.size() * (y.size() + theta.size()));
  for (size_t i = 0; i < outputs.size(); ++i)
    result.values[i] = outputs[i].val();
  for (size_t output = 0; output < outputs.size(); ++output) {
    stan::math::grad(outputs[output].vi_);
    const size_t width = y.size() + theta.size();
    for (size_t i = 0; i < y.size(); ++i)
      result.jacobian[output * width + i] = y[i].adj();
    for (size_t i = 0; i < theta.size(); ++i)
      result.jacobian[output * width + y.size() + i] = theta[i].adj();
    if (output + 1 < outputs.size()) nested.set_zero_all_adjoints();
  }
  return result;
}

LocalResult local_candidate(DerivativeProvider* provider, const RhsProgram& rhs,
                            double t, const std::vector<double>& y,
                            const std::vector<double>& theta,
                            const std::vector<double>& x_r) {
  LocalResult result;
  result.values.resize(static_cast<size_t>(rhs.n_y));
  std::vector<double> J_y(static_cast<size_t>(rhs.n_y * rhs.n_y));
  std::vector<double> J_theta(static_cast<size_t>(rhs.n_y) * theta.size());
  provider->evaluate<false>(t, y.data(), theta, x_r.data(), true,
                            result.values.data(), J_y.data(), J_theta.data(),
                            nullptr);
  result.jacobian.resize(static_cast<size_t>(rhs.n_y) *
                         (static_cast<size_t>(rhs.n_y) + theta.size()));
  for (int output = 0; output < rhs.n_y; ++output) {
    const size_t width = static_cast<size_t>(rhs.n_y) + theta.size();
    for (int i = 0; i < rhs.n_y; ++i)
      result.jacobian[static_cast<size_t>(output) * width +
                      static_cast<size_t>(i)] =
          J_y[static_cast<size_t>(output * rhs.n_y + i)];
    for (size_t i = 0; i < theta.size(); ++i)
      result.jacobian[static_cast<size_t>(output) * width +
                      static_cast<size_t>(rhs.n_y) + i] =
          J_theta[static_cast<size_t>(output) * theta.size() + i];
  }
  return result;
}

double checksum(const SolveResult& result) {
  double value = 0.0;
  if (!result.values.empty()) {
    value += result.values.front();
    value += 0.5 * result.values.back();
  }
  if (!result.jacobian.empty()) {
    value += 0.25 * result.jacobian.front();
    value += 0.125 * result.jacobian[result.jacobian.size() / 2];
    value += 0.0625 * result.jacobian.back();
  }
  return value;
}

template <typename F>
double time_solves(int iterations, F&& solve) {
  double sink = 0.0;
  const auto begin = Clock::now();
  for (int i = 0; i < iterations; ++i) sink += checksum(solve());
  const auto end = Clock::now();
  benchmark_sink += sink;
  return std::chrono::duration<double, std::nano>(end - begin).count() /
         static_cast<double>(iterations);
}

double median(std::vector<double> values) {
  std::sort(values.begin(), values.end());
  const size_t middle = values.size() / 2;
  if (values.size() % 2 != 0) return values[middle];
  return 0.5 * (values[middle - 1] + values[middle]);
}

void print_breakdown(const char* label, const Breakdown& stats) {
  const double callbacks =
      static_cast<double>(std::max<uint64_t>(1, stats.callbacks));
  std::printf(
      "%s diagnostic callbacks=%llu solver=%.3f us final=%.3f us "
      "callback=%.3f ns/cb seed=%.3f body=%.3f output=%.3f "
      "jacobian=%.3f propagation=%.3f ns/cb\n",
      label, static_cast<unsigned long long>(stats.callbacks),
      stats.solver_ns / 1000.0, stats.final_ns / 1000.0,
      stats.callback_ns / callbacks, stats.seed_ns / callbacks,
      stats.body_ns / callbacks, stats.output_ns / callbacks,
      stats.jacobian_ns / callbacks, stats.propagation_ns / callbacks);
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const Options options = parse_options(argc, argv);
    const stanli::DataMap data =
        stanli::DataMap::from_json(slurp(options.data));
    stanli::CompiledModel model =
        stanli::compile_model(slurp(options.mir), data);
    stanli::Executor executor(std::move(model.graph));
    model.bind(executor);
    for (int64_t i = 0; i < executor.n_params(); ++i)
      executor.params_data()[i] = eval_point(i, options.point);

    // One ordinary evaluation materializes transformed ODE inputs in their
    // bound slots.  It is preparation, outside every solve timing below.
    std::vector<double> model_gradient(
        static_cast<size_t>(executor.n_params()));
    const double model_lp = executor.gradient(model_gradient.data());

    const stanli::Op* ode_op = nullptr;
    for (const auto& op : executor.graph().ops) {
      if (op.opcode == stanli::OP_ODE) {
        ode_op = &op;
        break;
      }
    }
    if (!ode_op) throw std::runtime_error("compiled model has no OP_ODE");
    const auto* spec = static_cast<const OdeSpec*>(ode_op->udata);
    if (!spec) throw std::runtime_error("OP_ODE has no OdeSpec");
    if (!spec->prog.ok)
      throw std::runtime_error("first ODE uses the interpreter fallback: " +
                               spec->prog.why);

    const auto& graph = executor.graph();
    const size_t S = static_cast<size_t>(graph.slots[ode_op->in[0]].len);
    const size_t P = static_cast<size_t>(graph.slots[ode_op->in[1]].len);
    const size_t output_size =
        static_cast<size_t>(graph.slots[ode_op->out].len);
    if (S == 0 || output_size % S != 0 || output_size / S != spec->ts.size())
      throw std::runtime_error("OP_ODE shape does not match OdeSpec times");
    if (spec->prog.out_regs.size() != S)
      throw std::runtime_error(
          "compiled RHS output width does not match state");
    const double* y0_data = executor.value_ptr(ode_op->in[0]);
    const double* theta_data = executor.value_ptr(ode_op->in[1]);
    const std::vector<double> y0(y0_data, y0_data + S);
    const std::vector<double> theta(theta_data, theta_data + P);
    const uint8_t type_mask =
        (ode_op->variant & 0x4u) != 0 ? (ode_op->variant & 0x3u) : 0x3u;
    const RkSolver solver = select_solver(options, *spec);

    DerivativeProvider provider(spec->prog, P, options.provider);
    const size_t N_y0 = (type_mask & 0x1u) != 0 ? S : 0;
    const size_t N_theta = (type_mask & 0x2u) != 0 ? P : 0;
    const size_t coupled_size = S * (1 + N_y0 + N_theta);

    std::printf(
        "model_lp=%.17g rhs=%s original_solver=%s benchmark_solver=%s "
        "legacy=%d point=%d\n",
        model_lp, spec->rhs_name.c_str(), spec_solver_name(spec->solver),
        solver_name(solver), spec->legacy ? 1 : 0, options.point);
    std::printf(
        "shape states=%zu theta_source=%zu outputs=%zu times=%zu "
        "type_mask=0x%x coupled=%zu rhs_instructions=%zu rhs_registers=%d\n",
        S, P, output_size, spec->ts.size(), static_cast<unsigned>(type_mask),
        coupled_size, spec->prog.code.size(), spec->prog.n_regs);
    std::printf(
        "provider requested=%s selected=%s generated_eligible=%d "
        "generated_forward=%zu generated_reverse=%zu",
        options.provider.c_str(), provider.name(),
        provider.generated_ok() ? 1 : 0,
        provider.generated_forward_instructions(),
        provider.generated_reverse_instructions());
    if (!provider.generated_ok())
      std::printf(" refusal=\"%s\"", provider.refusal().c_str());
    std::printf("\n");

    // Check both sides of time-dependent branches when the first observation
    // lies after t0, plus a late point.  The state is held fixed so this is a
    // local RHS/Jacobian proof rather than another numerical integration.
    std::vector<double> local_times{spec->t0};
    if (!spec->ts.empty()) {
      local_times.push_back(spec->t0 + 0.5 * (spec->ts.front() - spec->t0));
      if (bits(spec->ts.back()) != bits(local_times.back()))
        local_times.push_back(spec->ts.back());
    }
    bool local_exact = true;
    bool local_numerical = true;
    for (size_t k = 0; k < local_times.size(); ++k) {
      const LocalResult old_local =
          local_oracle(spec->prog, local_times[k], y0, theta, spec->x_r);
      const LocalResult new_local = local_candidate(
          &provider, spec->prog, local_times[k], y0, theta, spec->x_r);
      const std::string value_label = "local[" + std::to_string(k) + "].values";
      const std::string jacobian_label =
          "local[" + std::to_string(k) + "].jacobian";
      const CompareResult value_comparison =
          compare(new_local.values, old_local.values);
      const CompareResult jacobian_comparison =
          compare(new_local.jacobian, old_local.jacobian);
      std::printf("local[%zu] t=%.17g\n", k, local_times[k]);
      print_compare(value_label.c_str(), value_comparison);
      print_compare(jacobian_label.c_str(), jacobian_comparison);
      local_exact &= value_comparison.bitwise == value_comparison.total &&
                     jacobian_comparison.bitwise == jacobian_comparison.total;
      local_numerical &= value_comparison.max_relative <= 1e-9 &&
                         jacobian_comparison.max_relative <= 1e-9;
    }

    Breakdown old_diagnostic;
    Breakdown new_diagnostic;
    const SolveResult old_result =
        run_oracle<true>(solver, *spec, y0, theta, type_mask, &old_diagnostic);
    const SolveResult new_result = run_candidate<true>(
        solver, *spec, y0, theta, type_mask, &provider, &new_diagnostic);
    std::printf("callbacks oracle=%llu candidate=%llu equal=%d\n",
                static_cast<unsigned long long>(old_diagnostic.callbacks),
                static_cast<unsigned long long>(new_diagnostic.callbacks),
                old_diagnostic.callbacks == new_diagnostic.callbacks ? 1 : 0);
    // Both pinned tableaux use six RHS evaluations per attempt. Cash--Karp
    // has no FSAL initialization calls; Dopri5's dense wrapper evaluates the
    // initial derivative and may reinitialize for the exact final endpoint.
    // Keep this explicitly labelled as a stage-count derivation.
    const size_t initialization_rhs =
        solver == RkSolver::RK45 ? new_diagnostic.callbacks % 6u : 0u;
    const size_t attempted_steps =
        (new_diagnostic.callbacks - initialization_rhs) / 6u;
    if (attempted_steps < new_diagnostic.accepted_steps)
      throw std::runtime_error("derived RK attempt count is inconsistent");
    std::printf(
        "steps candidate_accepted=%zu attempted_derived=%zu "
        "rejected_derived=%zu initialization_rhs=%zu\n",
        new_diagnostic.accepted_steps, attempted_steps,
        attempted_steps - new_diagnostic.accepted_steps, initialization_rhs);
    const CompareResult solution_values =
        compare(new_result.values, old_result.values);
    const CompareResult solution_jacobian =
        compare(new_result.jacobian, old_result.jacobian);
    print_compare("solution.values", solution_values);
    print_compare("solution.jacobian", solution_jacobian);
    const bool callback_equal =
        old_diagnostic.callbacks == new_diagnostic.callbacks;
    const bool solution_exact =
        solution_values.bitwise == solution_values.total &&
        solution_jacobian.bitwise == solution_jacobian.total;
    const bool solution_numerical = solution_values.max_relative <= 1e-9 &&
                                    solution_jacobian.max_relative <= 1e-9;
    const bool exact_required =
        options.require_exact || provider.kind() == ProviderKind::Generated;
    if (!callback_equal || !local_numerical || !solution_numerical ||
        (exact_required && (!local_exact || !solution_exact)))
      throw std::runtime_error(
          "candidate failed the pre-timing correctness gate");
    std::printf("timing_gate=%s pass=1 work_parity=callback_count\n",
                exact_required ? "exact" : "proximity");
    if (options.diagnostic) {
      print_breakdown("oracle", old_diagnostic);
      print_breakdown("candidate", new_diagnostic);
      std::printf(
          "diagnostic note: component clocks perturb callbacks; the paired "
          "timings below instantiate clock-free solve arms\n");
    }

    // Warm the clock-free template instantiations independently so each arm
    // reaches the requested duration even when their costs differ greatly.
    const int64_t warmup_target_ns =
        static_cast<int64_t>(options.warmup_ms) * 1000000;
    int64_t old_warmup_ns = 0, new_warmup_ns = 0;
    size_t old_warmup_solves = 0, new_warmup_solves = 0;
    while (old_warmup_ns < warmup_target_ns ||
           new_warmup_ns < warmup_target_ns) {
      if (old_warmup_ns < warmup_target_ns) {
        const TimePoint begin = Clock::now();
        benchmark_sink += checksum(
            run_oracle<false>(solver, *spec, y0, theta, type_mask, nullptr));
        old_warmup_ns += elapsed_ns(begin);
        ++old_warmup_solves;
      }
      if (new_warmup_ns < warmup_target_ns) {
        const TimePoint begin = Clock::now();
        benchmark_sink += checksum(run_candidate<false>(
            solver, *spec, y0, theta, type_mask, &provider, nullptr));
        new_warmup_ns += elapsed_ns(begin);
        ++new_warmup_solves;
      }
    }
    std::printf(
        "warmup oracle_ms=%.3f candidate_ms=%.3f oracle_solves=%zu "
        "candidate_solves=%zu\n",
        old_warmup_ns / 1e6, new_warmup_ns / 1e6, old_warmup_solves,
        new_warmup_solves);

    std::vector<double> old_ns;
    std::vector<double> new_ns;
    std::vector<double> paired;
    old_ns.reserve(static_cast<size_t>(options.batches));
    new_ns.reserve(static_cast<size_t>(options.batches));
    paired.reserve(static_cast<size_t>(options.batches));
    for (int batch = 0; batch < options.batches; ++batch) {
      double old_time;
      double new_time;
      if (batch % 2 == 0) {
        old_time = time_solves(options.iterations, [&] {
          return run_oracle<false>(solver, *spec, y0, theta, type_mask,
                                   nullptr);
        });
        new_time = time_solves(options.iterations, [&] {
          return run_candidate<false>(solver, *spec, y0, theta, type_mask,
                                      &provider, nullptr);
        });
      } else {
        new_time = time_solves(options.iterations, [&] {
          return run_candidate<false>(solver, *spec, y0, theta, type_mask,
                                      &provider, nullptr);
        });
        old_time = time_solves(options.iterations, [&] {
          return run_oracle<false>(solver, *spec, y0, theta, type_mask,
                                   nullptr);
        });
      }
      old_ns.push_back(old_time);
      new_ns.push_back(new_time);
      paired.push_back(old_time / new_time);
      std::printf("batch=%d oracle_ns=%.1f candidate_ns=%.1f speedup=%.6fx\n",
                  batch + 1, old_time, new_time, old_time / new_time);
    }
    const auto [ratio_min, ratio_max] =
        std::minmax_element(paired.begin(), paired.end());
    std::printf(
        "median oracle_ns=%.1f candidate_ns=%.1f paired_speedup=%.6fx "
        "range=[%.6fx,%.6fx] iterations=%d batches=%d sink=%.9g\n",
        median(old_ns), median(new_ns), median(paired), *ratio_min, *ratio_max,
        options.iterations, options.batches,
        static_cast<double>(benchmark_sink));
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "bench_ode_ceiling: %s\n", error.what());
    return 1;
  }
}
