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

namespace {

// The MIR spells an initialized local as its language-level default fill
// followed by a copy of the initializer, and copies values again through
// return temporaries. Native C++ optimization removes that bookkeeping. An
// ODE right-hand side otherwise pays it on every solver callback, including
// under var where a dead constant also allocates a disconnected tape node.
//
// Keep this deliberately narrower than a general Program optimizer. ODE
// scalar arithmetic has no effects, range reads, densities, or kernel calls;
// an unfamiliar instruction leaves the program unchanged. Dead constants
// may disappear only before the next control-flow edge, and a MOV aliases its
// source only when both registers have stable single definitions. Branch
// joins therefore retain their initialized value.
bool scalar_rhs_instruction(Program::Code code) {
  switch (code) {
    case Program::CONST:
    case Program::CONSTR:
    case Program::MOV:
    case Program::ADD:
    case Program::SUB:
    case Program::MUL:
    case Program::DIV:
    case Program::POW:
    case Program::FMAX:
    case Program::FMIN:
    case Program::NEG:
    case Program::EXP:
    case Program::LOG:
    case Program::SQRT:
    case Program::SQUARE:
    case Program::INV:
    case Program::FABS:
    case Program::INV_LOGIT:
    case Program::LOG1M:
    case Program::TANH:
    case Program::GT:
    case Program::GE:
    case Program::LT:
    case Program::LE:
    case Program::EQ:
    case Program::NE:
    case Program::JZ:
    case Program::JMP:
    case Program::LSE2:
    case Program::LOG_MIX:
    case Program::FMA:
      return true;
    default:
      return false;
  }
}

bool binary_rhs_instruction(Program::Code code) {
  switch (code) {
    case Program::ADD:
    case Program::SUB:
    case Program::MUL:
    case Program::DIV:
    case Program::POW:
    case Program::FMAX:
    case Program::FMIN:
    case Program::GT:
    case Program::GE:
    case Program::LT:
    case Program::LE:
    case Program::EQ:
    case Program::NE:
    case Program::LSE2:
      return true;
    default:
      return false;
  }
}

bool unary_rhs_instruction(Program::Code code) {
  switch (code) {
    case Program::MOV:
    case Program::NEG:
    case Program::EXP:
    case Program::LOG:
    case Program::SQRT:
    case Program::SQUARE:
    case Program::INV:
    case Program::FABS:
    case Program::INV_LOGIT:
    case Program::LOG1M:
    case Program::TANH:
    case Program::JZ:
      return true;
    default:
      return false;
  }
}

bool reads_rhs_register(const Program::Instr& instr, int reg) {
  if (unary_rhs_instruction(instr.code)) return instr.a == reg;
  if (binary_rhs_instruction(instr.code))
    return instr.a == reg || instr.b == reg;
  if (instr.code == Program::LOG_MIX || instr.code == Program::FMA)
    return instr.a == reg || instr.b == reg || instr.c == reg;
  return false;
}

bool writes_rhs_register(const Program::Instr& instr, int reg) {
  if (instr.code == Program::JZ || instr.code == Program::JMP) return false;
  if (instr.code == Program::CONSTR)
    return reg >= instr.dst && reg < instr.dst + instr.len;
  return instr.dst == reg;
}

void rewrite_rhs_reads(Program::Instr& instr, const std::vector<int>& alias) {
  if (unary_rhs_instruction(instr.code)) {
    instr.a = alias[(size_t)instr.a];
    return;
  }
  if (binary_rhs_instruction(instr.code)) {
    instr.a = alias[(size_t)instr.a];
    instr.b = alias[(size_t)instr.b];
    return;
  }
  if (instr.code == Program::LOG_MIX || instr.code == Program::FMA) {
    instr.a = alias[(size_t)instr.a];
    instr.b = alias[(size_t)instr.b];
    instr.c = alias[(size_t)instr.c];
  }
}

void optimize_scalar_rhs(RhsProgram& p) {
  if (!std::all_of(p.code.begin(), p.code.end(), [](const auto& instr) {
        return scalar_rhs_instruction(instr.code);
      }))
    return;

  const size_t n = p.code.size();
  std::vector<bool> remove(n, false);

  // A scalar declaration fill overwritten before any read or branch is
  // disconnected bookkeeping. Stop at a jump even when a later assignment
  // looks unconditional in the linear instruction array.
  for (size_t i = 0; i < n; ++i) {
    const Program::Instr& init = p.code[i];
    if (init.code != Program::CONST) continue;
    for (size_t j = i + 1; j < n; ++j) {
      const Program::Instr& next = p.code[j];
      if (next.code == Program::JZ || next.code == Program::JMP) break;
      if (reads_rhs_register(next, init.dst)) break;
      if (writes_rhs_register(next, init.dst)) {
        remove[i] = true;
        break;
      }
    }
  }

  std::vector<int> writers((size_t)p.n_regs, 0);
  std::vector<int> writer_pc((size_t)p.n_regs, -1);
  for (size_t pc = 0; pc < n; ++pc) {
    if (remove[pc]) continue;
    const Program::Instr& instr = p.code[pc];
    if (instr.code == Program::JZ || instr.code == Program::JMP) continue;
    const int len = instr.code == Program::CONSTR ? instr.len : 1;
    for (int k = 0; k < len; ++k) {
      const size_t reg = (size_t)(instr.dst + k);
      ++writers[reg];
      writer_pc[reg] = (int)pc;
    }
  }

  std::vector<int> alias((size_t)p.n_regs);
  for (int reg = 0; reg < p.n_regs; ++reg) alias[(size_t)reg] = reg;
  for (size_t pc = 0; pc < n; ++pc) {
    if (remove[pc]) continue;
    Program::Instr& instr = p.code[pc];
    rewrite_rhs_reads(instr, alias);
    if (instr.code == Program::MOV && writers[(size_t)instr.dst] == 1 &&
        writers[(size_t)instr.a] <= 1 && writer_pc[(size_t)instr.a] < (int)pc) {
      alias[(size_t)instr.dst] = instr.a;
      remove[pc] = true;
      continue;
    }
    if (instr.code == Program::JZ || instr.code == Program::JMP) continue;
    const int len = instr.code == Program::CONSTR ? instr.len : 1;
    for (int k = 0; k < len; ++k)
      alias[(size_t)(instr.dst + k)] = instr.dst + k;
  }
  for (int& reg : p.out_regs) reg = alias[(size_t)reg];

  std::vector<int> new_pc(n + 1, 0);
  int at = 0;
  for (size_t pc = 0; pc < n; ++pc) {
    new_pc[pc] = at;
    if (!remove[pc]) ++at;
  }
  new_pc[n] = at;
  std::vector<Program::Instr> compact;
  compact.reserve((size_t)at);
  for (size_t pc = 0; pc < n; ++pc) {
    if (remove[pc]) continue;
    Program::Instr instr = p.code[pc];
    if (instr.code == Program::JZ || instr.code == Program::JMP)
      instr.dst = new_pc[(size_t)instr.dst];
    compact.push_back(instr);
  }
  p.code = std::move(compact);
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

void stamp_rhs_view(Range* range, const mir::UnsizedView& view) {
  if (view.depth == 1)
    range->kind = ViewKind::Array;
  else if (view.leaf == mir::UnsizedLeaf::Vector)
    range->kind = ViewKind::Vector;
  else if (view.leaf == mir::UnsizedLeaf::RowVector)
    range->kind = ViewKind::RowVector;
}

}  // namespace

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
    optimize_scalar_rhs(p);
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
