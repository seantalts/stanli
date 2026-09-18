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
#include <memory>
#include <string>
#include <tuple>
#include <vector>

namespace stanli {

struct IslandProg;

struct RhsProgram : Program {
  // Where run_rhs deposits the call arguments.
  int t_reg = -1, y0 = -1, yp0 = -1, th0 = -1, xr0 = -1;
  int n_y = 0, n_yp = 0, n_th = 0, n_xr = 0;
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

// Compile a DAE residual with the convention (t, y, y', ...args). The result
// uses the same register program and packed variadic argument representation
// as an ODE RHS, with one additional seeded state-derivative region.
RhsProgram compile_dae_args(
    const mir::FunDef& f, const std::map<std::string, const mir::FunDef*>& funs,
    int n_y, const std::vector<RhsArg>& args);

// The deprecated interface's fixed (t, y, theta, x_r, x_i) convention,
// expressed in the same terms.
RhsProgram compile_rhs(const mir::FunDef& f,
                       const std::map<std::string, const mir::FunDef*>& funs,
                       int n_y, int n_theta, int n_x_r,
                       const std::vector<int>& x_i);

// Build an immutable double-forward/generated-reverse derivative payload from
// a compiled RHS. The canonical RhsProgram is copied before checkpoint saves
// are inserted, so callers always retain it as the exact var-replay oracle.
// Programs with runtime control flow or another unsupported derivative opcode
// return null and keep that oracle path.
std::shared_ptr<const IslandProg> make_rhs_adjoint_program(
    const RhsProgram& rhs, std::string* refusal = nullptr);

// A callback owns its registers for the solve, including deferred adjoint
// callbacks. Copies own independent buffers; no storage survives thread exit.
struct RhsWorkspace {
  std::tuple<std::vector<double>, std::vector<stan::math::var>> registers;

