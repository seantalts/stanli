// The MIR interpreter is the semantic fallback for a compiled register
// program.  A fast path may refuse an expression, but when it accepts one it
// must preserve the same Stan value, control flow, and errors.  These cases
// construct the smallest MIR that distinguishes four such rules and check
// each path independently against the stated language result before comparing
// the paths with each other.
#include <stanli/mir.hpp>
#include <stanli/mir_interp.hpp>
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

Stmt return_value(Expr value) {
  Stmt s;
  s.kind = Stmt::Return;
  s.has_init = true;
  s.rhs = std::move(value);
  return s;
}

FunDef rhs_function(std::string name, std::vector<Stmt> body) {
  FunDef f;
  f.name = std::move(name);
  f.arg_names = {"t", "y", "theta", "x_r", "x_i"};
  f.arg_types = {"UReal", "UVector", "UVector", "(UArray UReal)",
                 "(UArray UInt)"};
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
};

struct ProgramObservation {
  Observation compile;
  Observation outcome;
};

uint64_t raw_bits(double value) {
  uint64_t bits;
  static_assert(sizeof(bits) == sizeof(value), "double is not 64 bits");
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

Observation observed_values(std::vector<double> values) {
  Observation out;
  out.bits.reserve(values.size());
  for (double value : values) out.bits.push_back(raw_bits(value));
  out.value = std::move(values);
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
  return out;
}

Observation observe_interpreter(
    const FunDef& entry, const std::map<std::string, const FunDef*>& functions,
    double t) {
  const std::vector<double> y{0.25};
  const std::vector<double> theta{0.75};
  const std::vector<double> x_r{1.25};
  const std::vector<int> x_i{2};
  try {
    MirInterp<double> interp(functions, "semantic conformance");
    std::vector<double> value = interp.call(entry, {{t}, y, theta, x_r}, {x_i});
    return observed_values(std::move(value));
  } catch (const std::domain_error& e) {
    return {Stage::Execute, OutcomeKind::DomainError, {}, {}, e.what()};
  } catch (const std::exception& e) {
    return {Stage::Execute, OutcomeKind::OtherError, {}, {}, e.what()};
  }
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
    return {error, error};
  } catch (const std::exception& e) {
    Observation error{
        Stage::Compile, OutcomeKind::OtherError, {}, {}, e.what()};
    return {error, error};
  }

  if (!p.ok) {
    Observation refusal{
        Stage::Compile, OutcomeKind::Refused, {}, {}, std::move(p.why)};
    return {std::move(refusal), observe_interpreter(entry, functions, t)};
  }

  Observation accepted{Stage::Compile, OutcomeKind::Accepted, {}, {}, {}};
  try {
    std::vector<double> value;
    run_rhs<double>(p, t, y.data(), theta.data(), x_r.data(), value);
    return {std::move(accepted), observed_values(std::move(value))};
  } catch (const std::domain_error& e) {
    return {std::move(accepted),
            {Stage::Execute, OutcomeKind::DomainError, {}, {}, e.what()}};
  } catch (const std::exception& e) {
    return {std::move(accepted),
            {Stage::Execute, OutcomeKind::OtherError, {}, {}, e.what()}};
  }
}

bool same_observation(const Observation& a, const Observation& b) {
  if (a.stage != b.stage || a.kind != b.kind) return false;
  if (a.kind != OutcomeKind::Value) return a.detail == b.detail;
  return a.bits == b.bits;
}

bool satisfies_semantics(const Observation& got, const Observation& want) {
  if (got.stage != want.stage || got.kind != want.kind ||
      got.value.size() != want.value.size())
    return false;
  // The language oracle may specify only an error category. Route parity
  // below remains stricter and requires the two paths' full messages to
  // agree byte-for-byte.
  if (got.kind != OutcomeKind::Value)
    return want.detail.empty() || got.detail == want.detail;
  for (size_t i = 0; i < want.value.size(); ++i) {
    if (std::isnan(want.value[i])) {
      if (!std::isnan(got.value[i])) return false;
    } else if (got.bits[i] != want.bits[i]) {
      return false;
    }
  }
  return true;
}

void expect_observation(const std::string& case_name, const char* path,
                        const Observation& got, const Observation& want) {
  if (satisfies_semantics(got, want)) return;
  ++failures;
  std::printf("FAIL %-24s %-11s got %s; Stan semantics require %s\n",
              case_name.c_str(), path, describe(got).c_str(),
              describe(want).c_str());
}

void run_observation_case(const std::string& name, const char* semantics,
                          std::vector<FunDef> functions, double t,
                          Observation want) {
  std::map<std::string, const FunDef*> table;
  for (const FunDef& f : functions) table[f.name] = &f;
  const FunDef& entry = functions.front();
  const ProgramObservation program = observe_program(entry, table, t);
  const Observation interpreter = observe_interpreter(entry, table, t);

  std::printf("CASE %s: %s\n", name.c_str(), semantics);
  if (program.compile.kind == OutcomeKind::Refused)
    std::printf("NOTE %-24s Program %s; exercised MirInterp fallback\n",
                name.c_str(), describe(program.compile).c_str());
  expect_observation(name, "Program", program.outcome, want);
  expect_observation(name, "MirInterp", interpreter, want);
  if (!same_observation(program.outcome, interpreter)) {
    ++failures;
    std::printf("FAIL %-24s path parity Program=%s; MirInterp=%s\n",
                name.c_str(), describe(program.outcome).c_str(),
                describe(interpreter).c_str());
  }
}

void run_case(const std::string& name, const char* semantics,
              std::vector<FunDef> functions, double t,
              std::vector<double> expected) {
  run_observation_case(name, semantics, std::move(functions), t,
                       observed_values(std::move(expected)));
}

void run_domain_error_case(const std::string& name, const char* semantics,
                           std::vector<FunDef> functions, double t) {
  run_observation_case(name, semantics, std::move(functions), t,
                       {Stage::Execute, OutcomeKind::DomainError, {}, {}, {}});
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

}  // namespace

int main() {
  test_short_circuit_or();
  test_short_circuit_and();
  test_short_circuit_or_requires_rhs();
  test_short_circuit_and_requires_rhs();
  test_uninitialized_real();
  test_matrix_row_indexing();
  test_mixed_integer_udf_arguments();
  if (failures == 0)
    std::printf("test_mir_program_conformance: all cases passed\n");
  else
    std::printf("test_mir_program_conformance: %d failures\n", failures);
  return failures == 0 ? 0 : 1;
}
