// The compiled ODE right-hand side against the interpreter it replaces.
//
// compile_rhs is a second implementation of the same semantics, and a fast
// second implementation is exactly the kind that drifts. So every supported
// shape is run both ways on the same inputs and required to agree bitwise --
// not to a tolerance: the two evaluate the same operations in the same order,
// and anything else is a bug, not rounding.
//
// The other half is the fallback. compile_rhs refuses what it cannot express
// (a return out of a branch on the solve time), and the test pins both halves
// of that contract: it must refuse, with a reason, and the interpreter must
// still produce the right answer.
#include <stanli/mir.hpp>
#include <stanli/mir_interp.hpp>
#include <stanli/ode_prog.hpp>
#include <stanli/sexp.hpp>

#include <stan/math.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace {

int failures = 0;

void expect(const std::string& what, bool ok) {
  if (!ok) {
    ++failures;
    std::printf("FAIL %s\n", what.c_str());
  }
}

uint64_t bits(double x) {
  uint64_t out;
  std::memcpy(&out, &x, sizeof(out));
  return out;
}

std::string slurp(const std::string& path) {
  std::ifstream f(path);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// Deterministic, and spread over both sides of the branch condition.
double probe(int i) { return 0.35 + 0.21 * std::sin(1.7 * i) + 0.05 * (i % 3); }

void check(const std::string& name, const stanli::mir::FunDef& f,
           const std::map<std::string, const stanli::mir::FunDef*>& funs,
           int n_y, int n_th, const std::vector<double>& x_r,
           const std::vector<int>& x_i, bool want_ok) {
  using namespace stanli;
  RhsProgram p = compile_rhs(f, funs, n_y, n_th, (int)x_r.size(), x_i);
  if (p.ok != want_ok) {
    ++failures;
    std::printf("FAIL %s: compile ok=%d, wanted %d (%s)\n", name.c_str(),
                (int)p.ok, (int)want_ok, p.why.c_str());
    return;
  }
  if (!want_ok) {
    if (p.why.empty()) {
      ++failures;
      std::printf("FAIL %s: refused without saying why\n", name.c_str());
    }
    return;  // the interpreter still serves it; the ODE kernel falls back
  }

  for (int trial = 0; trial < 12; ++trial) {
    const double t = probe(trial) * 2.0;  // straddles the t > 0.5 branch
    std::vector<double> y((size_t)n_y), th((size_t)n_th);
    for (int i = 0; i < n_y; ++i) y[(size_t)i] = probe(trial * 7 + i) + 0.4;
    for (int i = 0; i < n_th; ++i) th[(size_t)i] = probe(trial * 11 + i) + 0.2;

    std::vector<double> got;
    run_rhs<double>(p, t, y.data(), th.data(), x_r.data(), got);

    MirInterp<double> ev(funs, "ODE function");
    std::vector<double> tv{t}, xrv(x_r.begin(), x_r.end());
    const std::vector<double> want = ev.call(f, {tv, y, th, xrv}, {x_i});

    if (got.size() != want.size()) {
      ++failures;
      std::printf("FAIL %s: %zu outputs, interpreter gave %zu\n", name.c_str(),
                  got.size(), want.size());
      return;
    }
    for (size_t k = 0; k < got.size(); ++k) {
      if (got[k] != want[k]) {  // bitwise: same ops, same order
        ++failures;
        std::printf("FAIL %s trial %d out %zu: %.17g vs %.17g\n", name.c_str(),
                    trial, k, got[k], want[k]);
        return;
      }
    }
  }
}

// Capture the observable result and the exact scalar tape written by one RHS
// replay. The staged case spells the old MirRhs path: promote all y/theta
// values first, then t, before entering the register machine. The direct case
// is the allocation-free path. Tape values as well as counts make moving one
// of those promotions across t visible even when the gradient is unchanged.
struct MixedRun {
  std::vector<uint64_t> values;
  std::vector<uint64_t> y_grads;
  std::vector<uint64_t> theta_grads;
  std::vector<uint64_t> chain_tape;
  std::vector<uint64_t> nochain_tape;
};

std::vector<uint64_t> tape_bits(const std::vector<stan::math::vari_base*>& tape,
                                size_t first) {
  std::vector<uint64_t> out;
  out.reserve(tape.size() - first);
  for (size_t i = first; i < tape.size(); ++i) {
    const auto* scalar = dynamic_cast<const stan::math::vari*>(tape[i]);
    if (!scalar) {
      ++failures;
      std::printf("FAIL mixed seed produced a non-scalar tape node\n");
      return {};
    }
    out.push_back(bits(scalar->val_));
  }
  return out;
}

template <bool YAutodiff, bool ThetaAutodiff, bool Staged>
MixedRun mixed_run(const stanli::RhsProgram& p, double t,
                   const std::vector<double>& y_values,
                   const std::vector<double>& theta_values,
                   const std::vector<double>& x_r) {
  using T_y = std::conditional_t<YAutodiff, stan::math::var, double>;
  using T_theta = std::conditional_t<ThetaAutodiff, stan::math::var, double>;
  using T = stan::return_type_t<T_y, T_theta>;

  stan::math::nested_rev_autodiff nested;
  std::vector<T_y> y(y_values.begin(), y_values.end());
  std::vector<T_theta> theta(theta_values.begin(), theta_values.end());
  auto* stack = stan::math::ChainableStack::instance_;
  const size_t chain_first = stack->var_stack_.size();
  const size_t nochain_first = stack->var_nochain_stack_.size();

  std::vector<T> out;
  if constexpr (Staged) {
    std::vector<T> staged_y(y.begin(), y.end());
    std::vector<T> staged_theta(theta.begin(), theta.end());
    const T staged_t(t);
    stanli::run_rhs<T>(p, staged_t, staged_y.data(), staged_theta.data(),
                       staged_theta.size(), x_r.data(), out);
  } else {
    stanli::run_rhs<T>(p, t, y.data(), theta.data(), theta.size(), x_r.data(),
                       out);
  }

  MixedRun run;
  for (const T& value : out)
    run.values.push_back(bits(stan::math::value_of(value)));
  run.chain_tape = tape_bits(stack->var_stack_, chain_first);
  run.nochain_tape = tape_bits(stack->var_nochain_stack_, nochain_first);

  if constexpr (std::is_same_v<T, stan::math::var>) {
    const stan::math::var root = out.at(0) * 0.37 + out.at(1) * -0.29;
    stan::math::grad(root.vi_);
  }
  run.y_grads.reserve(y.size());
  for (size_t i = 0; i < y.size(); ++i) {
    if constexpr (YAutodiff)
      run.y_grads.push_back(bits(y[i].adj()));
    else
      run.y_grads.push_back(bits(0.0));
  }
  run.theta_grads.reserve(theta.size());
  for (size_t i = 0; i < theta.size(); ++i) {
    if constexpr (ThetaAutodiff)
      run.theta_grads.push_back(bits(theta[i].adj()));
    else
      run.theta_grads.push_back(bits(0.0));
  }
  return run;
}

template <bool YAutodiff, bool ThetaAutodiff>
void check_mixed_seed(const stanli::RhsProgram& p, const char* label) {
  const std::vector<double> y{1.1, 0.7};
  // The fifth value models lower_ode_variadic's unread scalar placeholder.
  // The program consumes four, but the old staging vector promoted all five.
  const std::vector<double> theta{0.2, 0.35, 0.17, 0.41, 19.25};
  const std::vector<double> x_r{2.5, 1.25};
  const MixedRun staged =
      mixed_run<YAutodiff, ThetaAutodiff, true>(p, 0.73, y, theta, x_r);
  const MixedRun direct =
      mixed_run<YAutodiff, ThetaAutodiff, false>(p, 0.73, y, theta, x_r);
  const std::string prefix = std::string("mixed seed ") + label + ": ";
  expect(prefix + "output bits", direct.values == staged.values);
  expect(prefix + "y gradient bits", direct.y_grads == staged.y_grads);
  expect(prefix + "theta gradient bits",
         direct.theta_grads == staged.theta_grads);
  expect(prefix + "chain tape order", direct.chain_tape == staged.chain_tape);
  expect(prefix + "nochain tape order",
         direct.nochain_tape == staged.nochain_tape);
}

}  // namespace

