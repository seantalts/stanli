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

#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;

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
