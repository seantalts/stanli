// MIR -> Program: the shared front end.
//
// Two callers compile MIR into the register machine. An ODE right-hand
// side has to stay callable at runtime because the integrator picks the
// times (ode_prog.hpp). A region whose control flow depends on a
// parameter has to become a program because it cannot become graph ops
// at all: `if (theta > 0)` has no op-graph form, and until this existed
// it was a compile error (lower.cpp).
//
// The shape of the problem is what keeps this small: every size, every
// index and every integer is known at compile time -- loop bounds come
// from data, array lengths from declarations -- so only the reals need
// registers, and loops unroll as they compile.
//
// Names the compiler does not know are the one thing that differs
// between callers. The ODE side knows them all up front (t, y, theta,
// x_r, x_i). Lowering does not: a region reads model-block variables
// that already live in graph slots, so it hands over `bind_extern`,
// which allocates registers for such a name and records it as a live-in.
#ifndef STANLI_MIR_PROG_HPP
#define STANLI_MIR_PROG_HPP

#include <stanli/mir.hpp>
#include <stanli/program.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace stanli {

// A value is a contiguous run of registers: scalars are runs of one, arrays
// and vectors are runs of their length.
struct Range {
  int reg = 0;
  int len = 0;
};

struct Bail {
  std::string why;
};

struct ProgramCompiler {
  Program& p;
  const std::map<std::string, const mir::FunDef*>& funs;
  std::map<std::string, Range> reals;
  std::map<std::string, std::vector<long>> ints;
  int branch_depth = 0;  // inside a branch on a runtime value
  int inline_depth = 0;
  // A name that is neither a local nor a compile-time integer. The ODE
  // caller leaves this empty (its arguments are all bound up front);
  // lowering installs a hook that allocates registers for the graph slot
  // backing the name and records it as a live-in. Returning false means
  // "no such value", and the compile bails.
  std::function<bool(const std::string&, Range*)> bind_extern;
  // Where `target +=` accumulates, or -1 when the region may not have
  // one. Set by the caller, which also seeds it to zero.
  int target_reg = -1;

  // Registers are never recycled. Right-hand sides are a few lines over a
  // handful of states, so the count stays in the dozens; the cap is a
  // backstop against a pathological unroll, and trips into the interpreter
  // rather than into a huge allocation.
  static constexpr int kMaxRegs = 1 << 16;

  [[noreturn]] void bail(const std::string& why) { throw Bail{why}; }

  int alloc(int n) {
    if (p.n_regs + n > kMaxRegs)
      bail("right-hand side needs too many registers");
    const int r = p.n_regs;
    p.n_regs += n;
    return r;
  }

  int emit(Program::Code c, int dst, int a = 0, int b = 0) {
    p.code.push_back(Program::Instr{c, dst, a, b, 0, 0});
    return (int)p.code.size() - 1;
  }

  // Constants live in the pool, not in the instruction. Equal values share
  // an entry: `declare` zeroes every register it allocates, so a program
  // over a few arrays would otherwise carry hundreds of copies of 0.
  int pool_at(const double* v, int n) {
    for (size_t s = 0; s + (size_t)n <= p.pool.size(); ++s)
      if (std::equal(v, v + n, p.pool.begin() + (long)s)) return (int)s;
    const int at = (int)p.pool.size();
    p.pool.insert(p.pool.end(), v, v + n);
    return at;
  }

  // dst[0..n) = the given values, as one instruction.
  void emit_const(int dst, const double* v, int n) {
    p.code.push_back(Program::Instr{n == 1 ? Program::CONST : Program::CONSTR,
                                    dst, pool_at(v, n), 0, 0, n});
  }

  int konst(double v) {
    const int r = alloc(1);
    emit_const(r, &v, 1);
    return r;
  }

