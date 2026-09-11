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
#include <stanli/island.hpp>
#include <stanli/ode_prog.hpp>
#include <stanli/sexp.hpp>

#include <stan/math.hpp>

#include <algorithm>
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

void check_generated_local(const std::string& name, const stanli::RhsProgram& p,
                           const stanli::IslandProg& generated, double t,
                           const std::vector<double>& y,
                           const std::vector<double>& theta,
                           const std::vector<double>& x_r) {
  using namespace stanli;
  const size_t width = y.size() + theta.size();
  std::vector<double> want_values(p.out_regs.size());
  std::vector<double> want_jacobian(p.out_regs.size() * width);
  {
    stan::math::nested_rev_autodiff nested;
    std::vector<stan::math::var> y_vars(y.begin(), y.end());
    std::vector<stan::math::var> theta_vars(theta.begin(), theta.end());
    std::vector<stan::math::var> outputs(p.out_regs.size());
    std::vector<stan::math::var> registers;
    run_rhs_into<stan::math::var>(p, t, y_vars.data(), theta_vars.data(),
                                  theta_vars.size(), x_r.data(), outputs.data(),
                                  registers);
    for (size_t output = 0; output < outputs.size(); ++output)
      want_values[output] = outputs[output].val();
    for (size_t output = 0; output < outputs.size(); ++output) {
      stan::math::grad(outputs[output].vi_);
      for (size_t input = 0; input < y.size(); ++input)
        want_jacobian[output * width + input] = y_vars[input].adj();
      for (size_t input = 0; input < theta.size(); ++input)
        want_jacobian[output * width + y.size() + input] =
            theta_vars[input].adj();
      if (output + 1 < outputs.size()) nested.set_zero_all_adjoints();
    }
  }

  std::vector<double> values((size_t)generated.n_regs);
  for (int i = 0; i < p.n_y; ++i) values[(size_t)(p.y0 + i)] = y[(size_t)i];
  for (int i = 0; i < p.n_th; ++i)
    values[(size_t)(p.th0 + i)] = theta[(size_t)i];
  values[(size_t)p.t_reg] = t;
  for (int i = 0; i < p.n_xr; ++i) values[(size_t)(p.xr0 + i)] = x_r[(size_t)i];
  run_program(generated, values);

  std::vector<double> got_values(p.out_regs.size());
  std::vector<double> got_jacobian(p.out_regs.size() * width);
  std::vector<double> adjoints((size_t)generated.adj.n_regs);
  for (size_t output = 0; output < p.out_regs.size(); ++output) {
    const int output_reg = generated.out_regs[output];
    got_values[output] = values[(size_t)output_reg];
    std::fill(adjoints.begin(), adjoints.end(), 0.0);
    adjoints[(size_t)generated.adj.adj_reg[(size_t)output_reg]] = 1.0;
    run_adjoint(generated, generated.adj, values.data(), adjoints.data());
    for (int input = 0; input < p.n_y; ++input) {
      const int reg = p.y0 + input;
      got_jacobian[output * width + (size_t)input] =
          adjoints[(size_t)generated.adj.adj_reg[(size_t)reg]];
    }
    for (int input = 0; input < p.n_th; ++input) {
      const int reg = p.th0 + input;
      got_jacobian[output * width + y.size() + (size_t)input] =
          adjoints[(size_t)generated.adj.adj_reg[(size_t)reg]];
    }
  }

  bool values_exact = got_values.size() == want_values.size();
  for (size_t i = 0; i < got_values.size() && values_exact; ++i)
    values_exact = bits(got_values[i]) == bits(want_values[i]);
  bool jacobian_exact = got_jacobian.size() == want_jacobian.size();
  for (size_t i = 0; i < got_jacobian.size() && jacobian_exact; ++i)
    jacobian_exact = bits(got_jacobian[i]) == bits(want_jacobian[i]);
  expect(name + ": generated local values are bitwise exact", values_exact);
  expect(name + ": generated local Jacobian is bitwise exact", jacobian_exact);
}

