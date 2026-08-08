// Compile-time description of an ODE solve, attached to its op through the
// graph's user-data channel. Owns everything the integrator needs that is
// fixed at lowering time: the right-hand side's MIR, the function table it
// may call into, the solve times, the data arrays, and the tolerances.
#ifndef STANLI_ODE_HPP
#define STANLI_ODE_HPP

#include <stanli/mir.hpp>
#include <stanli/ode_prog.hpp>

#include <map>
#include <string>
#include <vector>

namespace stanli {

struct OdeSpec {
  // The spec outlives lowering (the graph does), so it owns copies of the
  // functions it may call rather than pointing into the parsed program.
  std::map<std::string, mir::FunDef> owned;
  std::map<std::string, const mir::FunDef*> funs_map;
  std::string rhs_name;

  void adopt(const std::map<std::string, const mir::FunDef*>& src) {
    for (const auto& [name, def] : src) owned[name] = *def;
    for (const auto& [name, def] : owned) funs_map[name] = &def;
  }
  const mir::FunDef* rhs() const {
    auto it = owned.find(rhs_name);
    return it == owned.end() ? nullptr : &it->second;
  }
  const std::map<std::string, const mir::FunDef*>* funs() const {
    return &funs_map;
  }
  double t0 = 0;
  std::vector<double> ts;
  std::vector<double> x_r;
  std::vector<int> x_i;
  double rtol = 1e-6, atol = 1e-6;
  long max_steps = 1000000;
  bool stiff = false;  // bdf when true, rk45 otherwise (deprecated path)
  // Which integrator, for the modern ode_* family. These are genuinely
  // different methods, not aliases: Adams and BDF are both CVODES
  // multistep but with different stability, and CKRK is a different
  // Runge-Kutta tableau from RK45. On an easy system they agree to
  // solver tolerance, which is exactly why running the wrong one would
  // pass a casual test and fail the user who chose it for stiffness.
  enum Solver { RK45, BDF, ADAMS, CKRK };
  Solver solver = RK45;
  // True for integrate_ode_*, whose call goes through the deprecated
  // stan-math entry points. Those delegate to the same *_tol_impl the
  // modern ones use, but keeping the call site distinct is what leaves
  // the four corpus ODE models running the exact code they were
  // verified against.
  bool legacy = false;
  // The right-hand side's arguments after (t, y), in declaration order.
  // The deprecated interface always fills this with exactly three --
  // theta, x_r, x_i -- and the variadic ode_* interface with however many
  // the call passed. It is what lets the INTERPRETER fallback split the
  // packed theta and x_r back into the individual formal parameters; the
  // compiled program has the same information baked into its register
  // ranges. Without it the fallback could only serve the deprecated
  // shape, and a right-hand side the compiler cannot take would lose
  // coverage rather than lose speed.
  std::vector<RhsArg> args;
  // The right-hand side, compiled. Falls back to the MIR interpreter when
  // `prog.ok` is false; `prog.why` says what stopped it.
  RhsProgram prog;
};

}  // namespace stanli

#endif
