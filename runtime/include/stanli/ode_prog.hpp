// A compiled ODE right-hand side.
//
// Every other user-defined function is inlined at lowering time. An ODE
// right-hand side cannot be: the integrator picks the times, so the body has
// to stay callable at runtime, on double for the state solve and on var for
// the jacobian stan-math takes at every step. It was therefore evaluated by a
// tree-walking interpreter over the MIR (mir_interp.hpp), which costs a
// std::map lookup per variable reference and a std::vector allocation per
// intermediate -- 5.8 us per call on lotka_volterra's two-line right-hand
// side, against roughly 500 calls per gradient. That interpreter was 97% of
// the model's gradient time.
//
// This compiles the same MIR once, at lowering time, into a flat register
// machine: names become indices, loops with data-known bounds unroll,
// data-only conditions fold away, and evaluation is a switch over a
// contiguous instruction array with no allocation and no lookups. Conditions
// on runtime values (`if (t > 0)`, a dosing schedule) become branches.
//
// Anything it cannot compile leaves `ok` false with a reason, and the caller
// falls back to the interpreter, so coverage never shrinks -- only speed.
#ifndef STANLI_ODE_PROG_HPP
#define STANLI_ODE_PROG_HPP

#include <stanli/mir.hpp>
#include <stanli/program.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace stanli {

struct RhsProgram : Program {
  // Where run_rhs deposits the call arguments.
  int t_reg = -1, y0 = -1, th0 = -1, xr0 = -1;
  int n_y = 0, n_th = 0, n_xr = 0;
  bool ok = false;
  std::string why;  // why not, when !ok
};

// One argument of a right-hand side, after (t, y).
//
// The deprecated `integrate_ode_*` interface fixes exactly three of these
// -- theta, x_r, x_i -- and the modern `ode_*` interface takes any number
// of any type. Both reduce to this list, so there is one calling
// convention: real arguments are packed in order into the theta region
// when they carry autodiff and into the x_r region when they are data,
// and integer arguments bind as compile-time constants. The lowering
// packs the call site the same way, in the same order, which is what
// makes the two halves agree.
struct RhsArg {
  bool is_int = false;
  bool is_param = false;  // reals: theta region when true, x_r when false
  int len = 0;            // reals
  std::vector<int> ints;  // ints
};

// Compile `f` against a variadic argument list. Never throws: failure
// comes back as ok == false with a reason.
RhsProgram compile_rhs_args(
    const mir::FunDef& f, const std::map<std::string, const mir::FunDef*>& funs,
    int n_y, const std::vector<RhsArg>& args);

// The deprecated interface's fixed (t, y, theta, x_r, x_i) convention,
// expressed in the same terms.
RhsProgram compile_rhs(const mir::FunDef& f,
                       const std::map<std::string, const mir::FunDef*>& funs,
                       int n_y, int n_theta, int n_x_r,
                       const std::vector<int>& x_i);

// Evaluate. The register file is reused between calls (one per scalar type),
// which is what makes a call allocation-free; the compiler guarantees every
// register is written before it is read, so leftovers are never observed.
// Not reentrant, which is fine: an ODE right-hand side cannot solve an ODE.
template <typename T>
void run_rhs(const RhsProgram& p, const T& t, const T* y, const T* th,
             const double* xr, std::vector<T>& out) {
  static thread_local std::vector<T> reg;
  if ((int)reg.size() < p.n_regs) reg.resize((size_t)p.n_regs);
  reg[(size_t)p.t_reg] = t;
  for (int i = 0; i < p.n_y; ++i) reg[(size_t)(p.y0 + i)] = y[i];
  for (int i = 0; i < p.n_th; ++i) reg[(size_t)(p.th0 + i)] = th[i];
  for (int i = 0; i < p.n_xr; ++i) reg[(size_t)(p.xr0 + i)] = T(xr[i]);

  run_program(p, reg);

  out.resize(p.out_regs.size());
  for (size_t i = 0; i < p.out_regs.size(); ++i)
    out[i] = reg[(size_t)p.out_regs[i]];
}

}  // namespace stanli

#endif
