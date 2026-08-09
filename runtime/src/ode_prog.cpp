// The ODE entry into the MIR compiler (mir_prog.hpp). All this adds is
// the integrate_ode_* calling convention: the signature fixes the
// argument order and the sizes, so t, y, theta and x_r get their register
// ranges up front and x_i binds as compile-time integers. Everything the
// body can contain is the shared compiler's problem.
#include <stanli/ode_prog.hpp>

#include <stanli/mir_prog.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace stanli {

RhsProgram compile_rhs_args(
    const mir::FunDef& f, const std::map<std::string, const mir::FunDef*>& funs,
    int n_y, const std::vector<RhsArg>& args) {
  RhsProgram p;
  if (f.arg_names.size() != args.size() + 2) {
    p.why = "right-hand side takes " + std::to_string(f.arg_names.size()) +
            " arguments, the call passes " + std::to_string(args.size() + 2) +
            " (t, y, and " + std::to_string(args.size()) + " more)";
    return p;
  }
  ProgramCompiler c{p, funs};
  try {
    // Two contiguous regions, so run_rhs can seed each with one loop: the
    // autodiff arguments and the data ones. Each formal parameter gets a
    // sub-range of whichever region it belongs to, assigned in argument
    // order -- the same order the lowering concatenates the call site in.
    int n_th = 0, n_xr = 0;
    for (const auto& a : args) {
      if (a.is_int) continue;
      (a.is_param ? n_th : n_xr) += a.len;
    }
    p.t_reg = c.alloc(1);
    p.y0 = c.alloc(n_y);
    p.th0 = c.alloc(n_th);
    p.xr0 = c.alloc(n_xr);
    p.n_y = n_y;
    p.n_th = n_th;
    p.n_xr = n_xr;
    c.reals[f.arg_names[0]] = Range{p.t_reg, 1};
    c.reals[f.arg_names[1]] = Range{p.y0, n_y};
    int th_at = 0, xr_at = 0;
    for (size_t k = 0; k < args.size(); ++k) {
      const RhsArg& a = args[k];
      const std::string& name = f.arg_names[k + 2];
      if (a.is_int) {
        c.ints[name] = std::vector<long>(a.ints.begin(), a.ints.end());
      } else if (a.is_param) {
        c.reals[name] = Range{p.th0 + th_at, a.len};
        th_at += a.len;
      } else {
        c.reals[name] = Range{p.xr0 + xr_at, a.len};
        xr_at += a.len;
      }
    }

    Range out{0, 0};
    try {
      for (const auto& s : f.body) c.stmt(s);
      c.bail("right-hand side returned no value");
    } catch (ProgramCompiler::Returned& r) {
      out = r.r;
    }
    if (out.len != n_y)
      c.bail("right-hand side returns " + std::to_string(out.len) +
             " values for " + std::to_string(n_y) + " states");
    for (int k = 0; k < out.len; ++k) p.out_regs.push_back(out.reg + k);
    p.ok = true;
  } catch (Bail& b) {
    p.ok = false;
    p.why = b.why;
    p.code.clear();
    p.out_regs.clear();
  }
  return p;
}

RhsProgram compile_rhs(const mir::FunDef& f,
                       const std::map<std::string, const mir::FunDef*>& funs,
                       int n_y, int n_theta, int n_x_r,
                       const std::vector<int>& x_i) {
  // integrate_ode_*'s fixed convention is three variadic arguments: theta
  // is the autodiff one, x_r the data one, x_i the integer one.
  if (f.arg_names.size() != 5) {
    RhsProgram p;
    p.why = "right-hand side does not take (t, y, theta, x_r, x_i)";
    return p;
  }
  std::vector<RhsArg> args(3);
  args[0].is_param = true;
  args[0].len = n_theta;
  args[1].len = n_x_r;
  args[2].is_int = true;
  args[2].ints = x_i;
  return compile_rhs_args(f, funs, n_y, args);
}

}  // namespace stanli
