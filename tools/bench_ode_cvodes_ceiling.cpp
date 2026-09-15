// Developer-only ceiling experiment for the CVODES ODE paths.
//
// This is an uninstalled developer benchmark, not a test or runtime hook. It
// keeps the merged PR #245 Stan Math solve as the oracle, then compares it with
// a local CVODES driver whose primal, state-Jacobian, and sensitivity callbacks
// evaluate an RhsProgram without reverse-mode autodiff. The experimental
// Jacobian uses one stackless fvar<double> replay per input direction,
// including the canonical program's JZ/JMP control flow.
//
// The fvar arm is a ceiling/proximity experiment, not an exact replacement:
// forward and reverse accumulation group some derivatives differently.  In
// particular one_comp_mm_elim_abs has normal finite points where the two local
// Jacobians differ by two ulp.  The program therefore always reports exact-bit
// counts and numerical deltas against the current solve; it never silently
// promotes the experimental path to an oracle.
//
// A representative manual build (from the repository root) is:
//
//   c++ -O3 -DNDEBUG -std=c++17 -ffp-contract=off \
//     -DBOOST_DISABLE_ASSERTS -DBRIDGESTAN_EXPORT=1 -DSTAN_THREADS=1 \
//     -D_REENTRANT -Iruntime/include -Ideps/math -Ideps/stan/src \
//     -Ideps/math/lib/eigen_5.0.1 -Ideps/math/lib/boost_1.87.0 \
//     -Ideps/math/lib/sundials_6.1.1/include \
//     -Ideps/math/lib/tbb_2020.3/include \
//     tools/bench_ode_cvodes_ceiling.cpp tests/tbb_stub.cpp \
//     build-rel/libstanli.a build-rel/libsundials.a \
//     -o build-rel/bench_ode_cvodes_ceiling
//
// Include fwd before the register-program header: run_program's qualified Stan
// Math calls need the fvar overloads in their overload set at definition time.
#include <stan/math/fwd.hpp>

#include <stanli/compile.hpp>
#include <stanli/graph.hpp>
#include <stanli/ode.hpp>
#include <stanli/ode_prog.hpp>
#include <stanli/optable.hpp>
#include <stanli/program.hpp>

#include <stan/math.hpp>
#include <stan/math/rev/functor/cvodes_utils.hpp>

#include <cvodes/cvodes.h>
#include <cvodes/cvodes_ls.h>
#include <nvector/nvector_serial.h>
#include <sunlinsol/sunlinsol_dense.h>

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

// RhsPrograms never contain densities.  run_program<T>'s closed switch still
// references program_density<T>, while the shared library explicitly
// instantiates only double and var.  Supply a benchmark-local rejecting
// specialization so fvar replay links without compiling the complete density
// catalogue for a path that structurally refuses it.
namespace stanli {
template <>
stan::math::fvar<double> program_density<stan::math::fvar<double>>(
    int, const stan::math::fvar<double>*) {
  throw std::logic_error("density instruction in ODE RHS tangent replay");
}
}  // namespace stanli

namespace {

using Clock = std::chrono::steady_clock;
using Fvar = stan::math::fvar<double>;
using stan::math::cvodes_check;
using stan::math::var;
using stanli::OdeSpec;
using stanli::Program;
using stanli::RhsProgram;

volatile double benchmark_sink = 0.0;

int64_t elapsed_ns(Clock::time_point begin) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
                                                              begin)
      .count();
}

int64_t duration_ns(Clock::time_point begin, Clock::time_point end) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin)
      .count();
}

std::string slurp(const std::string& path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("cannot open " + path);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

int integer_arg(const char* text, const char* name, int minimum = 0) {
  char* end = nullptr;
  const long value = std::strtol(text, &end, 10);
  if (!text[0] || !end || *end != '\0' || value < minimum || value > 100000000L)
    throw std::runtime_error(std::string(name) + " is out of range");
  return static_cast<int>(value);
}

struct Options {
  std::string mir;
  std::string data;
  std::string solver = "model";
  int point = 0;
  int iterations = 200;
  int batches = 9;
  int warmup_ms = 200;
  bool require_exact = false;
};

void usage() {
  std::fprintf(stderr,
               "usage: bench_ode_cvodes_ceiling MODEL.tmir.sexp DATA.json "
               "[--solver model|bdf|adams] [--point 0|1|2] "
               "[--iterations N] [--batches N] [--warmup-ms N] "
               "[--require-exact]\n");
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
    const std::string flag = argv[i];
    auto value = [&](const char* name) -> const char* {
      if (i + 1 >= argc)
        throw std::runtime_error(std::string(name) + " needs a value");
      return argv[++i];
    };
    if (flag == "--solver") {
      out.solver = value("--solver");
    } else if (flag == "--point") {
      out.point = integer_arg(value("--point"), "--point");
      if (out.point > 2) throw std::runtime_error("--point must be 0, 1, or 2");
    } else if (flag == "--iterations") {
      out.iterations = integer_arg(value("--iterations"), "--iterations", 1);
    } else if (flag == "--batches") {
      out.batches = integer_arg(value("--batches"), "--batches", 1);
    } else if (flag == "--warmup-ms") {
      out.warmup_ms = integer_arg(value("--warmup-ms"), "--warmup-ms");
    } else if (flag == "--require-exact") {
      out.require_exact = true;
    } else if (flag == "--help" || flag == "-h") {
      usage();
      std::exit(0);
    } else {
      throw std::runtime_error("unknown option " + flag);
    }
  }
  if (out.solver != "model" && out.solver != "bdf" && out.solver != "adams")
    throw std::runtime_error("--solver must be model, bdf, or adams");
  return out;
}

double eval_point(int64_t index, int point) {
  switch (point) {
    case 1:
      return 0.02 * static_cast<double>((index % 5) - 2);
    case 2:
      return 0.0;
    default:
      return 0.1 + 0.05 * static_cast<double>(index % 7) -
             0.15 * static_cast<double>(index % 3);
  }
}