  // ---- compile-time integers ----------------------------------------------
  long cint(const mir::Expr& e) {
    switch (e.kind) {
      case mir::Expr::LitInt:
        return e.lit_i;
      case mir::Expr::LitReal:
        return (long)e.lit;
      case mir::Expr::Var: {
        auto it = ints.find(e.name);
        if (it != ints.end() && it->second.size() == 1) return it->second[0];
        bail("integer " + e.name + " is not known at compile time");
      }
      case mir::Expr::Indexed: {
        if (e.args.size() != 2 || e.args[1].name != "IndexSingle")
          bail("integer index form");
        if (e.args[0].kind != mir::Expr::Var) bail("integer index base");
        auto it = ints.find(e.args[0].name);
        if (it == ints.end()) bail("integer array " + e.args[0].name);
        const long ix = cint(e.args[1].args[0]);
        if (ix < 1 || (size_t)ix > it->second.size())
          bail("integer index range");
        return it->second[(size_t)ix - 1];
      }
      case mir::Expr::FunApp:
        if (e.args.size() == 2) {
          if (e.name == "Plus__") return cint(e.args[0]) + cint(e.args[1]);
          if (e.name == "Minus__") return cint(e.args[0]) - cint(e.args[1]);
          if (e.name == "Times__") return cint(e.args[0]) * cint(e.args[1]);
          if (e.name == "IntDivide__" || e.name == "Divide__")
            return cint(e.args[0]) / cint(e.args[1]);
        }
        if (e.args.size() == 1 && e.name == "PMinus__") return -cint(e.args[0]);
        bail("integer function " + e.name);
      default:
        bail("integer expression");
    }
  }

  bool try_cint(const mir::Expr& e, long* out) {
    try {
      *out = cint(e);
      return true;
    } catch (Bail&) {
      return false;
    }
  }

  // ---- expressions ---------------------------------------------------------
  Range expr(const mir::Expr& e) {
    switch (e.kind) {
      case mir::Expr::LitInt:
        return {konst((double)e.lit_i), 1};
      case mir::Expr::LitReal:
        return {konst(e.lit), 1};
      case mir::Expr::Var: {
        auto it = reals.find(e.name);
        if (it != reals.end()) return it->second;
        auto ii = ints.find(e.name);
        if (ii != ints.end()) {
          const std::vector<double> vals(ii->second.begin(), ii->second.end());
          const int r = alloc((int)vals.size());
          emit_const(r, vals.data(), (int)vals.size());
          return {r, (int)vals.size()};
        }
        Range ext;
        if (bind_extern && bind_extern(e.name, &ext)) {
          reals[e.name] = ext;
          return ext;
        }
        bail("unknown variable " + e.name);
      }
      case mir::Expr::Indexed: {
        const Range b = expr(e.args[0]);
        if (e.args.size() == 2 && e.args[1].name == "IndexAll") return b;
        if (e.args.size() != 2 || e.args[1].name != "IndexSingle")
          bail("index form");
        const long ix = cint(e.args[1].args[0]);
        if (ix < 1 || ix > b.len) bail("index out of the declared range");
        return {b.reg + (int)ix - 1, 1};
      }
      case mir::Expr::TernaryIf: {
        long c;
        if (try_cint(e.args[0], &c)) return expr(e.args[c != 0 ? 1 : 2]);
        return branchy_select(e.args[0], e.args[1], e.args[2]);
      }
      case mir::Expr::EOr:
      case mir::Expr::EAnd: {
        // No short-circuit: Stan expressions are total, and the branchless
        // form keeps this out of the instruction stream's way.
        const Range a = expr(e.args[0]), b = expr(e.args[1]);
        if (a.len != 1 || b.len != 1) bail("logical operator on a container");
        const int z = konst(0.0), ta = alloc(1), tb = alloc(1), r = alloc(1);
        emit(Program::NE, ta, a.reg, z);
        emit(Program::NE, tb, b.reg, z);
        emit(e.kind == mir::Expr::EOr ? Program::FMAX : Program::FMIN, r, ta,
             tb);
        return {r, 1};
      }
      case mir::Expr::FunApp:
        return fun(e);
      default:
        bail("expression");
    }
  }

  // A ternary on a runtime condition: both arms write the same registers.
  Range branchy_select(const mir::Expr& c, const mir::Expr& a,
                       const mir::Expr& b) {
    const Range cv = expr(c);
    if (cv.len != 1) bail("conditional on a container");
    // Compile the arms first to learn the width, then re-emit into place.
    const int jz = emit(Program::JZ, 0, cv.reg);
    const Range av = expr(a);
    const int dst = alloc(av.len);
    for (int k = 0; k < av.len; ++k) emit(Program::MOV, dst + k, av.reg + k);
    const int jmp = emit(Program::JMP, 0);
    p.code[(size_t)jz].dst = (int)p.code.size();
    const Range bv = expr(b);
    if (bv.len != av.len) bail("conditional arms of different widths");
    for (int k = 0; k < bv.len; ++k) emit(Program::MOV, dst + k, bv.reg + k);
    p.code[(size_t)jmp].dst = (int)p.code.size();
    return {dst, av.len};
  }

