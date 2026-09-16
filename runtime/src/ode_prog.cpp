// The ODE entry into the MIR compiler (mir_prog.hpp). All this adds is
// the integrate_ode_* calling convention: the signature fixes the
// argument order and the sizes, so t, y, theta and x_r get their register
// ranges up front and x_i binds as compile-time integers. Everything the
// body can contain is the shared compiler's problem.
#include <stanli/ode_prog.hpp>

#include <stanli/island.hpp>
#include <stanli/mir_prog.hpp>

#include <stdexcept>

namespace stanli {

namespace {

// The register regions run_rhs seeds before the program starts, in the order
// the fields appear on RhsProgram.
void compact_rhs(RhsProgram& p) {
  std::vector<std::pair<int, int>> seeded{{p.t_reg, 1},
                                          {p.y0, p.n_y},
                                          {p.yp0, p.n_yp},
                                          {p.th0, p.n_th},
                                          {p.xr0, p.n_xr}};
  compact_program(p, seeded);
  p.t_reg = seeded[0].first;
  p.y0 = seeded[1].first;
  p.yp0 = seeded[2].first;
  p.th0 = seeded[3].first;
  p.xr0 = seeded[4].first;
}

bool supported_rhs_view(const mir::UnsizedView& view) {
  if (view.depth > 1) return false;
  if (view.depth == 1)
    return view.leaf == mir::UnsizedLeaf::Real ||
           view.leaf == mir::UnsizedLeaf::Int;
  return view.leaf == mir::UnsizedLeaf::Real ||
         view.leaf == mir::UnsizedLeaf::Int ||
         view.leaf == mir::UnsizedLeaf::Vector ||
         view.leaf == mir::UnsizedLeaf::RowVector;
}

// The direct RK path compares against a var replay, so admitting a derivative
// rule is not enough: its double forward must also preserve the replay's exact
// value grouping. Keep this an explicit whitelist so a new Program opcode
// fails closed. In particular DOT uses Eigen packet reduction for double and
// Stan's scalar dot_product for var; one ULP can change adaptive step history.
bool exact_ode_adjoint_opcode(Program::Code code) {
  switch (code) {
    case Program::CONST:
    case Program::FILL:
    case Program::CONSTR:
    case Program::MOV:
    case Program::MOVR:
    case Program::ADD:
    case Program::SUB:
    case Program::MUL:
    case Program::DIV:
    case Program::POW:
    case Program::NEG:
    case Program::EXP:
    case Program::LOG:
    case Program::SQRT:
    case Program::SQUARE:
    case Program::INV:
    case Program::FABS:
    case Program::INV_LOGIT:
    case Program::LOG1M:
    case Program::LOG1P_EXP:
    case Program::TANH:
    case Program::GT:
    case Program::GE:
    case Program::LT:
    case Program::LE:
    case Program::EQ:
    case Program::NE:
    case Program::LOG_RANGE:
    case Program::EXP_RANGE:
    case Program::LSE2:
    case Program::LOG_MIX:
    case Program::FMA:
      return true;
    default:
      return false;
  }
}

void stamp_rhs_view(Range* range, const mir::UnsizedView& view) {
  if (view.depth == 1)
    range->kind = ViewKind::Array;
  else if (view.leaf == mir::UnsizedLeaf::Vector)
    range->kind = ViewKind::Vector;
  else if (view.leaf == mir::UnsizedLeaf::RowVector)
    range->kind = ViewKind::RowVector;
}

}  // namespace

RhsProgram compile_dae_args(
    const mir::FunDef& f, const std::map<std::string, const mir::FunDef*>& funs,
    int n_y, const std::vector<RhsArg>& args) {
  RhsProgram p;
  if (f.arg_names.size() != args.size() + 3) {
    p.why = "DAE residual takes " + std::to_string(f.arg_names.size()) +
            " arguments, the call passes " + std::to_string(args.size() + 3) +
            " (t, y, y', and " + std::to_string(args.size()) + " more)";
    return p;
  }
  if (f.arg_views.size() != f.arg_names.size()) {
    p.why = "DAE residual has incomplete unsized argument metadata";
    return p;
  }
  for (size_t i = 0; i < f.arg_views.size(); ++i) {
    if (!supported_rhs_view(f.arg_views[i])) {
      p.why = "DAE residual argument " + std::to_string(i + 1) +
              " has an unsupported logical view";
      return p;
    }
  }
  ProgramCompiler c{p, funs};
  try {
    int n_th = 0, n_xr = 0;
    for (const auto& a : args) {
      if (a.is_int) continue;
      (a.is_param ? n_th : n_xr) += a.len;
    }
    p.t_reg = c.alloc(1);
    p.y0 = c.alloc(n_y);
    p.yp0 = c.alloc(n_y);
    p.th0 = c.alloc(n_th);
    p.xr0 = c.alloc(n_xr);
    p.n_y = n_y;
    p.n_yp = n_y;
    p.n_th = n_th;
    p.n_xr = n_xr;
    c.reals[f.arg_names[0]] = Range{p.t_reg, 1};
    Range y{p.y0, n_y};
    stamp_rhs_view(&y, f.arg_views[1]);
    c.reals[f.arg_names[1]] = y;
    Range yp{p.yp0, n_y};
    stamp_rhs_view(&yp, f.arg_views[2]);
    c.reals[f.arg_names[2]] = yp;
    int th_at = 0, xr_at = 0;
    for (size_t k = 0; k < args.size(); ++k) {
      const RhsArg& a = args[k];
      const std::string& name = f.arg_names[k + 3];
      if (a.is_int) {
        c.ints[name] = std::vector<long>(a.ints.begin(), a.ints.end());
      } else if (a.is_param) {
        Range r{p.th0 + th_at, a.len};
        stamp_rhs_view(&r, f.arg_views[k + 3]);
        c.reals[name] = r;
        th_at += a.len;
      } else {
        Range r{p.xr0 + xr_at, a.len};
        stamp_rhs_view(&r, f.arg_views[k + 3]);
        c.reals[name] = r;
        xr_at += a.len;
      }
    }
    Range out{0, 0};
    try {
      for (const auto& s : f.body) c.stmt(s);
      c.bail("DAE residual returned no value");
    } catch (ProgramCompiler::Returned& r) {
      out = r.r;
    }
    if (out.len != n_y)
      c.bail("DAE residual returns " + std::to_string(out.len) +
             " values for " + std::to_string(n_y) + " states");
    for (int k = 0; k < out.len; ++k) p.out_regs.push_back(out.reg + k);
    c.finish();
    compact_rhs(p);
    p.ok = true;
  } catch (Bail& b) {
    p.ok = false;
    p.why = b.why;
    p.code.clear();
    p.out_regs.clear();
  }
  return p;
}

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
  if (f.arg_views.size() != f.arg_names.size()) {
    p.why = "right-hand side has incomplete unsized argument metadata";
    return p;
  }
  for (size_t i = 0; i < f.arg_views.size(); ++i) {
    if (!supported_rhs_view(f.arg_views[i])) {
      p.why = "right-hand side argument " + std::to_string(i + 1) +
              " has an unsupported logical view";
      return p;
    }
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
    Range y{p.y0, n_y};
    stamp_rhs_view(&y, f.arg_views[1]);
    c.reals[f.arg_names[1]] = y;
    int th_at = 0, xr_at = 0;
    for (size_t k = 0; k < args.size(); ++k) {
      const RhsArg& a = args[k];
      const std::string& name = f.arg_names[k + 2];
      if (a.is_int) {
        c.ints[name] = std::vector<long>(a.ints.begin(), a.ints.end());
      } else if (a.is_param) {
        Range r{p.th0 + th_at, a.len};
        stamp_rhs_view(&r, f.arg_views[k + 2]);
        c.reals[name] = r;
        th_at += a.len;
      } else {
        Range r{p.xr0 + xr_at, a.len};
        stamp_rhs_view(&r, f.arg_views[k + 2]);
        c.reals[name] = r;
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
    c.finish();
    compact_rhs(p);
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

std::shared_ptr<const IslandProg> make_rhs_adjoint_program(
    const RhsProgram& rhs, std::string* refusal) {
  if (refusal) refusal->clear();
  if (!rhs.ok) {
    if (refusal) *refusal = "compiled RHS unavailable: " + rhs.why;
    return nullptr;
  }
  for (const Program::Instr& instruction : rhs.code) {
    if (exact_ode_adjoint_opcode(instruction.code)) continue;
    if (refusal)
      *refusal = std::string("opcode ") +
                 program_code_spec(instruction.code).name +
                 " is not in the exact direct-RK whitelist";
    return nullptr;
  }
  auto candidate = std::make_shared<IslandProg>();
  static_cast<Program&>(*candidate) = static_cast<const Program&>(rhs);
  candidate->ins.push_back(IslandProg::LiveIn{rhs.t_reg, 1, -1, 0, false});
  candidate->ins.push_back(IslandProg::LiveIn{rhs.y0, rhs.n_y, -1, 0, true});
  candidate->ins.push_back(IslandProg::LiveIn{rhs.th0, rhs.n_th, -1, 0, true});
  candidate->ins.push_back(IslandProg::LiveIn{rhs.xr0, rhs.n_xr, -1, 0, false});
  if (!gen_adjoint(*candidate)) {
    if (refusal) *refusal = "generated adjoint refused the compiled RHS";
    return nullptr;
  }
  return candidate;
}

}  // namespace stanli