int main() {
  using namespace stanli;

  mir::Program prog =
      mir::read_program(sexp::parse(slurp("tests/fixtures/odefns.tmir.sexp")));
  std::map<std::string, const mir::FunDef*> funs;
  for (const auto& f : prog.fun_defs) funs[f.name] = &f;

  const std::vector<double> x_r{2.5, 1.25};
  const std::vector<int> x_i{3};

  struct Case {
    const char* name;
    int n_y, n_th;
    bool want_ok;
  };
  const Case cases[] = {
      {"f_lin", 2, 4, true},
      {"f_branch", 2, 4, true},
      {"f_udf", 2, 4, true},
      {"f_early", 2, 4, false},  // return out of a runtime branch
  };
  for (const Case& c : cases) {
    auto it = funs.find(c.name);
    if (it == funs.end()) {
      ++failures;
      std::printf("FAIL fixture has no function %s\n", c.name);
      continue;
    }
    check(c.name, *it->second, funs, c.n_y, c.n_th, x_r, x_i, c.want_ok);
  }

  // stan-math instantiates a var state whenever either side is active. The
  // data-y/active-theta case is included too: run_rhs is a generic boundary,
  // and this is the combination most likely to expose a changed y promotion.
  {
    const auto it = funs.find("f_lin");
    const RhsProgram p = compile_rhs(*it->second, funs, 2, 4, 2, x_i);
    expect("mixed seed fixture compiles", p.ok);
    if (p.ok) {
      check_mixed_seed<true, false>(p, "var/double");
      check_mixed_seed<false, true>(p, "double/var");
      check_mixed_seed<true, true>(p, "var/var");
      check_mixed_seed<false, false>(p, "double/double");
    }
  }

  // A right-hand side whose arity is not the integrate_ode_* one is refused
  // rather than mis-bound.
  {
    auto it = funs.find("scale");
    RhsProgram p = compile_rhs(*it->second, funs, 2, 4, 2, x_i);
    if (p.ok) {
      ++failures;
      std::printf("FAIL scale/2 compiled as a right-hand side\n");
    }
  }

  if (failures == 0) std::printf("test_ode_prog OK\n");
  return failures == 0 ? 0 : 1;
}