  Range fun(const mir::Expr& e) {
    if (e.fn_lib == mir::Expr::Lib::UserDefined) {
      auto it = funs.find(e.name);
      if (it == funs.end()) bail("unknown function " + e.name);
      std::vector<Range> args;
      std::vector<std::vector<long>> iargs;
      for (const auto& a : e.args) {
        long v;
        if (a.type_ == "UInt" && try_cint(a, &v))
          iargs.push_back({v});
        else
          args.push_back(expr(a));
      }
      return inline_call(*it->second, args, iargs);
    }
    if (e.fn_lib == mir::Expr::Lib::Internal) {
      if (e.name == "FnMakeArray" || e.name == "FnMakeRowVec") {
        std::vector<Range> parts;
        int total = 0;
        for (const auto& a : e.args) {
          parts.push_back(expr(a));
          total += parts.back().len;
        }
        const int r = alloc(total);
        int at = 0;
        for (const Range& q : parts)
          for (int k = 0; k < q.len; ++k)
            emit(Program::MOV, r + at++, q.reg + k);
        return {r, total};
      }
      bail("internal function " + e.name);
    }
    // Ahead of the arity-keyed blocks below: those end in a bail on an
    // unknown name, so while this table sat after them a two-argument
    // density (exponential_lpdf) was unreachable -- listed as supported
    // and refused in practice.
    // Explicit density calls. `target += normal_lpdf(y | mu, s)` keeps
    // every constant, which is the propto-OFF instantiation the machine
    // has; a `~` statement's dropped-constant form depends on which
    // arguments are autodiff and is not expressible here.
    {
      Program::Code dc = Program::CONST;
      int arity = 0;
#define STANLI_PROG_DENSITY_NAME(code, fn, n) \
  if (e.name == #fn) {                        \
    dc = Program::code;                       \
    arity = n;                                \
  }
      STANLI_PROGRAM_DENSITY_LIST(STANLI_PROG_DENSITY_NAME)
#undef STANLI_PROG_DENSITY_NAME
      if (arity) {
        // A `~` statement lowers to the same call with propto set, and
        // which constants it drops depends on which arguments are
        // autodiff -- a distinction the program cannot make, since it
        // binds every argument the same way. Getting this wrong is
        // invisible in the gradient and shows up only in lp, so refuse
        // rather than approximate. (Measured, before this check: lp off
        // by exactly log(2*pi)/2 on a normal.)
        if (e.fn_propto)
          bail(
              "`~` inside a parameter-dependent region (write it as "
              "`target += " +
              e.name +
              "(...)`, which keeps every "
              "constant and is what the region can reproduce)");
        if ((int)e.args.size() != arity)
          bail(e.name + " takes " + std::to_string(arity) + " arguments here");
        Range av[3];
        for (int k = 0; k < arity; ++k) {
          av[k] = expr(e.args[(size_t)k]);
          // One lp per call: a vectorized density inside a branch would
          // have to sum over its arguments, which this does not do.
          if (av[k].len != 1) bail(e.name + " on a container");
        }
        const int r = alloc(1);
        p.code.push_back(Program::Instr{dc, r, av[0].reg,
                                        arity > 1 ? av[1].reg : 0,
                                        arity > 2 ? av[2].reg : 0, 0});
        return {r, 1};
      }
    }
    if (e.args.size() == 2) {
      const Range a = expr(e.args[0]), b = expr(e.args[1]);
      const int n = std::max(a.len, b.len);
      if ((a.len != 1 && a.len != n) || (b.len != 1 && b.len != n))
        bail("binary " + e.name + " on mismatched widths");
      Program::Code c;
      if (e.name == "Plus__")
        c = Program::ADD;
      else if (e.name == "Minus__")
        c = Program::SUB;
      else if (e.name == "Times__" || e.name == "EltTimes__")
        c = Program::MUL;
      else if (e.name == "Divide__" || e.name == "EltDivide__")
        c = Program::DIV;
      else if (e.name == "Pow__" || e.name == "pow")
        c = Program::POW;
      else if (e.name == "fmax")
        c = Program::FMAX;
      else if (e.name == "fmin")
        c = Program::FMIN;
      else if (e.name == "Greater__")
        c = Program::GT;
      else if (e.name == "Geq__")
        c = Program::GE;
      else if (e.name == "Less__")
        c = Program::LT;
      else if (e.name == "Leq__")
        c = Program::LE;
      else if (e.name == "Equals__")
        c = Program::EQ;
      else if (e.name == "NEquals__")
        c = Program::NE;
      else
        bail("function " + e.name);
      const int r = alloc(n);
      for (int i = 0; i < n; ++i)
        emit(c, r + i, a.reg + (a.len == 1 ? 0 : i),
             b.reg + (b.len == 1 ? 0 : i));
      return {r, n};
    }
    if (e.args.size() == 1) {
      const Range a = expr(e.args[0]);
      if (e.name == "sum") {
        const int r = alloc(1);
        emit(Program::MOV, r, a.reg);
        for (int i = 1; i < a.len; ++i) emit(Program::ADD, r, r, a.reg + i);
        return {r, 1};
      }
      Program::Code c;
      if (e.name == "PMinus__")
        c = Program::NEG;
      else if (e.name == "PPlus__")
        c = Program::MOV;
      else if (e.name == "exp")
        c = Program::EXP;
      else if (e.name == "log")
        c = Program::LOG;
      else if (e.name == "sqrt")
        c = Program::SQRT;
      else if (e.name == "square")
        c = Program::SQUARE;
      else if (e.name == "inv")
        c = Program::INV;
      else if (e.name == "fabs" || e.name == "abs")
        c = Program::FABS;
      else if (e.name == "inv_logit")
        c = Program::INV_LOGIT;
      else
        bail("function " + e.name);
      const int r = alloc(a.len);
      for (int i = 0; i < a.len; ++i) emit(c, r + i, a.reg + i);
      return {r, a.len};
    }
    bail("function " + e.name);
  }

