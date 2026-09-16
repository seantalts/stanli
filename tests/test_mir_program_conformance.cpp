// The MIR interpreter is the semantic fallback for a compiled register
// program.  A fast path may refuse an expression, but when it accepts one it
// must preserve the same Stan value, control flow, and errors.  These cases
// construct small MIR cases for those rules and check each path independently
// against the stated language result before comparing the paths with each
// other.
#include "stdout_capture.hpp"

#include <stanli/mir.hpp>
#include <stanli/mir_interp.hpp>
#include <stanli/mir_prog.hpp>
#include <stanli/ode_prog.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using stanli::compile_rhs;
using stanli::MirInterp;
using stanli::RhsProgram;
using stanli::run_rhs;
using stanli::mir::Expr;
using stanli::mir::FunDef;
using stanli::mir::SizedType;
using stanli::mir::Stmt;
using stanli::mir::UnsizedLeaf;
using stanli_test::StdoutCapture;

int failures = 0;

Expr lit_int(long value) {
  Expr e;
  e.kind = Expr::LitInt;
  e.lit_i = value;
  e.type_ = "UInt";
  e.data_only = true;
  return e;
}

Expr lit_real(double value) {
  Expr e;
  e.kind = Expr::LitReal;
  e.lit = value;
  e.type_ = "UReal";
  e.data_only = true;
  return e;
}

Expr lit_string(std::string value) {
  Expr e;
  e.kind = Expr::LitStr;
  e.lit_s = std::move(value);
  e.type_ = "UReal";
  e.data_only = true;
  return e;
}

Expr var(std::string name, std::string type) {
  Expr e;
  e.kind = Expr::Var;
  e.name = std::move(name);
  e.type_ = std::move(type);
  return e;
}

Expr fun(std::string name, std::vector<Expr> args, std::string type,
         Expr::Lib lib = Expr::Lib::StanLib) {
  Expr e;
  e.kind = Expr::FunApp;
  e.name = std::move(name);
  e.args = std::move(args);
  e.type_ = std::move(type);
  e.fn_lib = lib;
  return e;
}

Expr make_array(std::vector<Expr> values, std::string type = "UVector") {
  return fun("FnMakeArray", std::move(values), std::move(type),
             Expr::Lib::Internal);
}

Expr index_single(Expr base, long index, std::string type) {
  Expr ix;
  ix.name = "IndexSingle";
  ix.args.push_back(lit_int(index));

  Expr e;
  e.kind = Expr::Indexed;
  e.args.push_back(std::move(base));
  e.args.push_back(std::move(ix));
  e.type_ = std::move(type);
  return e;
}

Stmt declaration(std::string name, std::string base,
                 std::vector<Expr> dims = {}) {
  Stmt s;
  s.kind = Stmt::Decl;
  s.decl_id = std::move(name);
  s.decl_type = SizedType{std::move(base), std::move(dims), "", ""};
  return s;
}

Stmt assignment(std::string name, Expr rhs) {
  Stmt s;
  s.kind = Stmt::Assignment;
  s.lhs = std::move(name);
  s.rhs = std::move(rhs);
  return s;
}

Stmt full_span_assignment(std::string name, Expr rhs) {
  Stmt s = assignment(std::move(name), std::move(rhs));
  Expr all;
  all.kind = Expr::FunApp;
  all.name = "IndexAll";
  s.lhs_idx.push_back(std::move(all));
  return s;
}

Stmt return_value(Expr value) {
  Stmt s;
  s.kind = Stmt::Return;
  s.has_init = true;
  s.rhs = std::move(value);
  return s;
}

Stmt nr_fun_app(std::string name, std::vector<Expr> args) {
  Stmt s;
  s.kind = Stmt::NRFunApp;
  s.fn_name = std::move(name);
  s.fn_args = std::move(args);
  return s;
}

FunDef rhs_function(std::string name, std::vector<Stmt> body) {
  FunDef f;
  f.name = std::move(name);
  f.arg_names = {"t", "y", "theta", "x_r", "x_i"};
  f.arg_types = {"UReal", "UVector", "UVector", "(UArray UReal)",
                 "(UArray UInt)"};
  f.arg_views = {{0, UnsizedLeaf::Real},
                 {0, UnsizedLeaf::Vector},
                 {0, UnsizedLeaf::Vector},
                 {1, UnsizedLeaf::Real},
                 {1, UnsizedLeaf::Int}};
  f.body = std::move(body);
  return f;
}

enum class Stage { Compile, Execute };

enum class OutcomeKind { Value, Accepted, Refused, DomainError, OtherError };

struct Observation {
  Stage stage = Stage::Execute;
  OutcomeKind kind = OutcomeKind::Value;
  std::vector<double> value;
  std::vector<uint64_t> bits;
  std::string detail;
  std::string stdout_text;
};

struct ProgramObservation {
  Observation compile;
  Observation outcome;
  bool refused_program_is_empty = false;
};