  template <typename T>
  std::vector<T>& get() {
    return std::get<std::vector<T>>(registers);
  }
};

namespace detail {

template <typename T, typename T_time, typename T_y, typename T_theta>
void seed_rhs_regs(const RhsProgram& p, const T_time& t, const T_y* y,
                   const T_theta* th, size_t n_th_source, const double* xr,
                   std::vector<T>& reg) {
  if ((int)reg.size() < p.n_regs) reg.resize((size_t)p.n_regs);
  for (int i = 0; i < p.n_y; ++i) reg[(size_t)(p.y0 + i)] = T(y[i]);
  for (int i = 0; i < p.n_th; ++i) reg[(size_t)(p.th0 + i)] = T(th[i]);
  for (size_t i = (size_t)p.n_th; i < n_th_source; ++i) {
    [[maybe_unused]] const T promoted(th[i]);
  }
  reg[(size_t)p.t_reg] = T(t);
  for (int i = 0; i < p.n_xr; ++i) reg[(size_t)(p.xr0 + i)] = T(xr[i]);
}

template <typename T, typename T_time, typename T_y, typename T_yp,
          typename T_theta>
void seed_dae_regs(const RhsProgram& p, const T_time& t, const T_y* y,
                   const T_yp* yp, const T_theta* th, size_t n_th_source,
                   const double* xr, std::vector<T>& reg) {
  if ((int)reg.size() < p.n_regs) reg.resize((size_t)p.n_regs);
  for (int i = 0; i < p.n_y; ++i) reg[(size_t)(p.y0 + i)] = T(y[i]);
  for (int i = 0; i < p.n_yp; ++i) reg[(size_t)(p.yp0 + i)] = T(yp[i]);
  for (int i = 0; i < p.n_th; ++i) reg[(size_t)(p.th0 + i)] = T(th[i]);
  for (size_t i = (size_t)p.n_th; i < n_th_source; ++i) {
    [[maybe_unused]] const T promoted(th[i]);
  }
  reg[(size_t)p.t_reg] = T(t);
  for (int i = 0; i < p.n_xr; ++i) reg[(size_t)(p.xr0 + i)] = T(xr[i]);
}

template <typename T, typename T_time, typename T_y, typename T_yp,
          typename T_theta>
std::vector<T>& eval_dae_regs(const RhsProgram& p, const T_time& t,
                              const T_y* y, const T_yp* yp, const T_theta* th,
                              size_t n_th_source, const double* xr,
                              std::vector<T>& reg) {
  seed_dae_regs<T>(p, t, y, yp, th, n_th_source, xr, reg);
  run_program(p, reg);
  return reg;
}

template <typename T, typename T_time, typename T_y, typename T_theta>
std::vector<T>& eval_rhs_regs(const RhsProgram& p, const T_time& t,
                              const T_y* y, const T_theta* th,
                              size_t n_th_source, const double* xr,
                              std::vector<T>& reg) {
  seed_rhs_regs<T>(p, t, y, th, n_th_source, xr, reg);

  run_program(p, reg);
  return reg;
}

}  // namespace detail

template <typename T, typename T_time, typename T_y, typename T_yp,
          typename T_theta>
void run_dae_into(const RhsProgram& p, const T_time& t, const T_y* y,
                  const T_yp* yp, const T_theta* th, size_t n_th_source,
                  const double* xr, T* out, std::vector<T>& registers) {
  const std::vector<T>& reg =
      detail::eval_dae_regs<T>(p, t, y, yp, th, n_th_source, xr, registers);
  for (size_t i = 0; i < p.out_regs.size(); ++i)
    out[i] = reg[(size_t)p.out_regs[i]];
}

// Caller-owned output form for hot callback adapters. The destination must
// hold p.out_regs.size() scalars and must not alias the reusable register file.
// Unlike the vector wrapper below this performs no output allocation.
template <typename T, typename T_time, typename T_y, typename T_theta>
void run_rhs_into(const RhsProgram& p, const T_time& t, const T_y* y,
                  const T_theta* th, size_t n_th_source, const double* xr,
                  T* out, std::vector<T>& registers) {
  const std::vector<T>& reg =
      detail::eval_rhs_regs<T>(p, t, y, th, n_th_source, xr, registers);
  for (size_t i = 0; i < p.out_regs.size(); ++i)
    out[i] = reg[(size_t)p.out_regs[i]];
}

template <typename T, typename T_time, typename T_y, typename T_theta>
void run_rhs(const RhsProgram& p, const T_time& t, const T_y* y,
             const T_theta* th, size_t n_th_source, const double* xr,
             std::vector<T>& out, std::vector<T>& registers) {
  const std::vector<T>& reg =
      detail::eval_rhs_regs<T>(p, t, y, th, n_th_source, xr, registers);

  // Keep resize after the program replay, as in the original adapter. Besides
  // retaining allocation behavior for existing callers, this leaves the
  // observable var/nochain tape order unchanged.
  out.resize(p.out_regs.size());
  for (size_t i = 0; i < p.out_regs.size(); ++i)
    out[i] = reg[(size_t)p.out_regs[i]];
}

// Compatibility entry for the register program's existing callers. A normal
// RHS has exactly p.n_th source values; the ODE adapter above uses the sized
// overload when lowering supplied an extra, deliberately unread placeholder.
template <typename T, typename T_time, typename T_y, typename T_theta>
void run_rhs(const RhsProgram& p, const T_time& t, const T_y* y,
             const T_theta* th, const double* xr, std::vector<T>& out,
             std::vector<T>& registers) {
  run_rhs<T>(p, t, y, th, (size_t)p.n_th, xr, out, registers);
}

template <typename T, typename T_time, typename T_y, typename T_theta>
void run_rhs_into(const RhsProgram& p, const T_time& t, const T_y* y,
                  const T_theta* th, const double* xr, T* out,
                  std::vector<T>& registers) {
  run_rhs_into<T>(p, t, y, th, (size_t)p.n_th, xr, out, registers);
}

}  // namespace stanli

#endif