void check_exact_opcode_contract() {
  using namespace stanli;
  RhsProgram p;
  p.ok = true;
  p.t_reg = 0;
  p.y0 = 1;
  p.n_y = 2;
  p.th0 = 3;
  p.n_th = 3;
  p.xr0 = 6;
  p.n_xr = 2;
  p.n_regs = 8;
  auto alloc = [&](int len = 1) {
    const int reg = p.n_regs;
    p.n_regs += len;
    return reg;
  };
  auto emit = [&](Program::Code code, int a, int b = 0, int c = 0, int len = 0,
                  int out_len = 1) {
    const int dst = alloc(out_len);
    p.code.push_back(Program::Instr{code, dst, a, b, c, len});
    for (int i = 0; i < out_len; ++i) p.out_regs.push_back(dst + i);
  };

  p.pool = {0.375, -0.25, 1.5};
  emit(Program::CONST, 0);
  emit(Program::CONSTR, 1, 0, 0, 2, 2);
  emit(Program::MOV, p.y0);
  emit(Program::MOVR, p.y0, 0, 0, 2, 2);
  for (Program::Code code :
       {Program::ADD, Program::SUB, Program::MUL, Program::DIV, Program::POW})
    emit(code, p.y0, p.th0);
  for (Program::Code code :
       {Program::NEG, Program::EXP, Program::LOG, Program::SQRT,
        Program::SQUARE, Program::INV, Program::FABS, Program::INV_LOGIT,
        Program::LOG1M, Program::LOG1P_EXP, Program::TANH})
    emit(code, code == Program::LOG1M ? p.th0 : p.y0);
  for (Program::Code code : {Program::GT, Program::GE, Program::LT, Program::LE,
                             Program::EQ, Program::NE})
    emit(code, p.y0, p.th0);
  emit(Program::LOG_RANGE, p.y0, 0, 0, 2, 2);
  emit(Program::EXP_RANGE, p.y0, 0, 0, 2, 2);
  emit(Program::LSE2, p.y0, p.y0 + 1);
  emit(Program::LOG_MIX, p.th0, p.y0, p.y0 + 1);
  emit(Program::FMA, p.y0, p.th0, p.xr0);

  std::string refusal;
  const std::shared_ptr<const IslandProg> generated =
      make_rhs_adjoint_program(p, &refusal);
  expect("exact opcode contract is eligible", (bool)generated);
  expect("exact opcode contract has no refusal", refusal.empty());
  if (generated)
    check_generated_local("exact opcode contract", p, *generated, 0.25,
                          {0.7, 1.2}, {0.3, 0.8, 1.1}, {0.4, 0.6});
}