uint64_t raw_bits(double value) {
  uint64_t bits;
  static_assert(sizeof(bits) == sizeof(value), "double is not 64 bits");
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

Observation observed_values(std::vector<double> values,
                            std::string stdout_text = {}) {
  Observation out;
  out.bits.reserve(values.size());
  for (double value : values) out.bits.push_back(raw_bits(value));
  out.value = std::move(values);
  out.stdout_text = std::move(stdout_text);
  return out;
}

const char* kind_name(OutcomeKind kind) {
  switch (kind) {
    case OutcomeKind::Value:
      return "value";
    case OutcomeKind::Accepted:
      return "accepted";
    case OutcomeKind::Refused:
      return "refused";
    case OutcomeKind::DomainError:
      return "domain_error";
    case OutcomeKind::OtherError:
      return "other exception";
  }
  return "unknown";
}

std::string escaped(std::string text) {
  std::string out;
  for (char c : text) {
    if (c == '\n')
      out += "\\n";
    else if (c == '\r')
      out += "\\r";
    else if (c == '\t')
      out += "\\t";
    else
      out += c;
  }
  return out;
}

std::string describe(const Observation& o) {
  std::string out = o.stage == Stage::Compile ? "compile " : "execute ";
  out += kind_name(o.kind);
  if (o.kind == OutcomeKind::Value) {
    out += " [";
    for (size_t i = 0; i < o.value.size(); ++i) {
      if (i) out += ", ";
      char rendered[64];
      if (std::isnan(o.value[i]))
        std::snprintf(rendered, sizeof(rendered), "NaN(0x%016llx)",
                      static_cast<unsigned long long>(o.bits[i]));
      else
        std::snprintf(rendered, sizeof(rendered), "%.17g(0x%016llx)",
                      o.value[i], static_cast<unsigned long long>(o.bits[i]));
      out += rendered;
    }
    out += "]";
  } else if (!o.detail.empty()) {
    out += ": " + o.detail;
  }
  if (!o.stdout_text.empty())
    out += "; stdout=\"" + escaped(o.stdout_text) + "\"";
  return out;
}

template <typename F>
Observation observe_execution(F&& run) {
  StdoutCapture captured;
  try {
    Observation out = observed_values(run());
    out.stdout_text = captured.finish();
    return out;
  } catch (const std::domain_error& e) {
    return {Stage::Execute, OutcomeKind::DomainError, {}, {},
            e.what(),       captured.finish()};
  } catch (const std::exception& e) {
    return {Stage::Execute, OutcomeKind::OtherError, {}, {},
            e.what(),       captured.finish()};
  }
}

Observation observe_interpreter(
    const FunDef& entry, const std::map<std::string, const FunDef*>& functions,
    double t) {
  const std::vector<double> y{0.25};
  const std::vector<double> theta{0.75};
  const std::vector<double> x_r{1.25};
  const std::vector<int> x_i{2};
  return observe_execution([&] {
    MirInterp<double> interp(functions, "semantic conformance");
    return interp.call(entry, {{t}, y, theta, x_r}, {x_i});
  });
}

ProgramObservation observe_program(
    const FunDef& entry, const std::map<std::string, const FunDef*>& functions,
    double t) {
  const std::vector<double> y{0.25};
  const std::vector<double> theta{0.75};
  const std::vector<double> x_r{1.25};
  const std::vector<int> x_i{2};
  RhsProgram p;
  try {
    p = compile_rhs(entry, functions, 1, (int)theta.size(), (int)x_r.size(),
                    x_i);
  } catch (const std::domain_error& e) {
    Observation error{
        Stage::Compile, OutcomeKind::DomainError, {}, {}, e.what()};
    return {error, error, false};
  } catch (const std::exception& e) {
    Observation error{
        Stage::Compile, OutcomeKind::OtherError, {}, {}, e.what()};
    return {error, error, false};
  }

  if (!p.ok) {
    Observation refusal{
        Stage::Compile, OutcomeKind::Refused, {}, {}, std::move(p.why)};
    const bool empty = p.code.empty() && p.out_regs.empty();
    return {std::move(refusal), observe_interpreter(entry, functions, t),
            empty};
  }

  Observation accepted{Stage::Compile, OutcomeKind::Accepted, {}, {}, {}};
  return {std::move(accepted), observe_execution([&] {
            std::vector<double> value;
            std::vector<double> rhs_registers;
            run_rhs<double>(p, t, y.data(), theta.data(), x_r.data(), value,
                            rhs_registers);
            return value;
          }),
          false};
}

bool same_observation(const Observation& a, const Observation& b) {
  if (a.stage != b.stage || a.kind != b.kind || a.stdout_text != b.stdout_text)
    return false;
  if (a.kind != OutcomeKind::Value) return a.detail == b.detail;
  return a.bits == b.bits;
}

int64_t ulp_distance(double a, double b) {
  int64_t ia, ib;
  std::memcpy(&ia, &a, sizeof ia);
  std::memcpy(&ib, &b, sizeof ib);
  if ((ia < 0) != (ib < 0)) return std::numeric_limits<int64_t>::max();
  const int64_t d = ia - ib;
  return d < 0 ? -d : d;
}

bool satisfies_semantics(const Observation& got, const Observation& want,
                         int64_t ulps) {
  if (got.stage != want.stage || got.kind != want.kind ||
      got.value.size() != want.value.size() ||
      got.stdout_text != want.stdout_text)
    return false;
  // The language oracle may specify only an error category. Route parity
  // below remains stricter and requires the two paths' full messages to
  // agree byte-for-byte.
  if (got.kind != OutcomeKind::Value)
    return want.detail.empty() || got.detail == want.detail;
  for (size_t i = 0; i < want.value.size(); ++i) {
    if (std::isnan(want.value[i])) {
      if (!std::isnan(got.value[i])) return false;
    } else if (got.bits[i] != want.bits[i] &&
               ulp_distance(got.value[i], want.value[i]) > ulps) {
      return false;
    }
  }
  return true;
}

void expect_observation(const std::string& case_name, const char* path,
                        const Observation& got, const Observation& want,
                        int64_t ulps) {
  if (satisfies_semantics(got, want, ulps)) return;
  ++failures;
  std::printf("FAIL %-24s %-11s got %s; Stan semantics require %s\n",
              case_name.c_str(), path, describe(got).c_str(),
              describe(want).c_str());
}

void run_observation_case(const std::string& name, const char* semantics,
                          std::vector<FunDef> functions, double t,
                          Observation want,
                          const char* required_refusal = nullptr,
                          bool required_acceptance = false, int64_t ulps = 0) {
  std::map<std::string, const FunDef*> table;
  for (const FunDef& f : functions) table[f.name] = &f;
  const FunDef& entry = functions.front();
  const ProgramObservation program = observe_program(entry, table, t);
  const Observation interpreter = observe_interpreter(entry, table, t);

  std::printf("CASE %s: %s\n", name.c_str(), semantics);
  if (program.compile.kind == OutcomeKind::Refused)
    std::printf("NOTE %-24s Program %s; exercised MirInterp fallback\n",
                name.c_str(), describe(program.compile).c_str());
  if (required_acceptance && program.compile.kind != OutcomeKind::Accepted) {
    ++failures;
    std::printf("FAIL %-24s expected the Program to compile this; got %s\n",
                name.c_str(), describe(program.compile).c_str());
  }
  if (required_refusal &&
      (program.compile.kind != OutcomeKind::Refused ||
       program.compile.detail.find(required_refusal) == std::string::npos ||
       !program.refused_program_is_empty)) {
    ++failures;
    std::printf(
        "FAIL %-24s expected a cleared Program refusal naming %s; got %s\n",
        name.c_str(), required_refusal, describe(program.compile).c_str());
  }
  expect_observation(name, "Program", program.outcome, want, ulps);
  expect_observation(name, "MirInterp", interpreter, want, ulps);
  if (!same_observation(program.outcome, interpreter)) {
    ++failures;
    std::printf("FAIL %-24s path parity Program=%s; MirInterp=%s\n",
                name.c_str(), describe(program.outcome).c_str(),
                describe(interpreter).c_str());
  }
}

void run_case(const std::string& name, const char* semantics,
              std::vector<FunDef> functions, double t,
              std::vector<double> expected, std::string expected_stdout = {},
              const char* required_refusal = nullptr) {
  run_observation_case(
      name, semantics, std::move(functions), t,
      observed_values(std::move(expected), std::move(expected_stdout)),
      required_refusal);
}

// The same, for what the register program is required to compile rather than
// allowed to decline: a refusal here is a failure, not a fallback.
void run_program_case(const std::string& name, const char* semantics,
                      std::vector<FunDef> functions, double t,
                      std::vector<double> expected, int64_t ulps = 0) {
  run_observation_case(name, semantics, std::move(functions), t,
                       observed_values(std::move(expected)), nullptr, true,
                       ulps);
}

void run_domain_error_case(const std::string& name, const char* semantics,
                           std::vector<FunDef> functions, double t,
                           std::string detail = {},
                           std::string expected_stdout = {},
                           const char* required_refusal = nullptr) {
  run_observation_case(name, semantics, std::move(functions), t,
                       {Stage::Execute,
                        OutcomeKind::DomainError,
                        {},
                        {},
                        std::move(detail),
                        std::move(expected_stdout)},
                       required_refusal);
}

void run_other_error_case(const std::string& name, const char* semantics,
                          std::vector<FunDef> functions, double t,
                          std::string detail = {},
                          const char* required_refusal = nullptr) {
  run_observation_case(
      name, semantics, std::move(functions), t,
      {Stage::Execute, OutcomeKind::OtherError, {}, {}, std::move(detail), {}},
      required_refusal);
}

void test_short_circuit_or() {
  // Stan's || evaluates its right operand only when the left operand is
  // false.  The invalid density must therefore never be called here.
  Expr bad_density =
      fun("normal_lpdf", {lit_real(0), lit_real(0), lit_real(-1)}, "UReal");
  Expr invalid_rhs =
      fun("Greater__", {std::move(bad_density), lit_real(0)}, "UInt");
  Expr disjunction;
  disjunction.kind = Expr::EOr;
  disjunction.type_ = "UInt";
  disjunction.args = {lit_int(1), std::move(invalid_rhs)};

  FunDef entry =
      rhs_function("short_circuit_rhs",
                   {return_value(make_array({std::move(disjunction)}))});
  run_case("short-circuit invalid RHS",
           "true || rhs is 1 and never evaluates rhs", {std::move(entry)}, 1.0,
           {1.0});
}

void test_short_circuit_and() {
  // Stan's && evaluates its right operand only when the left operand is
  // true.  The invalid density must therefore never be called here.
  Expr bad_density =
      fun("normal_lpdf", {lit_real(0), lit_real(0), lit_real(-1)}, "UReal");
  Expr invalid_rhs =
      fun("Greater__", {std::move(bad_density), lit_real(0)}, "UInt");
  Expr conjunction;
  conjunction.kind = Expr::EAnd;
  conjunction.type_ = "UInt";
  conjunction.args = {lit_int(0), std::move(invalid_rhs)};

  FunDef entry =
      rhs_function("short_circuit_and_rhs",
                   {return_value(make_array({std::move(conjunction)}))});
  run_case("short-circuit AND invalid RHS",
           "false && rhs is 0 and never evaluates rhs", {std::move(entry)}, 1.0,
           {0.0});
}

void test_short_circuit_or_requires_rhs() {
  // Complement the skipping polarity: when the runtime left operand is
  // false, || must evaluate the invalid right operand and surface its error.
  Expr bad_density =
      fun("normal_lpdf", {lit_real(0), lit_real(0), lit_real(-1)}, "UReal");
  Expr invalid_rhs =
      fun("Greater__", {std::move(bad_density), lit_real(0)}, "UInt");
  Expr runtime_left =
      fun("Greater__", {var("t", "UReal"), lit_real(0)}, "UInt");
  Expr disjunction;
  disjunction.kind = Expr::EOr;
  disjunction.type_ = "UInt";
  disjunction.args = {std::move(runtime_left), std::move(invalid_rhs)};

  FunDef entry =
      rhs_function("short_circuit_or_required_rhs",
                   {return_value(make_array({std::move(disjunction)}))});
  run_domain_error_case(
      "short-circuit OR takes RHS",
      "false || rhs evaluates rhs and propagates its domain error",
      {std::move(entry)}, -1.0);
}

void test_short_circuit_and_requires_rhs() {
  // Complement the skipping polarity: when the runtime left operand is
  // true, && must evaluate the invalid right operand and surface its error.
  Expr bad_density =
      fun("normal_lpdf", {lit_real(0), lit_real(0), lit_real(-1)}, "UReal");
  Expr invalid_rhs =
      fun("Greater__", {std::move(bad_density), lit_real(0)}, "UInt");
  Expr runtime_left =
      fun("Greater__", {var("t", "UReal"), lit_real(0)}, "UInt");
  Expr conjunction;
  conjunction.kind = Expr::EAnd;
  conjunction.type_ = "UInt";
  conjunction.args = {std::move(runtime_left), std::move(invalid_rhs)};

  FunDef entry =
      rhs_function("short_circuit_and_required_rhs",
                   {return_value(make_array({std::move(conjunction)}))});
  run_domain_error_case(
      "short-circuit AND takes RHS",
      "true && rhs evaluates rhs and propagates its domain error",
      {std::move(entry)}, 1.0);
}

void test_uninitialized_real() {
  // A bare local real has Stan's uninitialized NaN value, never numeric zero.
  FunDef entry = rhs_function("uninitialized_rhs",
                              {declaration("u", "SReal"),
                               return_value(make_array({var("u", "UReal")}))});
  run_case("uninitialized real", "a bare real observes as NaN",
           {std::move(entry)}, 1.0, {std::numeric_limits<double>::quiet_NaN()});
}

void test_nullary_constants() {
  // All public nullary constants, plus stanc's internal negative-infinity
  // spelling, share one resolver across ProgramCompiler and MirInterp.
  const auto check = [&](const std::string& name, Expr value, double expected) {
    FunDef entry = rhs_function("nullary_" + name + "_rhs",
                                {return_value(make_array({std::move(value)}))});
    run_case("nullary " + name, "constant compiles identically",
             {std::move(entry)}, 1.0, {expected});
  };
  check("e", fun("e", {}, "UReal"), stan::math::e());
  check("pi", fun("pi", {}, "UReal"), stan::math::pi());
  check("log2", fun("log2", {}, "UReal"), stan::math::log2());
  check("log10", fun("log10", {}, "UReal"), stan::math::log10());
  check("sqrt2", fun("sqrt2", {}, "UReal"), stan::math::sqrt2());
  check("machine_precision", fun("machine_precision", {}, "UReal"),
        std::numeric_limits<double>::epsilon());
  check("negative_infinity", fun("negative_infinity", {}, "UReal"),
        -std::numeric_limits<double>::infinity());
  check("positive_infinity", fun("positive_infinity", {}, "UReal"),
        std::numeric_limits<double>::infinity());
  check("not_a_number", fun("not_a_number", {}, "UReal"),
        std::numeric_limits<double>::quiet_NaN());
  check("FnNegInf", fun("FnNegInf", {}, "UReal", Expr::Lib::Internal),
        -std::numeric_limits<double>::infinity());
}

void test_discrete_densities() {
  // The integer-outcome densities. Their outcome rides in idata rather than
  // on an argument register, so both routes reach the graph kernel and the
  // oracle is Stan Math's own propto-OFF value.
  const Expr rate = var("t", "UReal");
  const int64_t library_ulps = 2;
  const auto check = [&](const std::string& name, std::vector<Expr> args,
                         double expected) {
    FunDef entry = rhs_function(
        name + "_rhs",
        {return_value(make_array({fun(name, std::move(args), "UReal")}))});
    run_program_case(name, "an integer outcome reaches the same kernel",
                     {std::move(entry)}, 0.25, {expected}, library_ulps);
  };
  check("poisson_lpmf", {lit_int(3), rate},
        stan::math::poisson_lpmf<false>(3, 0.25));
  check("poisson_log_lpmf", {lit_int(3), rate},
        stan::math::poisson_log_lpmf<false>(3, 0.25));
  check("bernoulli_lpmf", {lit_int(1), rate},
        stan::math::bernoulli_lpmf<false>(1, 0.25));
  check("bernoulli_logit_lpmf", {lit_int(1), rate},
        stan::math::bernoulli_logit_lpmf<false>(1, 0.25));
  check("binomial_lpmf", {lit_int(1), lit_int(2), rate},
        stan::math::binomial_lpmf<false>(1, 2, 0.25));
  check("binomial_logit_lpmf", {lit_int(1), lit_int(2), rate},
        stan::math::binomial_logit_lpmf<false>(1, 2, 0.25));
  check("neg_binomial_2_lpmf", {lit_int(3), rate, lit_real(1.25)},
        stan::math::neg_binomial_2_lpmf<false>(3, 0.25, 1.25));
  check("neg_binomial_2_log_lpmf", {lit_int(3), rate, lit_real(1.25)},
        stan::math::neg_binomial_2_log_lpmf<false>(3, 0.25, 1.25));
}

void test_full_span_ode_vector() {
  // ODE RHS compilation is one production caller of ProgramCompiler. The
  // destination is vector[1], and y supplies a vector view of exactly that
  // width, so the indexed assignment must stay on the compiled path.
  FunDef entry =
      rhs_function("full_span_vector_rhs",
                   {declaration("copy", "SVector", {lit_int(1)}),
                    full_span_assignment("copy", var("y", "UVector")),
                    return_value(var("copy", "UVector"))});
  run_case("full-span ODE vector", "vector[1] copy[:] = y returns y",
           {std::move(entry)}, 1.0, {0.25});
}

void test_full_span_program_views() {
  // Statement islands also use ProgramCompiler and can bind complete array
  // container views from surrounding graph slots. Build those bindings
  // directly here so vector, row-vector, scalar-array, and array-container
  // geometry all execute through the register machine.
  const auto exercise = [&](const std::string& name, stanli::Range dst,
                            stanli::Range rhs, std::vector<double> values) {
    stanli::Program program;
    std::map<std::string, const FunDef*> functions;
    stanli::ProgramCompiler compiler{program, functions};
    dst.reg = compiler.alloc(dst.len);
    rhs.reg = compiler.alloc(rhs.len);
    std::vector<double> initial(static_cast<size_t>(dst.len), -1.0);
    compiler.emit_const(dst.reg, initial.data(), dst.len);
    compiler.emit_const(rhs.reg, values.data(), rhs.len);
    compiler.reals["dst"] = dst;
    compiler.reals["rhs"] = rhs;
    const std::string rhs_type =
        rhs.kind == stanli::ViewKind::Vector      ? "UVector"
        : rhs.kind == stanli::ViewKind::RowVector ? "URowVector"
                                                  : "UArray";
    compiler.stmt(full_span_assignment("dst", var("rhs", rhs_type)));
    compiler.finish();
    const stanli::Range out = compiler.reals.at("dst");
    std::vector<double> registers(static_cast<size_t>(program.n_regs));
    stanli::run_program(program, registers);
    std::vector<double> got(registers.begin() + out.reg,
                            registers.begin() + out.reg + out.len);
    if (got != values) {
      ++failures;
      std::printf("FAIL %-24s Program full-span value mismatch\n",
                  name.c_str());
    }
  };

  stanli::Range vector;
  vector.len = 3;
  vector.kind = stanli::ViewKind::Vector;
  exercise("full-span vector view", vector, vector, {1, 2, 3});

  stanli::Range row;
  row.len = 3;
  row.kind = stanli::ViewKind::RowVector;
  exercise("full-span row view", row, row, {4, 5, 6});

  stanli::Range scalar_array;
  scalar_array.len = 4;
  scalar_array.kind = stanli::ViewKind::Array;
  scalar_array.dims = {2, 2};
  exercise("full-span scalar array", scalar_array, scalar_array,
           {11, 12, 21, 22});

  stanli::Range container_array;
  container_array.len = 8;
  container_array.kind = stanli::ViewKind::Array;
  container_array.dims = {2, 2, 2};
  exercise("full-span container array", container_array, container_array,
           {1, 2, 3, 4, 5, 6, 7, 8});

  stanli::Program mismatch_program;
  std::map<std::string, const FunDef*> functions;
  stanli::ProgramCompiler mismatch{mismatch_program, functions};
  stanli::Range dst{mismatch.alloc(4), 4};
  dst.kind = stanli::ViewKind::Array;
  dst.dims = {2, 2};
  stanli::Range rhs{mismatch.alloc(4), 4};
  rhs.kind = stanli::ViewKind::Array;
  rhs.dims = {4};
  mismatch.reals["dst"] = dst;
  mismatch.reals["rhs"] = rhs;
  bool refused = false;
  try {
    mismatch.stmt(full_span_assignment("dst", var("rhs", "UArray")));
  } catch (const stanli::Bail& error) {
    refused = error.why.find("logical view mismatch") != std::string::npos;
  }
  if (!refused) {
    ++failures;
    std::printf("FAIL full-span Program accepted mismatched array geometry\n");
  }
}

void test_program_extrema() {
  // ProgramCompiler must use the same language-level overload classifier as
  // graph lowering and MirInterp. Exercise every one-argument surface plus
  // the scalar integer pair, and verify that min/max share one instruction
  // whose immediate selects the operation.
  stanli::Program program;
  std::map<std::string, const FunDef*> functions;
  stanli::ProgramCompiler compiler{program, functions};
  const double values[] = {3.0, -2.0, 7.0, 1.0};

  auto bind = [&](const std::string& name, const std::string& type,
                  UnsizedLeaf leaf, uint8_t depth, stanli::ViewKind kind) {
    stanli::Range range{compiler.alloc(4), 4};
    range.kind = kind;
    if (kind == stanli::ViewKind::Matrix) {
      range.rows = 2;
      range.cols = 2;
    }
    if (kind == stanli::ViewKind::Array) {
      range.dims = {4};
      range.leaf = stanli::ViewKind::Flat;
    }
    compiler.emit_const(range.reg, values, 4);
    compiler.reals[name] = range;
    Expr input = var(name, type);
    input.unsized = {depth, leaf};
    input.data_only = leaf != UnsizedLeaf::Int;
    return input;
  };
  auto reduce = [&](const char* name, Expr input, const char* result_type,
                    UnsizedLeaf result_leaf) {
    Expr call = fun(name, {std::move(input)}, result_type);
    call.unsized = {0, result_leaf};
    return compiler.expr(call).reg;
  };

  std::vector<int> outputs;
  outputs.push_back(reduce(
      "min",
      bind("v", "UVector", UnsizedLeaf::Vector, 0, stanli::ViewKind::Vector),
      "UReal", UnsizedLeaf::Real));
  outputs.push_back(reduce("max",
                           bind("rv", "URowVector", UnsizedLeaf::RowVector, 0,
                                stanli::ViewKind::RowVector),
                           "UReal", UnsizedLeaf::Real));
  outputs.push_back(reduce(
      "min",
      bind("m", "UMatrix", UnsizedLeaf::Matrix, 0, stanli::ViewKind::Matrix),
      "UReal", UnsizedLeaf::Real));
  outputs.push_back(reduce(
      "max",
      bind("ar", "UArray", UnsizedLeaf::Real, 1, stanli::ViewKind::Array),
      "UReal", UnsizedLeaf::Real));
  outputs.push_back(reduce(
      "min", bind("ai", "UArray", UnsizedLeaf::Int, 1, stanli::ViewKind::Array),
      "UInt", UnsizedLeaf::Int));

  Expr lhs = lit_int(9);
  Expr rhs = lit_int(-4);
  lhs.unsized = rhs.unsized = {0, UnsizedLeaf::Int};
  Expr pair = fun("max", {std::move(lhs), std::move(rhs)}, "UInt");
  pair.unsized = {0, UnsizedLeaf::Int};
  outputs.push_back(compiler.expr(pair).reg);
  compiler.finish();

  int extrema_count = 0;
  int min_count = 0;
  int max_count = 0;
  int packet_count = 0;
  int scalar_count = 0;
  for (const auto& instruction : program.code) {
    if (instruction.code != stanli::Program::EXTREMA_RANGE) continue;
    ++extrema_count;
    instruction.b == 0 ? ++min_count : ++max_count;
    if (instruction.c & stanli::kProgramExtremaScalar)
      ++scalar_count;
    else
      ++packet_count;
  }
  if (extrema_count != 6 || min_count != 3 || max_count != 3 ||
      packet_count != 4 || scalar_count != 2) {
    ++failures;
    std::printf(
        "FAIL Program extrema opcode/grouping: total=%d min=%d max=%d "
        "packet=%d scalar=%d\n",
        extrema_count, min_count, max_count, packet_count, scalar_count);
  }

  std::vector<double> registers(static_cast<size_t>(program.n_regs));
  stanli::run_program(program, registers);
  const double expected[] = {-2.0, 7.0, -2.0, 7.0, -2.0, 9.0};
  for (size_t i = 0; i < outputs.size(); ++i) {
    if (registers[static_cast<size_t>(outputs[i])] == expected[i]) continue;
    ++failures;
    std::printf("FAIL Program extrema output %zu: got %.17g, want %.17g\n", i,
                registers[static_cast<size_t>(outputs[i])], expected[i]);
  }
}

void test_matrix_row_indexing() {
  // A[1] is the complete first row.  For [[1,2],[3,4]], sum(A[1]) is 3;
  // indexing a flattened register is not the same operation.
  Expr row1 = fun("FnMakeRowVec", {lit_real(1), lit_real(2)}, "URowVector",
                  Expr::Lib::Internal);
  Expr row2 = fun("FnMakeRowVec", {lit_real(3), lit_real(4)}, "URowVector",
                  Expr::Lib::Internal);
  Expr matrix = make_array({std::move(row1), std::move(row2)}, "UMatrix");
  Expr first_row = index_single(var("A", "UMatrix"), 1, "URowVector");
  Expr row_sum = fun("sum", {std::move(first_row)}, "UReal");

  FunDef entry = rhs_function(
      "matrix_row_rhs", {declaration("A", "SMatrix", {lit_int(2), lit_int(2)}),
                         assignment("A", std::move(matrix)),
                         return_value(make_array({std::move(row_sum)}))});
  run_case("matrix row indexing", "sum([[1,2],[3,4]][1]) is 3",
           {std::move(entry)}, 1.0, {3.0});
}

void test_mixed_integer_udf_arguments() {
  // Arguments bind by source position.  With t > 0, score(t > 0, 3) is
  // 10*1 + 3 = 13; partitioning runtime and constant ints must not swap them.
  FunDef score;
  score.name = "score";
  score.arg_names = {"a", "b"};
  score.arg_types = {"UInt", "UInt"};
  score.arg_views = {{0, UnsizedLeaf::Int}, {0, UnsizedLeaf::Int}};
  score.body = {
      return_value(fun("Plus__",
                       {fun("Times__", {var("a", "UInt"), lit_int(10)}, "UInt"),
                        var("b", "UInt")},
                       "UInt"))};

  Expr runtime_int = fun("Greater__", {var("t", "UReal"), lit_real(0)}, "UInt");
  Expr call = fun("score", {std::move(runtime_int), lit_int(3)}, "UInt",
                  Expr::Lib::UserDefined);
  FunDef entry = rhs_function("mixed_integer_args_rhs",
                              {return_value(make_array({std::move(call)}))});
  run_case("mixed integer UDF args",
           "score(t > 0, 3) binds arguments positionally",
           {std::move(entry), std::move(score)}, 1.0, {13.0});
}

void test_nested_print_effect() {
  // print is an ordered language effect even when it lives in an inlined
  // user-defined function. Its arguments are evaluated and rendered once.
  FunDef echo;
  echo.name = "echo";
  echo.arg_names = {"x"};
  echo.arg_types = {"UReal"};
  echo.arg_views = {{0, UnsizedLeaf::Real}};
  echo.body = {
      nr_fun_app("FnPrint", {lit_string("nested print x="), var("x", "UReal")}),
      return_value(var("x", "UReal"))};

  Expr call = fun("echo", {var("t", "UReal")}, "UReal", Expr::Lib::UserDefined);
  FunDef entry = rhs_function("nested_print_rhs",
                              {return_value(make_array({std::move(call)}))});
  run_case("nested print effect",
           "print in a UDF emits exactly once before returning",
           {std::move(entry), std::move(echo)}, 1.0, {1.0},
           "nested print x=1\n");
}

void test_print_then_reject_effects() {
  // Earlier effects remain observable when reject terminates evaluation;
  // reject is specifically a domain_error carrying every rendered chunk.
  FunDef entry = rhs_function(
      "print_then_reject_rhs",
      {nr_fun_app("FnPrint",
                  {lit_string("before reject t="), var("t", "UReal")}),
       nr_fun_app("FnReject", {lit_string("bad rhs t="), var("t", "UReal")}),
       nr_fun_app("FnPrint", {lit_string("unreachable after reject")}),
       return_value(make_array({lit_real(0)}))});
  run_domain_error_case(
      "print then reject effects",
      "print occurs first; reject throws domain_error with its full message",
      {std::move(entry)}, 1.0, "bad rhs t=1", "before reject t=1\n", nullptr);
}

FunDef runtime_effect_rhs() {
  Stmt effects;
  effects.kind = Stmt::Block;
  effects.body = {nr_fun_app("FnPrint", {lit_string("negative branch t="),
                                         var("t", "UReal")}),
                  nr_fun_app("FnReject", {lit_string("negative t rejected: "),
                                          var("t", "UReal")})};

  Stmt guarded;
  guarded.kind = Stmt::IfElse;
  guarded.cond = fun("Less__", {var("t", "UReal"), lit_real(0)}, "UInt");
  guarded.body.push_back(std::move(effects));

  return rhs_function(
      "runtime_effect_rhs",
      {std::move(guarded), return_value(make_array({var("t", "UReal")}))});
}

void test_runtime_guarded_effects() {
  // The same runtime branch supplies both truth-table halves. An untaken
  // effect is absent; a taken effect executes in source order.
  const FunDef entry = runtime_effect_rhs();
  run_case("untaken runtime effects",
           "a false branch emits nothing and evaluation continues", {entry},
           1.0, {1.0});
  run_domain_error_case(
      "taken runtime effects",
      "a true branch prints once, then reject terminates evaluation", {entry},
      -1.0, "negative t rejected: -1", "negative branch t=-1\n");
}

void test_unknown_nrfunapp_fails_loud() {
  // Erasing an unrecognized non-returning call invents semantics. Until its
  // effect is implemented, both routes must fail loudly rather than return.
  FunDef entry = rhs_function("unknown_effect_rhs",
                              {nr_fun_app("FnUnmodeledEffect", {lit_real(1)}),
                               return_value(make_array({lit_real(0)}))});
  run_other_error_case("unknown NRFunApp",
                       "an unmodeled statement is never a silent no-op",
                       {std::move(entry)}, 1.0,
                       "stanli semantic conformance: unsupported statement "
                       "function FnUnmodeledEffect",
                       "FnUnmodeledEffect");
}

// Speculative unknown values are common in runtime branches. Probe lazily,
// preserving real comparisons and geometry even when values are unavailable.
void test_constant_probes() {
  stanli::Program p;
  std::map<std::string, const FunDef*> functions;
  stanli::ProgramCompiler pc{p, functions};
  const auto require = [](bool ok, const char* why) {
    if (!ok) {
      ++failures;
      std::printf("FAIL constant probe: %s\n", why);
    }
  };
  long integer = 123;
  double real = 123;
  const Expr unknown = var("unknown", "UInt");
  require(!pc.try_cint(unknown, &integer) && integer == 123, "unknown integer");
  require(!pc.try_creal(var("unknown", "UReal"), &real) && real == 123,
          "unknown real");
  for (auto kind : {Expr::EAnd, Expr::EOr}) {
    Expr e;
    e.kind = kind;
    e.type_ = "UInt";
    const long lhs = kind == Expr::EAnd ? 0 : 1;
    e.args = {lit_int(lhs), unknown};
    require(pc.try_cint(e, &integer) && integer == lhs, "lazy dead operand");
    e.args[0] = lit_int(1 - lhs);
    require(!pc.try_cint(e, &integer), "required unknown operand");
  }
  Expr select;
  select.kind = Expr::TernaryIf;
  select.type_ = "UReal";
  select.args = {lit_int(1), lit_real(-0.0), var("unknown", "UReal")};
  require(pc.try_creal(select, &real) && real == 0 && std::signbit(real),
          "lazy real ternary and signed zero");
  select.args[0] = lit_int(0);
  require(!pc.try_creal(select, &real), "selected unknown arm");
  require(pc.try_cint(fun("Greater__", {lit_real(2.5), lit_int(2)}, "UInt"),
                      &integer) &&
              integer == 1,
          "fractional comparison must not truncate");
  require(
      pc.try_creal(lit_real(std::numeric_limits<double>::quiet_NaN()), &real) &&
          std::isnan(real),
      "known NaN is a value");
  stanli::Range shape;
  shape.kind = stanli::ViewKind::Vector;
  shape.len = 7;
  shape.reg = pc.alloc(shape.len);
  pc.reals["runtime_values"] = shape;
  require(pc.try_cint(fun("rows", {var("runtime_values", "UVector")}, "UInt"),
                      &integer) &&
              integer == 7,
          "shape without runtime values");
}

void test_builtin_activity() {
  for (int mode = 0; mode < 4; ++mode) {
    stanli::Program p;
    std::map<std::string, const FunDef*> functions;
    stanli::ProgramCompiler pc{p, functions};
    pc.reals["x"] = stanli::Range{pc.alloc(1), 1};
    Expr argument = var("x", mode == 2 ? "UInt" : "UReal");
    argument.data_only = mode == 1;
    argument.promoted = mode == 3;
    stanli::Range out;
    try {
      out = pc.expr(fun("lgamma", {argument}, "UReal"));
    } catch (const stanli::Bail& error) {
      ++failures;
      std::printf("FAIL builtin argument mode %d: %s\n", mode,
                  error.why.c_str());
      continue;
    }
    if (p.calls.size() != 1 ||
        p.calls[0].input_adjoint_mask != (mode == 0 || mode == 3 ? 1 : 0)) {
      ++failures;
      std::printf("FAIL builtin argument activity mode %d\n", mode);
    }
    for (double value : {2., 4.}) {
      stan::math::nested_rev_autodiff nested;
      stan::math::var input = value;
      std::vector<stan::math::var> reg(p.n_regs);
      reg[0] = input;
      stanli::run_program(p, reg);
      reg[out.reg].grad();
      const double want_grad =
          mode == 0 || mode == 3 ? stan::math::digamma(value) : 0.;
      if (reg[out.reg].val() != stan::math::lgamma(value) ||
          input.adj() != want_grad) {
        ++failures;
        std::printf("FAIL builtin argument derivative mode %d\n", mode);
      }
    }
  }
}

void test_constant_fill_bits() {
  double nan1, nan2;
  const uint64_t bits1 = UINT64_C(0x7ff8000000000021);
  const uint64_t bits2 = UINT64_C(0x7ff8000000000022);
  std::memcpy(&nan1, &bits1, sizeof nan1);
  std::memcpy(&nan2, &bits2, sizeof nan2);
  const std::vector<std::vector<double>> cases{
      {0., 0., 0.}, {-0., -0., -0.}, {1.5, 1.5, 1.5}, {nan1, nan1, nan1},
      {nan1, nan2}, {0., -0.},       {1., 2., 1.}};
  for (const auto& values : cases) {
    stanli::Program program;
    std::map<std::string, const FunDef*> functions;
    stanli::ProgramCompiler pc{program, functions};
    (void)pc.konst(0.);  // a later -0 must not reuse this different bit pattern
    const int dst = pc.alloc((int)values.size());
    pc.emit_const(dst, values.data(), (int)values.size());
    bool uniform = true;
    for (size_t k = 1; k < values.size(); ++k)
      uniform = uniform && std::memcmp(values.data(), values.data() + k,
                                       sizeof(double)) == 0;
    if ((program.code.back().code == stanli::Program::FILL) != uniform)
      ++failures;
    std::vector<double> reg(program.n_regs);
    stanli::run_program(program, reg);
    stan::math::nested_rev_autodiff nested;
    std::vector<stan::math::var> vr(program.n_regs);
    stanli::run_program(program, vr);
    for (size_t k = 0; k < values.size(); ++k) {
      const double replayed = vr[dst + k].val();
      if (std::memcmp(&reg[dst + k], &values[k], sizeof(double)) != 0 ||
          std::memcmp(&replayed, &values[k], sizeof(double)) != 0) {
        ++failures;
        std::printf("FAIL constant fill changed a literal bit pattern\n");
      }
    }
    if (uniform && vr[dst].vi_ != vr[dst + 1].vi_) ++failures;
  }
}

void test_dynamic_vector_program() {
  using Var = stan::math::var;
  using stanli::Program;
  using stanli::ProgramCompiler;
  using stanli::Range;
  using stanli::ViewKind;
  const auto require = [](bool ok, const char* label) {
    if (!ok) {
      ++failures;
      std::printf("FAIL dynamic vector %s\n", label);
    }
  };
  for (bool row : {false, true}) {
    for (bool compact : {false, true}) {
      Program p;
      std::map<std::string, const FunDef*> functions;
      ProgramCompiler pc{p, functions};
      Range base{pc.alloc(8), 8};
      base.kind = row ? ViewKind::RowVector : ViewKind::Vector;
      const std::string type = row ? "URowVector" : "UVector";
      pc.reals["values"] = base;
      std::vector<std::pair<int, int>> seeds{{base.reg, base.len}};
      for (const char* name : {"index", "lo", "hi"}) {
        Range r{pc.alloc(1), 1};
        pc.reals[name] = r;
        seeds.emplace_back(r.reg, 1);
      }
      // RHS aliases the destination vector. Indexing and the RHS must read
      // their old values before a write changes the selected element.
      Stmt set =
          assignment("values", index_single(var("values", type), 1, "UReal"));
      Expr index;
      index.name = "IndexSingle";
      index.args = {var("index", "UInt")};
      set.lhs_idx = {index};
      pc.stmt(set);
      Expr slice;
      slice.kind = Expr::Indexed;
      slice.type_ = type;
      Expr span;
      span.name = "IndexBetween";
      span.args = {var("lo", "UInt"), var("hi", "UInt")};
      slice.args = {var("values", type), span};
      const Range result = pc.expr(fun("log_sum_exp", {slice}, "UReal"));
      p.out_regs = {result.reg};
      require(p.code.size() == 2 && p.code[0].code == Program::DYN_SET &&
                  p.code[1].code == Program::DYN_LSE_RANGE,
              "compiler selects dynamic operations");
      if (compact) stanli::compact_program(p, seeds);
      // Reuse the same program across changing writes, extents and an empty
      // slice. There is deliberately an unread NaN after the final prefix.
      for (const auto& controls : std::vector<std::vector<int>>{
               {1, 1, 8}, {3, 2, 6}, {8, 8, 8}, {4, 4, 3}, {3, 1, 2}}) {
        stan::math::nested_rev_autodiff nested;
        std::vector<double> input{.2, -.5, 1.7, -3., .8, 2.1, -1.2, .4};
        if (controls[2] == 2)
          input[7] = std::numeric_limits<double>::quiet_NaN();
        std::vector<double> reg(p.n_regs);
        std::vector<Var> vr(p.n_regs), vin;
        Eigen::Matrix<Var, Eigen::Dynamic, 1> reference(8);
        std::vector<Var> ref_in;
        for (int k = 0; k < 8; ++k) {
          reg[seeds[0].first + k] = input[k];
          vin.emplace_back(input[k]);
          ref_in.emplace_back(input[k]);
          vr[seeds[0].first + k] = vin.back();
          reference[k] = ref_in.back();
        }
        for (int k = 0; k < 3; ++k) {
          reg[seeds[k + 1].first] = controls[k];
          vr[seeds[k + 1].first] = controls[k];
        }
        stanli::run_program(p, reg);
        stanli::run_program(p, vr);
        reference[controls[0] - 1] = reference[0];
        const int width = std::max(0, controls[2] - controls[1] + 1);
        const Eigen::Matrix<Var, Eigen::Dynamic, 1> selected =
            reference.segment(controls[1] - 1, width);
        Var expected = stan::math::log_sum_exp(selected);
        const auto equal = [](double a, double b) {
          return a == b || (std::isnan(a) && std::isnan(b)) ||
                 std::abs(a - b) <= 1e-14 * std::max(1., std::abs(b));
        };
        require(equal(reg[p.out_regs[0]], expected.val()), "double oracle");
        require(equal(vr[p.out_regs[0]].val(), expected.val()), "var oracle");
        // Both tapes are independent, so one sum seeds both outputs without
        // requiring a reset of nested autodiff's other variables.
        (vr[p.out_regs[0]] + expected).grad();
        for (int k = 0; k < 8; ++k)
          require(equal(vin[k].adj(), ref_in[k].adj()),
                  "aliased write gradient");
      }
      for (const auto& controls : std::vector<std::vector<double>>{
               {0, 1, 2},
               {9, 1, 2},
               {1.5, 1, 2},
               {1, 0, 2},
               {1, 1, 9},
               {1, 1.5, 2},
               {1, 1, std::numeric_limits<double>::infinity()}}) {
        std::vector<double> reg(p.n_regs, .2);
        for (int k = 0; k < 3; ++k) reg[seeds[k + 1].first] = controls[k];
        bool threw = false;
        try {
          stanli::run_program(p, reg);
        } catch (const std::out_of_range&) {
          threw = true;
        }
        require(threw, "runtime bounds rejection");
      }
      // A standalone dynamically sized slice still refuses; this change is
      // limited to scalar reductions with a fixed-capacity source.
      bool refused = false;
      try {
        (void)pc.expr(slice);
      } catch (const stanli::Bail&) {
        refused = true;
      }
      require(refused, "standalone dynamic slice still refuses");
    }
  }
}

void test_runtime_remainder_and_identity_index() {
  stanli::Program p;
  std::map<std::string, const FunDef*> functions;
  stanli::ProgramCompiler pc{p, functions};
  for (const char* name : {"a", "b"})
    pc.reals[name] = stanli::Range{pc.alloc(1), 1};
  const auto out =
      pc.expr(fun("Modulo__", {var("a", "UInt"), var("b", "UInt")}, "UInt"));
  if (p.code.size() != 1 || p.code[0].code != stanli::Program::IMOD) ++failures;
  for (auto pair : std::vector<std::pair<int, int>>{
           {7, 3},
           {-7, 3},
           {7, -3},
           {-7, -3},
           {0, 2},
           {std::numeric_limits<int>::min(), 2},
           {std::numeric_limits<int>::max(), -7}}) {
    std::vector<double> reg(p.n_regs);
    reg[0] = pair.first;
    reg[1] = pair.second;
    stanli::run_program(p, reg);
    if (reg[out.reg] != stan::math::modulus(pair.first, pair.second))
      ++failures;
  }
  bool threw = false;
  try {
    std::vector<double> reg(p.n_regs, 0.);
    stanli::run_program(p, reg);
  } catch (const std::domain_error&) {
    threw = true;
  }
  if (!threw) ++failures;
  stanli::Range values{pc.alloc(3), 3};
  values.kind = stanli::ViewKind::Vector;
  pc.reals["values"] = values;
  Expr identity;
  identity.kind = Expr::Indexed;
  identity.type_ = "UVector";
  identity.args = {var("values", "UVector")};
  const auto same = pc.expr(identity);
  if (same.reg != values.reg || same.len != 3 || same.kind != values.kind)
    ++failures;
}

}  // namespace

int main() {
  test_builtin_activity();
  test_constant_fill_bits();
  test_dynamic_vector_program();
  test_runtime_remainder_and_identity_index();
  test_constant_probes();
  test_short_circuit_or();
  test_short_circuit_and();
  test_short_circuit_or_requires_rhs();
  test_short_circuit_and_requires_rhs();
  test_uninitialized_real();
  test_nullary_constants();
  test_discrete_densities();
  test_full_span_ode_vector();
  test_full_span_program_views();
  test_program_extrema();
  test_matrix_row_indexing();
  test_mixed_integer_udf_arguments();
  test_nested_print_effect();
  test_print_then_reject_effects();
  test_runtime_guarded_effects();
  test_unknown_nrfunapp_fails_loud();
  if (failures == 0)
    std::printf("test_mir_program_conformance: all cases passed\n");
  else
    std::printf("test_mir_program_conformance: %d failures\n", failures);
  return failures == 0 ? 0 : 1;
}