const char* solver_name(OdeSpec::Solver solver) {
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

OdeSpec::Solver select_solver(const Options& options, const OdeSpec& spec) {
  if (options.solver == "bdf") return OdeSpec::BDF;
  if (options.solver == "adams") return OdeSpec::ADAMS;
  if (spec.solver == OdeSpec::BDF || spec.solver == OdeSpec::ADAMS)
    return spec.solver;
  throw std::runtime_error("model's first ODE uses " +
                           std::string(solver_name(spec.solver)) +
                           "; pass --solver bdf or --solver adams for the "
                           "developer CVODES ceiling experiment");
}

uint64_t bits(double value) {
  uint64_t out;
  std::memcpy(&out, &value, sizeof(out));
  return out;
}

uint64_t ordered_bits(double value) {
  const uint64_t raw = bits(value);
  return (raw >> 63) ? ~raw : (raw | (uint64_t{1} << 63));
}

uint64_t ulp_distance(double a, double b) {
  const uint64_t x = ordered_bits(a), y = ordered_bits(b);
  return x > y ? x - y : y - x;
}

double median(std::vector<double> values) {
  if (values.empty()) return 0.0;
  std::sort(values.begin(), values.end());
  const size_t middle = values.size() / 2;
  if (values.size() & 1u) return values[middle];
  return 0.5 * (values[middle - 1] + values[middle]);
}

struct CallbackProfile {
  int64_t value_calls = 0;
  int64_t jacobian_calls = 0;
  int64_t full_jacobian_calls = 0;
  int64_t tangent_directions = 0;
  int64_t seed_ns = 0;
  int64_t body_ns = 0;
  int64_t extract_ns = 0;
  int64_t value_callback_ns = 0;
  int64_t jacobian_callback_ns = 0;
  int64_t sensitivity_callback_ns = 0;
  int64_t sensitivity_product_ns = 0;
};

struct CvodesStats {
  long int steps = 0;
  long int rhs_evals = 0;
  long int linear_setups = 0;
  long int error_test_fails = 0;
  long int nonlinear_iters = 0;
  long int nonlinear_fails = 0;
  long int jacobian_evals = 0;
  long int linear_rhs_evals = 0;
  long int linear_iters = 0;
  long int linear_fails = 0;
  long int sens_rhs_evals = 0;
  long int sens_extra_rhs_evals = 0;
  long int sens_error_test_fails = 0;
  long int sens_linear_setups = 0;
  long int sens_nonlinear_iters = 0;
  long int sens_nonlinear_fails = 0;
};

struct SolveProfile {
  int64_t total_ns = 0;
  int64_t allocation_ns = 0;
  int64_t setup_ns = 0;
  int64_t integrate_ns = 0;
  int64_t get_sens_ns = 0;
  int64_t output_ns = 0;
  int64_t final_harvest_ns = 0;
  int64_t oracle_double_callbacks = 0;
  int64_t oracle_var_callbacks = 0;
  int64_t oracle_double_callback_ns = 0;
  int64_t oracle_var_callback_ns = 0;
  CallbackProfile callback;
  CvodesStats cvodes;
};

struct SolveResult {
  std::vector<double> values;
  // Row-major by flattened solution output, then [y0, theta].
  std::vector<double> jacobian;
  SolveProfile profile;
};

struct BenchCase {
  OdeSpec ode;
  std::vector<double> y0;
  std::vector<double> theta;
};

BenchCase materialize_case(const OdeSpec& spec, const double* y0, size_t n_y,
                           const double* theta, size_t n_theta,
                           OdeSpec::Solver solver) {
  BenchCase out;
  out.ode = spec;
  // OdeSpec's default copy leaves the non-owning lookup map pointing into the
  // source.  The compiled path below does not consult it, but repair it so the
  // benchmark copy is independently well-formed.
  out.ode.funs_map.clear();
  for (const auto& [name, definition] : out.ode.owned)
    out.ode.funs_map[name] = &definition;
  out.ode.solver = solver;
  out.y0.assign(y0, y0 + n_y);
  out.theta.assign(theta, theta + n_theta);

  if (!out.ode.prog.ok)
    throw std::runtime_error("first ODE uses the interpreter fallback: " +
                             out.ode.prog.why);
  if (out.ode.prog.n_y != static_cast<int>(n_y))
    throw std::runtime_error("OP_ODE state width disagrees with RHS program");
  if (out.ode.prog.n_th < 0 || out.ode.prog.n_th > static_cast<int>(n_theta))
    throw std::runtime_error(
        "OP_ODE theta source is narrower than the RHS program input");
  for (const Program::Instr& instruction : out.ode.prog.code) {
    if (instruction.code == Program::CALL ||
        instruction.code == Program::DENSITY)
      throw std::runtime_error(
          "fvar ceiling refuses CALL and DENSITY instructions");
  }

  return out;
}

// Values and dense RHS Jacobians with no reverse-mode tape.  A full Jacobian
// call runs N + P directions; a primal-Jacobian call runs only the N state
// directions.  Buffers belong to one solve and therefore outlive every
// synchronous SUNDIALS callback without relying on global thread-local state.
template <bool Profile>
class TangentProvider {
 public:
  TangentProvider(const RhsProgram& program, const std::vector<double>& theta,
                  const std::vector<double>& xr)
      : p_(program),
        theta_(theta),
        xr_(xr),
        dreg_(static_cast<size_t>(program.n_regs)),
        freg_(static_cast<size_t>(program.n_regs)) {}

  void values(double t, const double* y, double* out) {
    Clock::time_point callback_begin;
    Clock::time_point part;
    if constexpr (Profile) {
      ++profile_.value_calls;
      callback_begin = Clock::now();
      part = callback_begin;
    }
    seed_double(t, y);
    if constexpr (Profile) {
      profile_.seed_ns += elapsed_ns(part);
      part = Clock::now();
    }
    stanli::run_program(p_, dreg_);
    if constexpr (Profile) {
      profile_.body_ns += elapsed_ns(part);
      part = Clock::now();
    }
    for (size_t i = 0; i < p_.out_regs.size(); ++i)
      out[i] = dreg_[static_cast<size_t>(p_.out_regs[i])];
    if constexpr (Profile) profile_.extract_ns += elapsed_ns(part);
    if constexpr (Profile)
      profile_.value_callback_ns += elapsed_ns(callback_begin);
  }

  void jacobian(double t, const double* y, bool include_theta, double* values,
                double* jy, double* jtheta) {
    const int width = p_.n_y + (include_theta ? p_.n_th : 0);
    if (include_theta && jtheta)
      std::fill(jtheta, jtheta + p_.out_regs.size() * theta_.size(), 0.0);
    Clock::time_point callback_begin;
    if constexpr (Profile) {
      if (include_theta)
        ++profile_.full_jacobian_calls;
      else
        ++profile_.jacobian_calls;
      profile_.tangent_directions += width;
      callback_begin = Clock::now();
    }

    for (int direction = 0; direction < width; ++direction) {
      Clock::time_point part;
      if constexpr (Profile) part = Clock::now();
      seed_fvar(t, y, direction, include_theta);
      if constexpr (Profile) {
        profile_.seed_ns += elapsed_ns(part);
        part = Clock::now();
      }
      stanli::run_program(p_, freg_);
      if constexpr (Profile) {
        profile_.body_ns += elapsed_ns(part);
        part = Clock::now();
      }
      for (size_t output = 0; output < p_.out_regs.size(); ++output) {
        const Fvar& result = freg_[static_cast<size_t>(p_.out_regs[output])];
        if (direction == 0 && values) values[output] = result.val_;
        if (direction < p_.n_y) {
          jy[output * static_cast<size_t>(p_.n_y) +
             static_cast<size_t>(direction)] = result.d_;
        } else if (jtheta) {
          const int column = direction - p_.n_y;
          jtheta[output * theta_.size() + static_cast<size_t>(column)] =
              result.d_;
        }
      }
      if constexpr (Profile) profile_.extract_ns += elapsed_ns(part);
    }
    if constexpr (Profile) {
      const int64_t elapsed = elapsed_ns(callback_begin);
      if (include_theta)
        profile_.sensitivity_callback_ns += elapsed;
      else
        profile_.jacobian_callback_ns += elapsed;
    }
  }

  CallbackProfile& profile() { return profile_; }
  const CallbackProfile& profile() const { return profile_; }

 private:
  const RhsProgram& p_;
  const std::vector<double>& theta_;
  const std::vector<double>& xr_;
  std::vector<double> dreg_;
  std::vector<Fvar> freg_;
  CallbackProfile profile_;

  void seed_double(double t, const double* y) {
    for (int i = 0; i < p_.n_y; ++i)
      dreg_[static_cast<size_t>(p_.y0 + i)] = y[i];
    for (int i = 0; i < p_.n_th; ++i)
      dreg_[static_cast<size_t>(p_.th0 + i)] = theta_[static_cast<size_t>(i)];
    dreg_[static_cast<size_t>(p_.t_reg)] = t;
    for (int i = 0; i < p_.n_xr; ++i)
      dreg_[static_cast<size_t>(p_.xr0 + i)] = xr_[static_cast<size_t>(i)];
  }

  void seed_fvar(double t, const double* y, int direction, bool include_theta) {
    for (int i = 0; i < p_.n_y; ++i)
      freg_[static_cast<size_t>(p_.y0 + i)] =
          Fvar(y[i], direction == i ? 1.0 : 0.0);
    for (int i = 0; i < p_.n_th; ++i) {
      const bool active = include_theta && direction == p_.n_y + i;
      freg_[static_cast<size_t>(p_.th0 + i)] =
          Fvar(theta_[static_cast<size_t>(i)], active ? 1.0 : 0.0);
    }
    freg_[static_cast<size_t>(p_.t_reg)] = Fvar(t, 0.0);
    for (int i = 0; i < p_.n_xr; ++i)
      freg_[static_cast<size_t>(p_.xr0 + i)] =
          Fvar(xr_[static_cast<size_t>(i)], 0.0);
  }
};

template <int Lmm, bool Profile>
class LocalCvodesIntegrator {
 public:
  LocalCvodesIntegrator(const BenchCase& benchmark, bool y_active,
                        bool theta_active)
      : benchmark_(benchmark),
        n_(benchmark.y0.size()),
        p_(benchmark.theta.size()),
        n_y_sens_(y_active ? n_ : 0),
        n_theta_sens_(theta_active ? p_ : 0),
        n_sens_(n_y_sens_ + n_theta_sens_),
        state_(n_ * (1 + n_sens_), 0.0),
        provider_(benchmark.ode.prog, benchmark.theta, benchmark.ode.x_r),
        jy_(n_ * n_),
        jtheta_(n_ * p_) {
    for (size_t i = 0; i < n_; ++i) state_[i] = benchmark.y0[i];
    for (size_t i = 0; i < n_y_sens_; ++i) state_[n_ + i * n_ + i] = 1.0;

    try {
      nv_state_ = N_VMake_Serial(n_, state_.data(), context_);
      if (!nv_state_) throw std::runtime_error("N_VMake_Serial failed");
      matrix_ = SUNDenseMatrix(n_, n_, context_);
      if (!matrix_) throw std::runtime_error("SUNDenseMatrix failed");
      linear_solver_ = SUNLinSol_Dense(nv_state_, matrix_, context_);
      if (!linear_solver_) throw std::runtime_error("SUNLinSol_Dense failed");
      if (n_sens_ != 0) {
        nv_sens_ =
            N_VCloneEmptyVectorArray(static_cast<int>(n_sens_), nv_state_);
        if (!nv_sens_)
          throw std::runtime_error("N_VCloneEmptyVectorArray failed");
        for (size_t s = 0; s < n_sens_; ++s)
          NV_DATA_S(nv_sens_[s]) = state_.data() + n_ + s * n_;
      }
    } catch (...) {
      release();
      throw;
    }
  }

  LocalCvodesIntegrator(const LocalCvodesIntegrator&) = delete;
  LocalCvodesIntegrator& operator=(const LocalCvodesIntegrator&) = delete;

  ~LocalCvodesIntegrator() { release(); }

  SolveResult solve() {
    SolveResult result;
    Clock::time_point total_begin;
    if constexpr (Profile) total_begin = Clock::now();
    void* memory = nullptr;
    try {
      Clock::time_point setup_begin;
      if constexpr (Profile) setup_begin = Clock::now();
      memory = CVodeCreate(Lmm, context_);
      if (!memory) throw std::runtime_error("CVodeCreate failed");
      CHECK_CVODES_CALL(CVodeInit(memory, &LocalCvodesIntegrator::cv_rhs,
                                  benchmark_.ode.t0, nv_state_));
      // CVodeSetJacFn and CVodeSensInit capture the current user-data pointer.
      // Preserve Stan Math's order: install it before either callback.
      CHECK_CVODES_CALL(CVodeSetUserData(memory, this));
      stan::math::cvodes_set_options(memory, benchmark_.ode.max_steps);
      CHECK_CVODES_CALL(
          CVodeSStolerances(memory, benchmark_.ode.rtol, benchmark_.ode.atol));
      CHECK_CVODES_CALL(CVodeSetLinearSolver(memory, linear_solver_, matrix_));
      CHECK_CVODES_CALL(
          CVodeSetJacFn(memory, &LocalCvodesIntegrator::cv_jacobian_states));
      if (n_sens_ != 0) {
        CHECK_CVODES_CALL(
            CVodeSensInit(memory, static_cast<int>(n_sens_), CV_STAGGERED,
                          &LocalCvodesIntegrator::cv_rhs_sens, nv_sens_));
        CHECK_CVODES_CALL(CVodeSetSensErrCon(memory, SUNTRUE));
        CHECK_CVODES_CALL(CVodeSensEEtolerances(memory));
      }
      if constexpr (Profile) result.profile.setup_ns = elapsed_ns(setup_begin);

      result.values.reserve(benchmark_.ode.ts.size() * n_);
      result.jacobian.assign(benchmark_.ode.ts.size() * n_ * (n_ + p_), 0.0);
      double current_time = benchmark_.ode.t0;
      for (size_t output_time = 0; output_time < benchmark_.ode.ts.size();
           ++output_time) {
        const double final_time = benchmark_.ode.ts[output_time];
        if (final_time != current_time) {
          Clock::time_point integrate_begin;
          if constexpr (Profile) integrate_begin = Clock::now();
          CHECK_CVODES_CALL(
              CVode(memory, final_time, nv_state_, &current_time, CV_NORMAL));
          if constexpr (Profile)
            result.profile.integrate_ns += elapsed_ns(integrate_begin);
          if (n_sens_ != 0) {
            Clock::time_point get_begin;
            if constexpr (Profile) get_begin = Clock::now();
            CHECK_CVODES_CALL(CVodeGetSens(memory, &current_time, nv_sens_));
            if constexpr (Profile)
              result.profile.get_sens_ns += elapsed_ns(get_begin);
          }
        }

        Clock::time_point output_begin;
        if constexpr (Profile) output_begin = Clock::now();
        for (size_t row = 0; row < n_; ++row) {
          result.values.push_back(state_[row]);
          const size_t flat_output = output_time * n_ + row;
          double* jac = result.jacobian.data() + flat_output * (n_ + p_);
          for (size_t column = 0; column < n_y_sens_; ++column)
            jac[column] = state_[n_ + column * n_ + row];
          for (size_t column = 0; column < n_theta_sens_; ++column) {
            const size_t sensitivity = n_y_sens_ + column;
            jac[n_ + column] = state_[n_ + sensitivity * n_ + row];
          }
        }
        if constexpr (Profile)
          result.profile.output_ns += elapsed_ns(output_begin);
        current_time = final_time;
      }

      if constexpr (Profile) collect_stats(memory, &result.profile.cvodes);
      CVodeFree(&memory);
      result.profile.callback = provider_.profile();
      if constexpr (Profile) result.profile.total_ns = elapsed_ns(total_begin);
      return result;
    } catch (...) {
      if (memory) CVodeFree(&memory);
      throw;
    }
  }

 private:
  const BenchCase& benchmark_;
  size_t n_;
  size_t p_;
  size_t n_y_sens_;
  size_t n_theta_sens_;
  size_t n_sens_;
  sundials::Context context_;
  std::vector<double> state_;
  N_Vector nv_state_ = nullptr;
  N_Vector* nv_sens_ = nullptr;
  SUNMatrix matrix_ = nullptr;
  SUNLinearSolver linear_solver_ = nullptr;
  TangentProvider<Profile> provider_;
  std::vector<double> jy_;
  std::vector<double> jtheta_;

  void release() noexcept {
    if (nv_sens_) {
      N_VDestroyVectorArray(nv_sens_, static_cast<int>(n_sens_));
      nv_sens_ = nullptr;
    }
    if (linear_solver_) {
      SUNLinSolFree(linear_solver_);
      linear_solver_ = nullptr;
    }
    if (matrix_) {
      SUNMatDestroy(matrix_);
      matrix_ = nullptr;
    }
    if (nv_state_) {
      N_VDestroy_Serial(nv_state_);
      nv_state_ = nullptr;
    }
  }

  static int cv_rhs(realtype t, N_Vector y, N_Vector ydot, void* user_data) {
    auto* self = static_cast<LocalCvodesIntegrator*>(user_data);
    self->provider_.values(t, NV_DATA_S(y), NV_DATA_S(ydot));
    return 0;
  }

  static int cv_jacobian_states(realtype t, N_Vector y, N_Vector,
                                SUNMatrix jacobian, void* user_data, N_Vector,
                                N_Vector, N_Vector) {
    auto* self = static_cast<LocalCvodesIntegrator*>(user_data);
    self->provider_.jacobian(t, NV_DATA_S(y), false, nullptr, self->jy_.data(),
                             nullptr);
    for (size_t column = 0; column < self->n_; ++column)
      for (size_t row = 0; row < self->n_; ++row)
        SM_ELEMENT_D(jacobian, row, column) =
            self->jy_[row * self->n_ + column];
    return 0;
  }

  static int cv_rhs_sens(int, realtype t, N_Vector y, N_Vector,
                         N_Vector* y_sens, N_Vector* y_sens_dot,
                         void* user_data, N_Vector, N_Vector) {
    auto* self = static_cast<LocalCvodesIntegrator*>(user_data);
    self->provider_.jacobian(
        t, NV_DATA_S(y), self->n_theta_sens_ != 0, nullptr, self->jy_.data(),
        self->n_theta_sens_ ? self->jtheta_.data() : nullptr);
    Clock::time_point product_begin;
    if constexpr (Profile) product_begin = Clock::now();
    // Match coupled_ode_system's scalar grouping: output row outermost;
    // initial-state lanes start at +0, parameter lanes start at J_theta, and
    // each dot product accumulates state columns in ascending order.
    for (size_t row = 0; row < self->n_; ++row) {
      for (size_t sensitivity = 0; sensitivity < self->n_y_sens_;
           ++sensitivity) {
        double derivative = 0.0;
        for (size_t state = 0; state < self->n_; ++state)
          derivative += NV_DATA_S(y_sens[sensitivity])[state] *
                        self->jy_[row * self->n_ + state];
        NV_DATA_S(y_sens_dot[sensitivity])[row] = derivative;
      }
      for (size_t parameter = 0; parameter < self->n_theta_sens_; ++parameter) {
        const size_t sensitivity = self->n_y_sens_ + parameter;
        double derivative = self->jtheta_[row * self->p_ + parameter];
        for (size_t state = 0; state < self->n_; ++state)
          derivative += NV_DATA_S(y_sens[sensitivity])[state] *
                        self->jy_[row * self->n_ + state];
        NV_DATA_S(y_sens_dot[sensitivity])[row] = derivative;
      }
    }
    if constexpr (Profile)
      self->provider_.profile().sensitivity_product_ns +=
          elapsed_ns(product_begin);
    return 0;
  }

  void collect_stats(void* memory, CvodesStats* stats) {
    CHECK_CVODES_CALL(CVodeGetNumSteps(memory, &stats->steps));
    CHECK_CVODES_CALL(CVodeGetNumRhsEvals(memory, &stats->rhs_evals));
    CHECK_CVODES_CALL(CVodeGetNumLinSolvSetups(memory, &stats->linear_setups));
    CHECK_CVODES_CALL(
        CVodeGetNumErrTestFails(memory, &stats->error_test_fails));
    CHECK_CVODES_CALL(
        CVodeGetNumNonlinSolvIters(memory, &stats->nonlinear_iters));
    CHECK_CVODES_CALL(
        CVodeGetNumNonlinSolvConvFails(memory, &stats->nonlinear_fails));
    CHECK_CVODES_CALL(CVodeGetNumJacEvals(memory, &stats->jacobian_evals));
    CHECK_CVODES_CALL(CVodeGetNumLinRhsEvals(memory, &stats->linear_rhs_evals));
    CHECK_CVODES_CALL(CVodeGetNumLinIters(memory, &stats->linear_iters));
    CHECK_CVODES_CALL(CVodeGetNumLinConvFails(memory, &stats->linear_fails));
    if (n_sens_ != 0) {
      CHECK_CVODES_CALL(
          CVodeGetSensNumRhsEvals(memory, &stats->sens_rhs_evals));
      CHECK_CVODES_CALL(
          CVodeGetNumRhsEvalsSens(memory, &stats->sens_extra_rhs_evals));
      CHECK_CVODES_CALL(
          CVodeGetSensNumErrTestFails(memory, &stats->sens_error_test_fails));
      CHECK_CVODES_CALL(
          CVodeGetSensNumLinSolvSetups(memory, &stats->sens_linear_setups));
      CHECK_CVODES_CALL(
          CVodeGetSensNumNonlinSolvIters(memory, &stats->sens_nonlinear_iters));
      CHECK_CVODES_CALL(CVodeGetSensNumNonlinSolvConvFails(
          memory, &stats->sens_nonlinear_fails));
    }
  }
};

struct OracleCallbackProfile {
  int64_t double_calls = 0;
  int64_t var_calls = 0;
  int64_t double_ns = 0;
  int64_t var_ns = 0;
};

// The merged PR #245 callback shape: caller-owned Eigen output and direct
// run_rhs_into, with no legacy public std::vector adapter.
template <bool Profile>
struct OracleRhs {
  const BenchCase* benchmark;
  OracleCallbackProfile* profile;
  mutable stanli::RhsWorkspace workspace;

  template <typename T_y, typename T_theta>
  Eigen::Matrix<stan::return_type_t<T_y, T_theta>, Eigen::Dynamic, 1>
  operator()(const double& t, const Eigen::Matrix<T_y, Eigen::Dynamic, 1>& y,
             std::ostream*, const std::vector<T_theta>& theta,
             const std::vector<double>& x_r, const std::vector<int>&) const {
    using T = stan::return_type_t<T_y, T_theta>;
    Clock::time_point begin;
    if constexpr (Profile) begin = Clock::now();
    Eigen::Matrix<T, Eigen::Dynamic, 1> out(
        static_cast<Eigen::Index>(benchmark->ode.prog.out_regs.size()));
    stanli::run_rhs_into<T>(benchmark->ode.prog, t, y.data(), theta.data(),
                            theta.size(), x_r.data(), out.data(),
                            workspace.get<T>());
    if constexpr (Profile) {
      if constexpr (std::is_same_v<T, double>) {
        ++profile->double_calls;
        profile->double_ns += elapsed_ns(begin);
      } else {
        ++profile->var_calls;
        profile->var_ns += elapsed_ns(begin);
      }
    }
    return out;
  }
};

struct NoNestedAutodiff {};

template <int Lmm, bool YActive, bool ThetaActive, bool Profile>
SolveResult run_oracle_typed(const BenchCase& benchmark) {
  using T_y = std::conditional_t<YActive, var, double>;
  using T_theta = std::conditional_t<ThetaActive, var, double>;
  using T_out = stan::return_type_t<T_y, T_theta>;

  SolveResult result;
  Clock::time_point total_begin;
  if constexpr (Profile) total_begin = Clock::now();
  OracleCallbackProfile callback;
  using Nested =
      std::conditional_t<YActive || ThetaActive,
                         stan::math::nested_rev_autodiff, NoNestedAutodiff>;
  Nested nested;

  // Retain ode_fwd_typed's construction order: y0, then theta, then the
  // Eigen-state handle copies made by solve().
  std::vector<T_y> y0(benchmark.y0.begin(), benchmark.y0.end());
  std::vector<T_theta> theta(benchmark.theta.begin(), benchmark.theta.end());
  Eigen::Matrix<T_y, Eigen::Dynamic, 1> eigen_y0(
      static_cast<Eigen::Index>(y0.size()));
  for (size_t i = 0; i < y0.size(); ++i)
    eigen_y0(static_cast<Eigen::Index>(i)) = y0[i];

  OracleRhs<Profile> rhs{&benchmark, &callback};
  std::vector<Eigen::Matrix<T_out, Eigen::Dynamic, 1>> solution;
  Clock::time_point solve_begin;
  if constexpr (Profile) solve_begin = Clock::now();
  if constexpr (Lmm == CV_BDF) {
    solution = stan::math::ode_bdf_tol_impl(
        "integrate_ode_bdf", rhs, eigen_y0, benchmark.ode.t0, benchmark.ode.ts,
        benchmark.ode.rtol, benchmark.ode.atol, benchmark.ode.max_steps,
        nullptr, theta, benchmark.ode.x_r, benchmark.ode.x_i);
  } else {
    solution = stan::math::ode_adams_tol_impl(
        "integrate_ode_adams", rhs, eigen_y0, benchmark.ode.t0,
        benchmark.ode.ts, benchmark.ode.rtol, benchmark.ode.atol,
        benchmark.ode.max_steps, nullptr, theta, benchmark.ode.x_r,
        benchmark.ode.x_i);
  }

  const size_t n = benchmark.y0.size(), p = benchmark.theta.size();
  const size_t output_count = solution.size() * n;
  result.values.resize(output_count);
  result.jacobian.assign(output_count * (n + p), 0.0);
  for (size_t time = 0; time < solution.size(); ++time)
    for (size_t state = 0; state < n; ++state) {
      const size_t output = time * n + state;
      if constexpr (std::is_same_v<T_out, double>)
        result.values[output] = solution[time](state);
      else
        result.values[output] = solution[time](state).val();
    }

  if constexpr (!std::is_same_v<T_out, double>) {
    Clock::time_point harvest_begin;
    if constexpr (Profile) harvest_begin = Clock::now();
    stan::math::set_zero_all_adjoints_nested();
    for (size_t output = output_count; output-- > 0;) {
      var value = solution[output / n](output % n);
      value.vi_->adj_ = 1.0;
      value.vi_->chain();
      double* row = result.jacobian.data() + output * (n + p);
      if constexpr (YActive) {
        for (size_t i = 0; i < n; ++i) {
          row[i] = y0[i].adj();
          y0[i].vi_->adj_ = 0.0;
        }
      }
      if constexpr (ThetaActive) {
        for (size_t i = 0; i < p; ++i) {
          row[n + i] = theta[i].adj();
          theta[i].vi_->adj_ = 0.0;
        }
      }
      value.vi_->adj_ = 0.0;
    }
    if constexpr (Profile)
      result.profile.final_harvest_ns = elapsed_ns(harvest_begin);
  }

  if constexpr (Profile) {
    result.profile.total_ns = elapsed_ns(total_begin);
    result.profile.allocation_ns = duration_ns(total_begin, solve_begin);
    result.profile.oracle_double_callbacks = callback.double_calls;
    result.profile.oracle_var_callbacks = callback.var_calls;
    result.profile.oracle_double_callback_ns = callback.double_ns;
    result.profile.oracle_var_callback_ns = callback.var_ns;
  }
  return result;
}

template <int Lmm, bool Profile>
SolveResult run_oracle(const BenchCase& benchmark, bool y_active,
                       bool theta_active) {
  if (y_active && theta_active)
    return run_oracle_typed<Lmm, true, true, Profile>(benchmark);
  if (y_active) return run_oracle_typed<Lmm, true, false, Profile>(benchmark);
  if (theta_active)
    return run_oracle_typed<Lmm, false, true, Profile>(benchmark);
  return run_oracle_typed<Lmm, false, false, Profile>(benchmark);
}

template <int Lmm, bool Profile>
SolveResult run_local(const BenchCase& benchmark, bool y_active,
                      bool theta_active) {
  Clock::time_point begin;
  Clock::time_point constructed;
  if constexpr (Profile) begin = Clock::now();
  SolveResult result;
  {
    LocalCvodesIntegrator<Lmm, Profile> integrator(benchmark, y_active,
                                                   theta_active);
    if constexpr (Profile) constructed = Clock::now();
    result = integrator.solve();
    if constexpr (Profile)
      result.profile.allocation_ns = duration_ns(begin, constructed);
  }
  if constexpr (Profile) result.profile.total_ns = elapsed_ns(begin);
  return result;
}

struct Comparison {
  size_t count = 0;
  size_t exact = 0;
  double max_abs = 0.0;
  double max_rel = 0.0;
  uint64_t max_ulp = 0;
  size_t worst = 0;
  bool numerical = true;
};

Comparison compare(const std::vector<double>& candidate,
                   const std::vector<double>& oracle) {
  if (candidate.size() != oracle.size())
    throw std::runtime_error("comparison size mismatch");
  Comparison out;
  out.count = candidate.size();
  for (size_t i = 0; i < candidate.size(); ++i) {
    const bool exact = bits(candidate[i]) == bits(oracle[i]);
    if (exact) ++out.exact;
    if (!std::isfinite(candidate[i]) || !std::isfinite(oracle[i])) {
      out.max_abs = std::numeric_limits<double>::infinity();
      out.max_rel = std::numeric_limits<double>::infinity();
      out.max_ulp = std::numeric_limits<uint64_t>::max();
      out.worst = i;
      out.numerical = false;
      continue;
    }
    const double absolute = std::abs(candidate[i] - oracle[i]);
    const double scale =
        std::max({1e-12, std::abs(candidate[i]), std::abs(oracle[i])});
    const double relative = absolute / scale;
    if (absolute > 1e-12 && relative > 1e-9) out.numerical = false;
    const uint64_t ulps = ulp_distance(candidate[i], oracle[i]);
    if (relative > out.max_rel ||
        (relative == out.max_rel && absolute > out.max_abs))
      out.worst = i;
    out.max_abs = std::max(out.max_abs, absolute);
    out.max_rel = std::max(out.max_rel, relative);
    out.max_ulp = std::max(out.max_ulp, ulps);
  }
  return out;
}

void print_comparison(const char* label, const Comparison& comparison,
                      const std::vector<double>& candidate,
                      const std::vector<double>& oracle) {
  std::printf(
      "%s: exact=%zu/%zu max_abs=%.17g max_rel=%.17g max_ulp=%llu "
      "worst[%zu]=%.17g oracle=%.17g\n",
      label, comparison.exact, comparison.count, comparison.max_abs,
      comparison.max_rel, static_cast<unsigned long long>(comparison.max_ulp),
      comparison.worst, comparison.count ? candidate[comparison.worst] : 0.0,
      comparison.count ? oracle[comparison.worst] : 0.0);
}

struct LocalRhsResult {
  std::vector<double> values;
  std::vector<double> jacobian;
};

LocalRhsResult local_reverse_result(const BenchCase& benchmark, double t) {
  stan::math::nested_rev_autodiff nested;
  std::vector<var> y;
  std::vector<var> theta;
  y.reserve(benchmark.y0.size());
  theta.reserve(benchmark.theta.size());
  for (double value : benchmark.y0) y.emplace_back(value);
  for (double value : benchmark.theta) theta.emplace_back(value);

  std::vector<var> output(benchmark.ode.prog.out_regs.size());
  std::vector<var> rhs_registers;
  stanli::run_rhs_into<var>(benchmark.ode.prog, t, y.data(), theta.data(),
                            theta.size(), benchmark.ode.x_r.data(),
                            output.data(), rhs_registers);
  LocalRhsResult result;
  result.values.resize(output.size());
  const size_t width = y.size() + theta.size();
  result.jacobian.resize(output.size() * width);
  for (size_t row = 0; row < output.size(); ++row) {
    result.values[row] = output[row].val();
    stan::math::grad(output[row].vi_);
    for (size_t column = 0; column < y.size(); ++column)
      result.jacobian[row * width + column] = y[column].adj();
    for (size_t column = 0; column < theta.size(); ++column)
      result.jacobian[row * width + y.size() + column] = theta[column].adj();
    if (row + 1 < output.size()) nested.set_zero_all_adjoints();
  }
  return result;
}

LocalRhsResult local_tangent_result(const BenchCase& benchmark, double t) {
  TangentProvider<false> provider(benchmark.ode.prog, benchmark.theta,
                                  benchmark.ode.x_r);
  const size_t n = benchmark.y0.size();
  const size_t p = benchmark.theta.size();
  std::vector<double> jy(n * n);
  std::vector<double> jtheta(n * p);
  LocalRhsResult result;
  result.values.resize(n);
  result.jacobian.resize(n * (n + p));
  provider.jacobian(t, benchmark.y0.data(), true, result.values.data(),
                    jy.data(), jtheta.data());
  for (size_t row = 0; row < n; ++row) {
    std::copy_n(jy.data() + row * n, n, result.jacobian.data() + row * (n + p));
    std::copy_n(jtheta.data() + row * p, p,
                result.jacobian.data() + row * (n + p) + n);
  }
  return result;
}

void print_local_branch_proof(const BenchCase& benchmark, bool require_exact) {
  const double infinity = std::numeric_limits<double>::infinity();
  const double midpoint =
      benchmark.ode.ts.empty()
          ? benchmark.ode.t0
          : benchmark.ode.t0 +
                0.5 * (benchmark.ode.ts.back() - benchmark.ode.t0);
  std::vector<double> candidates{benchmark.ode.t0};
  if (benchmark.ode.rhs_name == "one_comp_mm_elim_abs") {
    candidates.insert(candidates.end(),
                      {-0.0, 0.0, std::nextafter(benchmark.ode.t0, -infinity),
                       std::nextafter(benchmark.ode.t0, infinity)});
  } else if (benchmark.ode.rhs_name == "synthetic_rhs") {
    candidates.insert(candidates.end(),
                      {std::nextafter(midpoint, -infinity), midpoint,
                       std::nextafter(midpoint, infinity)});
  }
  std::vector<double> probes;
  for (double candidate : candidates) {
    const bool seen = std::any_of(
        probes.begin(), probes.end(),
        [&](double prior) { return bits(prior) == bits(candidate); });
    if (!seen) probes.push_back(candidate);
  }

  std::printf("local branch-aware tangent proof: probes=%zu\n", probes.size());
  for (size_t i = 0; i < probes.size(); ++i) {
    const LocalRhsResult oracle = local_reverse_result(benchmark, probes[i]);
    const LocalRhsResult tangent = local_tangent_result(benchmark, probes[i]);
    const Comparison values = compare(tangent.values, oracle.values);
    const Comparison jacobian = compare(tangent.jacobian, oracle.jacobian);
    std::printf("local probe[%zu] t=%.17g signbit=%d\n", i, probes[i],
                std::signbit(probes[i]) ? 1 : 0);
    print_comparison("  rhs values", values, tangent.values, oracle.values);
    print_comparison("  rhs Jacobian", jacobian, tangent.jacobian,
                     oracle.jacobian);
    const bool exact =
        values.exact == values.count && jacobian.exact == jacobian.count;
    if (!values.numerical || !jacobian.numerical || (require_exact && !exact))
      throw std::runtime_error("local tangent failed the numerical gate");
  }
}

void print_profile(const SolveResult& oracle, const SolveResult& local) {
  const SolveProfile& old = oracle.profile;
  const SolveProfile& now = local.profile;
  const CallbackProfile& callback = now.callback;
  const CvodesStats& stats = now.cvodes;
  std::printf(
      "oracle profile ns: total=%lld allocation=%lld callback_double=%lld/%lld "
      "callback_var=%lld/%lld final_harvest=%lld\n",
      static_cast<long long>(old.total_ns),
      static_cast<long long>(old.allocation_ns),
      static_cast<long long>(old.oracle_double_callback_ns),
      static_cast<long long>(old.oracle_double_callbacks),
      static_cast<long long>(old.oracle_var_callback_ns),
      static_cast<long long>(old.oracle_var_callbacks),
      static_cast<long long>(old.final_harvest_ns));
  std::printf(
      "local profile ns: total=%lld allocation=%lld setup=%lld integrate=%lld "
      "get_sens=%lld "
      "output=%lld seed=%lld body=%lld extract=%lld sens_product=%lld\n",
      static_cast<long long>(now.total_ns),
      static_cast<long long>(now.allocation_ns),
      static_cast<long long>(now.setup_ns),
      static_cast<long long>(now.integrate_ns),
      static_cast<long long>(now.get_sens_ns),
      static_cast<long long>(now.output_ns),
      static_cast<long long>(callback.seed_ns),
      static_cast<long long>(callback.body_ns),
      static_cast<long long>(callback.extract_ns),
      static_cast<long long>(callback.sensitivity_product_ns));
  std::printf(
      "local callbacks: values=%lld jy=%lld full_j=%lld directions=%lld "
      "value_ns=%lld jy_ns=%lld full_j_ns=%lld\n",
      static_cast<long long>(callback.value_calls),
      static_cast<long long>(callback.jacobian_calls),
      static_cast<long long>(callback.full_jacobian_calls),
      static_cast<long long>(callback.tangent_directions),
      static_cast<long long>(callback.value_callback_ns),
      static_cast<long long>(callback.jacobian_callback_ns),
      static_cast<long long>(callback.sensitivity_callback_ns));
  std::printf(
      "CVODES stats: steps=%ld rhs=%ld jac=%ld lin_setup=%ld err_fail=%ld "
      "nlin_iter=%ld nlin_fail=%ld lin_rhs=%ld lin_iter=%ld lin_fail=%ld\n",
      stats.steps, stats.rhs_evals, stats.jacobian_evals, stats.linear_setups,
      stats.error_test_fails, stats.nonlinear_iters, stats.nonlinear_fails,
      stats.linear_rhs_evals, stats.linear_iters, stats.linear_fails);
  std::printf(
      "CVODES sensitivity stats: rhs=%ld extra_primal_rhs=%ld err_fail=%ld "
      "lin_setup=%ld nlin_iter=%ld nlin_fail=%ld\n",
      stats.sens_rhs_evals, stats.sens_extra_rhs_evals,
      stats.sens_error_test_fails, stats.sens_linear_setups,
      stats.sens_nonlinear_iters, stats.sens_nonlinear_fails);
}

template <typename F>
double time_solves(int iterations, F&& solve) {
  const auto begin = Clock::now();
  for (int i = 0; i < iterations; ++i) {
    SolveResult result = solve();
    if (!result.values.empty()) benchmark_sink += result.values.back();
    if (!result.jacobian.empty()) benchmark_sink += result.jacobian.back();
  }
  return static_cast<double>(elapsed_ns(begin)) /
         static_cast<double>(iterations);
}

template <int Lmm>
void run_benchmark(const BenchCase& benchmark, bool y_active, bool theta_active,
                   int iterations, int batches, int warmup_ms,
                   bool require_exact) {
  const SolveResult oracle_profile =
      run_oracle<Lmm, true>(benchmark, y_active, theta_active);
  const SolveResult local_profile =
      run_local<Lmm, true>(benchmark, y_active, theta_active);
  const Comparison values =
      compare(local_profile.values, oracle_profile.values);
  const Comparison jacobian =
      compare(local_profile.jacobian, oracle_profile.jacobian);
  print_comparison("solution values (fvar ceiling vs current oracle)", values,
                   local_profile.values, oracle_profile.values);
  print_comparison("solution Jacobian (fvar ceiling vs current oracle)",
                   jacobian, local_profile.jacobian, oracle_profile.jacobian);
  const int64_t oracle_derivative_callbacks =
      oracle_profile.profile.oracle_var_callbacks;
  const int64_t local_derivative_callbacks =
      local_profile.profile.callback.jacobian_calls +
      local_profile.profile.callback.full_jacobian_calls;
  const bool callback_equal =
      oracle_profile.profile.oracle_double_callbacks ==
          local_profile.profile.callback.value_calls &&
      oracle_derivative_callbacks == local_derivative_callbacks;
  const bool solution_exact =
      values.exact == values.count && jacobian.exact == jacobian.count;
  const bool correctness_pass = values.numerical && jacobian.numerical &&
                                (!require_exact || solution_exact);
  std::printf(
      "timing_gate=%s pass=%d work_parity=callback_count "
      "oracle_cvodes_stats=unavailable\n",
      require_exact ? "exact" : "proximity",
      callback_equal && correctness_pass ? 1 : 0);
  if (!callback_equal || !correctness_pass)
    throw std::runtime_error(
        "candidate failed the pre-timing correctness gate");
  print_profile(oracle_profile, local_profile);

  // Warm the compile-time-uninstrumented arms independently so each reaches
  // the requested duration even when their costs differ materially.
  const double warmup_target_ns = static_cast<double>(warmup_ms) * 1e6;
  double oracle_warmup_ns = 0.0, local_warmup_ns = 0.0;
  size_t oracle_warmup_solves = 0, local_warmup_solves = 0;
  while (oracle_warmup_ns < warmup_target_ns ||
         local_warmup_ns < warmup_target_ns) {
    if (oracle_warmup_ns < warmup_target_ns) {
      oracle_warmup_ns += time_solves(1, [&] {
        return run_oracle<Lmm, false>(benchmark, y_active, theta_active);
      });
      ++oracle_warmup_solves;
    }
    if (local_warmup_ns < warmup_target_ns) {
      local_warmup_ns += time_solves(1, [&] {
        return run_local<Lmm, false>(benchmark, y_active, theta_active);
      });
      ++local_warmup_solves;
    }
  }
  std::printf(
      "warmup oracle_ms=%.3f local_ms=%.3f oracle_solves=%zu "
      "local_solves=%zu\n",
      oracle_warmup_ns / 1e6, local_warmup_ns / 1e6, oracle_warmup_solves,
      local_warmup_solves);

  std::vector<double> oracle_ns, local_ns, speedups;
  oracle_ns.reserve(static_cast<size_t>(batches));
  local_ns.reserve(static_cast<size_t>(batches));
  speedups.reserve(static_cast<size_t>(batches));
  for (int batch = 0; batch < batches; ++batch) {
    double old_time = 0.0, new_time = 0.0;
    if ((batch & 1) == 0) {
      old_time = time_solves(iterations, [&] {
        return run_oracle<Lmm, false>(benchmark, y_active, theta_active);
      });
      new_time = time_solves(iterations, [&] {
        return run_local<Lmm, false>(benchmark, y_active, theta_active);
      });
    } else {
      new_time = time_solves(iterations, [&] {
        return run_local<Lmm, false>(benchmark, y_active, theta_active);
      });
      old_time = time_solves(iterations, [&] {
        return run_oracle<Lmm, false>(benchmark, y_active, theta_active);
      });
    }
    oracle_ns.push_back(old_time);
    local_ns.push_back(new_time);
    speedups.push_back(old_time / new_time);
    std::printf("batch %02d: oracle=%.1f ns local=%.1f ns speedup=%.6fx\n",
                batch + 1, old_time, new_time, old_time / new_time);
  }
  std::printf(
      "uninstrumented medians: oracle=%.1f ns local=%.1f ns paired "
      "speedup=%.6fx (range %.6fx..%.6fx) sink=%.17g\n",
      median(oracle_ns), median(local_ns), median(speedups),
      *std::min_element(speedups.begin(), speedups.end()),
      *std::max_element(speedups.begin(), speedups.end()), benchmark_sink);
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

    // One normal graph evaluation materializes transformed y0/theta slots.
    // This preparation is outside all component and paired solve timings.
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

    const auto& graph = executor.graph();
    const size_t n_y = static_cast<size_t>(graph.slots[ode_op->in[0]].len);
    const size_t n_theta = static_cast<size_t>(graph.slots[ode_op->in[1]].len);
    const size_t output_size =
        static_cast<size_t>(graph.slots[ode_op->out].len);
    if (n_y == 0 || output_size % n_y != 0 ||
        output_size / n_y != spec->ts.size())
      throw std::runtime_error("OP_ODE shape does not match OdeSpec times");
    if (spec->prog.out_regs.size() != n_y)
      throw std::runtime_error(
          "compiled RHS output width does not match state");
    const uint8_t type_mask =
        (ode_op->variant & 0x4u) != 0 ? (ode_op->variant & 0x3u) : 0x3u;
    const bool y_active = (type_mask & 0x1u) != 0;
    const bool theta_active = (type_mask & 0x2u) != 0;
    const OdeSpec::Solver selected_solver = select_solver(options, *spec);
    BenchCase benchmark = materialize_case(
        *spec, executor.value_ptr(ode_op->in[0]), n_y,
        executor.value_ptr(ode_op->in[1]), n_theta, selected_solver);

    std::printf(
        "CVODES fvar ceiling/proximity experiment: model_lp=%.17g rhs=%s "
        "original_solver=%s solver=%s legacy=%d point=%d\n",
        model_lp, benchmark.ode.rhs_name.c_str(), solver_name(spec->solver),
        solver_name(selected_solver), benchmark.ode.legacy ? 1 : 0,
        options.point);
    std::printf(
        "shape states=%zu theta_source=%zu theta_program=%d xr=%zu times=%zu "
        "outputs=%zu type_mask=0x%x activity=%d/%d code=%zu regs=%d "
        "rtol=%.17g atol=%.17g max_steps=%ld\n",
        n_y, n_theta, benchmark.ode.prog.n_th, benchmark.ode.x_r.size(),
        benchmark.ode.ts.size(), output_size, static_cast<unsigned>(type_mask),
        y_active ? 1 : 0, theta_active ? 1 : 0, benchmark.ode.prog.code.size(),
        benchmark.ode.prog.n_regs, benchmark.ode.rtol, benchmark.ode.atol,
        benchmark.ode.max_steps);
    print_local_branch_proof(benchmark, options.require_exact);
    if (selected_solver == OdeSpec::BDF)
      run_benchmark<CV_BDF>(benchmark, y_active, theta_active,
                            options.iterations, options.batches,
                            options.warmup_ms, options.require_exact);
    else
      run_benchmark<CV_ADAMS>(benchmark, y_active, theta_active,
                              options.iterations, options.batches,
                              options.warmup_ms, options.require_exact);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "bench_ode_cvodes_ceiling: %s\n", error.what());
    return 1;
  }
}