void check(const std::string& name, const stanli::mir::FunDef& f,
           const std::map<std::string, const stanli::mir::FunDef*>& funs,
           int n_y, int n_th, const std::vector<double>& x_r,
           const std::vector<int>& x_i, bool want_ok, bool want_generated) {
  using namespace stanli;
  RhsProgram p = compile_rhs(f, funs, n_y, n_th, (int)x_r.size(), x_i);
  if (p.ok != want_ok) {
    ++failures;
    std::printf("FAIL %s: compile ok=%d, wanted %d (%s)\n", name.c_str(),
                (int)p.ok, (int)want_ok, p.why.c_str());
    return;
  }
  const int canonical_n_regs = p.n_regs;
  const std::vector<Program::Instr> canonical_code = p.code;
  std::string refusal;
  const std::shared_ptr<const IslandProg> generated =
      make_rhs_adjoint_program(p, &refusal);
  bool canonical_unchanged =
      p.n_regs == canonical_n_regs && p.code.size() == canonical_code.size();
  for (size_t i = 0; i < p.code.size() && canonical_unchanged; ++i) {
    const Program::Instr& got = p.code[i];
    const Program::Instr& want = canonical_code[i];
    canonical_unchanged = got.code == want.code && got.dst == want.dst &&
                          got.a == want.a && got.b == want.b &&
                          got.c == want.c && got.len == want.len;
  }
  expect(name + ": derivative builder preserves canonical bytecode",
         canonical_unchanged);
  if (!want_ok) {
    if (p.why.empty()) {
      ++failures;
      std::printf("FAIL %s: refused without saying why\n", name.c_str());
    }
    expect(name + ": compiler refusal has no derivative payload", !generated);
    expect(name + ": compiler refusal is observable", !refusal.empty());
    return;  // the interpreter still serves it; the ODE kernel falls back
  }
  expect(name + ": generated derivative eligibility",
         (bool)generated == want_generated);
  expect(name + ": generated derivative disposition is observable",
         generated ? refusal.empty() : !refusal.empty());
  if (!generated && (name == "f_branch" || name == "f_udf"))
    expect(name + ": runtime control-flow refusal names JZ/JMP",
           refusal.find("JZ") != std::string::npos ||
               refusal.find("JMP") != std::string::npos);

  for (int trial = 0; trial < 12; ++trial) {
    const double t = probe(trial) * 2.0;  // straddles the t > 0.5 branch
    std::vector<double> y((size_t)n_y), th((size_t)n_th);
    for (int i = 0; i < n_y; ++i) y[(size_t)i] = probe(trial * 7 + i) + 0.4;
    for (int i = 0; i < n_th; ++i) th[(size_t)i] = probe(trial * 11 + i) + 0.2;

    std::vector<double> got;
    std::vector<double> registers;
    run_rhs<double>(p, t, y.data(), th.data(), x_r.data(), got, registers);
    std::vector<double> got_into(p.out_regs.size());
    run_rhs_into<double>(p, t, y.data(), th.data(), x_r.data(), got_into.data(),
                         registers);
    if (got_into != got) {
      ++failures;
      std::printf("FAIL %s: caller-owned output differs\n", name.c_str());
      return;
    }

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
    if (generated) check_generated_local(name, p, *generated, t, y, th, x_r);
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

template <bool YAutodiff, bool ThetaAutodiff, bool Staged, bool Into = false>
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
  std::vector<T> registers;
  if constexpr (Into) {
    out.resize(p.out_regs.size());
    stanli::run_rhs_into<T>(p, t, y.data(), theta.data(), theta.size(),
                            x_r.data(), out.data(), registers);
  } else if constexpr (Staged) {
    std::vector<T> staged_y(y.begin(), y.end());
    std::vector<T> staged_theta(theta.begin(), theta.end());
    const T staged_t(t);
    stanli::run_rhs<T>(p, staged_t, staged_y.data(), staged_theta.data(),
                       staged_theta.size(), x_r.data(), out, registers);
  } else {
    stanli::run_rhs<T>(p, t, y.data(), theta.data(), theta.size(), x_r.data(),
                       out, registers);
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
  const MixedRun into =
      mixed_run<YAutodiff, ThetaAutodiff, false, true>(p, 0.73, y, theta, x_r);
  const std::string prefix = std::string("mixed seed ") + label + ": ";
  expect(prefix + "output bits", direct.values == staged.values);
  expect(prefix + "y gradient bits", direct.y_grads == staged.y_grads);
  expect(prefix + "theta gradient bits",
         direct.theta_grads == staged.theta_grads);
  expect(prefix + "chain tape order", direct.chain_tape == staged.chain_tape);
  expect(prefix + "nochain tape order",
         direct.nochain_tape == staged.nochain_tape);
  expect(prefix + "caller-owned output bits", into.values == staged.values);
  expect(prefix + "caller-owned y gradient bits",
         into.y_grads == staged.y_grads);
  expect(prefix + "caller-owned theta gradient bits",
         into.theta_grads == staged.theta_grads);
  expect(prefix + "caller-owned chain tape order",
         into.chain_tape == staged.chain_tape);
  expect(prefix + "caller-owned nochain tape order",
         into.nochain_tape == staged.nochain_tape);
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
    bool want_generated;
  };
  const Case cases[] = {
      {"f_lin", 2, 4, true, true},
      {"f_branch", 2, 4, true, false},  // JZ/JMP fail closed
      {"f_udf", 2, 4, true, false},     // runtime ternary emits JZ/JMP
      {"f_early", 2, 4, false, false},  // return from a runtime branch
  };
  for (const Case& c : cases) {
    auto it = funs.find(c.name);
    if (it == funs.end()) {
      ++failures;
      std::printf("FAIL fixture has no function %s\n", c.name);
      continue;
    }
    check(c.name, *it->second, funs, c.n_y, c.n_th, x_r, x_i, c.want_ok,
          c.want_generated);
  }
  check_exact_opcode_contract();

  // stan-math instantiates a var state whenever either side is active. The
  // data-y/active-theta case is included too: run_rhs is a generic boundary,
  // and this is the combination most likely to expose a changed y promotion.
  {
    const auto it = funs.find("f_lin");
    const RhsProgram p = compile_rhs(*it->second, funs, 2, 4, 2, x_i);
    expect("mixed seed fixture compiles", p.ok);
    if (p.ok) {
      const bool compact =
          p.code.size() == 6 &&
          std::none_of(p.code.begin(), p.code.end(), [](const auto& i) {
            return i.code == Program::CONST || i.code == Program::MOV;
          });
      if (!compact) {
        std::printf("optimized f_lin has %zu instructions:", p.code.size());
        for (const auto& i : p.code)
          std::printf(" %s", program_code_spec(i.code).name);
        std::printf("\n");
      }
      expect("straight-line RHS drops initializer/copy bookkeeping", compact);
      check_mixed_seed<true, false>(p, "var/double");
      check_mixed_seed<false, true>(p, "double/var");
      check_mixed_seed<true, true>(p, "var/var");
      check_mixed_seed<false, false>(p, "double/double");

      // gen_adjoint can differentiate DOT, but its double packet reduction is
      // not the var replay's scalar grouping. The ODE-specific exact whitelist
      // must refuse it before generated-adjoint construction.
      RhsProgram with_dot = p;
      with_dot.code.push_back(Program::Instr{Program::DOT, with_dot.n_regs,
                                             with_dot.y0, with_dot.y0, 0, 2});
      ++with_dot.n_regs;
      std::string refusal;
      expect("direct RK refuses DOT value grouping",
             !make_rhs_adjoint_program(with_dot, &refusal));
      expect("direct RK DOT refusal is observable",
             refusal.find("DOT") != std::string::npos);

      // C99 fmax/fmin and Stan Math's var overload choose different operands
      // for signed-zero ties. Even an otherwise unused occurrence makes the
      // program ineligible until the double forward can mirror var exactly.
      for (const Program::Code opcode : {Program::FMAX, Program::FMIN}) {
        RhsProgram with_signed_zero_tie = p;
        with_signed_zero_tie.code.push_back(Program::Instr{
            opcode, with_signed_zero_tie.n_regs, with_signed_zero_tie.y0,
            with_signed_zero_tie.y0 + 1, 0, 0});
        ++with_signed_zero_tie.n_regs;
        refusal.clear();
        const char* name = program_code_spec(opcode).name;
        expect(
            std::string("direct RK refuses ") + name + " signed-zero grouping",
            !make_rhs_adjoint_program(with_signed_zero_tie, &refusal));
        expect(std::string("direct RK ") + name + " refusal is observable",
               refusal.find(name) != std::string::npos);
      }
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