  // ---- statements ----------------------------------------------------------
  struct Returned {
    Range r;
  };

  int64_t sized_len(const mir::SizedType& t) {
    int64_t n = 1;
    for (const auto& d : t.dims) n *= cint(d);
    return n;
  }

  // Declare (or redeclare) a real variable of `len` registers, filled.
  // Stan initializes a local to NaN; the ODE side has always zeroed and
  // its bodies assign before reading, so `fill` keeps both honest.
  Range declare(const std::string& name, int len, double fill = 0.0) {
    const Range r{alloc(len), len};
    const std::vector<double> init((size_t)len, fill);
    emit_const(r.reg, init.data(), len);
    reals[name] = r;
    return r;
  }

  void stmt(const mir::Stmt& s) {
    switch (s.kind) {
      case mir::Stmt::Decl: {
        if (s.decl_type.base == "SInt" ||
            (s.decl_type.base == "SArray" && s.decl_type.elem_base == "SInt")) {
          if (s.has_init) {
            ints[s.decl_id] = {cint(s.init)};
          } else {
            ints[s.decl_id] =
                std::vector<long>((size_t)sized_len(s.decl_type), 0);
          }
          return;
        }
        if (s.has_init) {
          const Range v = expr(s.init);
          const Range d = declare(s.decl_id, v.len);
          for (int k = 0; k < v.len; ++k)
            emit(Program::MOV, d.reg + k, v.reg + k);
        } else {
          declare(s.decl_id, (int)sized_len(s.decl_type));
        }
        return;
      }
      case mir::Stmt::Assignment: {
        if (ints.count(s.lhs) && s.lhs_idx.empty()) {
          ints[s.lhs] = {cint(s.rhs)};
          return;
        }
        auto it = reals.find(s.lhs);
        if (it == reals.end()) {
          // Assigning to a name the region did not declare: it lives
          // outside, so bind it (its current value is a live-in -- the
          // untaken branch has to leave it alone) and assign into those
          // registers.
          Range ext;
          if (bind_extern && bind_extern(s.lhs, &ext)) {
            reals[s.lhs] = ext;
            it = reals.find(s.lhs);
          }
        }
        if (it == reals.end()) bail("assignment to undeclared " + s.lhs);
        const Range dst = it->second;
        const Range v = expr(s.rhs);
        if (s.lhs_idx.empty()) {
          if (v.len != dst.len) bail("assignment width mismatch for " + s.lhs);
          for (int k = 0; k < v.len; ++k)
            emit(Program::MOV, dst.reg + k, v.reg + k);
          return;
        }
        if (s.lhs_idx.size() != 1 || s.lhs_idx[0].name != "IndexSingle")
          bail("assignment index form for " + s.lhs);
        const long ix = cint(s.lhs_idx[0].args[0]);
        if (ix < 1 || ix > dst.len) bail("assignment index range for " + s.lhs);
        if (v.len != 1) bail("element assignment from a container");
        emit(Program::MOV, dst.reg + (int)ix - 1, v.reg);
        return;
      }
      case mir::Stmt::Return:
        // A return under a runtime branch is a control-flow join this flat
        // program has no way to express; the interpreter still handles it.
        if (branch_depth) bail("return inside a data-dependent branch");
        throw Returned{s.has_init ? expr(s.rhs) : Range{0, 0}};
      case mir::Stmt::For: {
        const long lo = cint(s.lower), hi = cint(s.upper);
        for (long v = lo; v <= hi; ++v) {
          ints[s.loopvar] = {v};
          for (const auto& k : s.body) stmt(k);
        }
        ints.erase(s.loopvar);
        return;
      }
      case mir::Stmt::IfElse: {
        long c;
        if (try_cint(s.cond, &c)) {
          if (c != 0 && !s.body.empty()) stmt(s.body[0]);
          if (c == 0 && s.body.size() > 1) stmt(s.body[1]);
          return;
        }
        const Range cv = expr(s.cond);
        if (cv.len != 1) bail("branch on a container");
        ++branch_depth;
        const int jz = emit(Program::JZ, 0, cv.reg);
        if (!s.body.empty()) stmt(s.body[0]);
        if (s.body.size() > 1) {
          const int jmp = emit(Program::JMP, 0);
          p.code[(size_t)jz].dst = (int)p.code.size();
          stmt(s.body[1]);
          p.code[(size_t)jmp].dst = (int)p.code.size();
        } else {
          p.code[(size_t)jz].dst = (int)p.code.size();
        }
        --branch_depth;
        return;
      }
      case mir::Stmt::Block:
      case mir::Stmt::SList:
        for (const auto& k : s.body) stmt(k);
        return;
      case mir::Stmt::TargetPE: {
        // The region's own running total. The caller decides what it is
        // (the ODE side never sees one; lowering makes it a target term),
        // so all this does is accumulate.
        if (target_reg < 0) bail("target += is not available in this region");
        const Range v = expr(s.target);
        if (v.len != 1) bail("target += of a container");
        emit(Program::ADD, target_reg, target_reg, v.reg);
        return;
      }
      case mir::Stmt::NRFunApp:
      case mir::Stmt::Skip:
        return;
      default:
        bail("statement");
    }
  }

  Range inline_call(const mir::FunDef& f, const std::vector<Range>& args,
                    const std::vector<std::vector<long>>& iargs) {
    if (++inline_depth > 32) bail("function inlining too deep");
    // Callee scope: save the caller's bindings, install the parameters, and
    // restore afterwards. Registers are never reused, so nothing aliases.
    auto saved_reals = reals;
    auto saved_ints = ints;
    reals.clear();
    ints.clear();
    size_t ai = 0, ii = 0;
    for (size_t k = 0; k < f.arg_names.size(); ++k) {
      const bool is_int = f.arg_types[k].find("UInt") != std::string::npos;
      if (is_int && ii < iargs.size())
        ints[f.arg_names[k]] = iargs[ii++];
      else if (ai < args.size())
        reals[f.arg_names[k]] = args[ai++];
    }
    Range out{0, 0};
    try {
      for (const auto& s : f.body) stmt(s);
      bail("function " + f.name + " returned no value");
    } catch (Returned& r) {
      out = r.r;
    }
    reals = std::move(saved_reals);
    ints = std::move(saved_ints);
    --inline_depth;
    return out;
  }
};

}  // namespace stanli

#endif
