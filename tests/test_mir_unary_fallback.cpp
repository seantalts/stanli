// Keep graph, MIR double/var, and ODE-fallback unary semantics aligned with
// Stan Math. Raw finite bits pin the shared pullback expression's ordering;
// NaNs are compared by category because payload bits are not Stan semantics.
#include <stanli/graph.hpp>
#include <stanli/mir.hpp>
#include <stanli/mir_interp.hpp>
#include <stanli/ode.hpp>
#include <stanli/ode_prog.hpp>
#include <stanli/optable.hpp>

#include <stan/math.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using stanli::mir::UnsizedLeaf;

using stanli::compile_rhs;
using stanli::MirInterp;
using stanli::RhsProgram;
using stanli::mir::Expr;
using stanli::mir::FunDef;
using stanli::mir::Stmt;

int failures = 0;

Expr lit(double x) {
  Expr e;
  e.kind = Expr::LitReal;
  e.lit = x;
  e.type_ = "UReal";
  e.data_only = true;
  return e;
}

Expr integer(long x) {
  Expr e;
  e.kind = Expr::LitInt;
  e.lit_i = x;
  e.type_ = "UInt";
  e.data_only = true;
  return e;
}

Expr variable(std::string name, std::string type) {
  Expr e;
  e.kind = Expr::Var;
  e.name = std::move(name);
  e.type_ = std::move(type);
  return e;
}

Expr call(std::string name, std::vector<Expr> args, std::string type,
          Expr::Lib lib = Expr::Lib::StanLib) {
  Expr e;
  e.kind = Expr::FunApp;
  e.name = std::move(name);
  e.args = std::move(args);
  e.type_ = std::move(type);
  e.fn_lib = lib;
  return e;
}

Expr element(Expr base, long index) {
  Expr ix;
  ix.name = "IndexSingle";
  ix.args.push_back(integer(index));
  Expr e;
  e.kind = Expr::Indexed;
  e.args = {std::move(base), std::move(ix)};
  e.type_ = "UReal";
  return e;
}

Stmt returning(Expr value) {
  Stmt s;
  s.kind = Stmt::Return;
  s.has_init = true;
  s.rhs = std::move(value);
  return s;
}

FunDef rhs(std::string name, Expr value) {
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
  f.body = {returning(
      call("FnMakeArray", {std::move(value)}, "UVector", Expr::Lib::Internal))};
  return f;
}

FunDef unary_rhs(const std::string& name, std::optional<double> seed) {
  Expr value = call(name, {element(variable("theta", "UVector"), 1)}, "UReal");
  if (seed) value = call("Times__", {std::move(value), lit(*seed)}, "UReal");
  return rhs("unary_rhs", std::move(value));
}

enum class Category { Value, DomainError, OtherError };
struct Observation {
  Category category;
  double value;
  double gradient;
  std::string detail;
};

template <class F>
Observation capture(F&& fn) {
  try {
    const auto result = fn();
    return {Category::Value, result.first, result.second, {}};
  } catch (const std::domain_error& e) {
    return {Category::DomainError, 0, 0, e.what()};
  } catch (const std::exception& e) {
    return {Category::OtherError, 0, 0, e.what()};
  }
}

uint64_t bits(double x) {
  uint64_t out;
  std::memcpy(&out, &x, sizeof(out));
  return out;
}

bool same_number(double got, double want) {
  return std::isnan(want) ? std::isnan(got) : bits(got) == bits(want);
}

void expect_same(const std::string& name, const Observation& got,
                 const Observation& want, bool gradient = true) {
  const bool ok = got.category == want.category &&
                  (want.category != Category::Value ||
                   (same_number(got.value, want.value) &&
                    (!gradient || same_number(got.gradient, want.gradient))));
  if (ok) return;
  ++failures;
  std::printf("FAIL %-35s got {%d,%.17g,%.17g,%s}; want {%d,%.17g,%.17g}\n",
              name.c_str(), (int)got.category, got.value, got.gradient,
              got.detail.c_str(), (int)want.category, want.value,
              want.gradient);
}

Observation mir_double(const std::string& name, double x,
                       std::optional<double> seed = {}) {
  return capture([&] {
    FunDef f = unary_rhs(name, seed);
    const std::map<std::string, const FunDef*> defs{{f.name, &f}};
    MirInterp<double> interp(defs, "unary double");
    const auto out = interp.call(f, {{0}, {1}, {x}, {1.25}}, {{2}});
    return std::pair{out.at(0), 0.0};
  });
}

Observation mir_var(const std::string& name, double x,
                    std::optional<double> seed = {}) {
  return capture([&] {
    stan::math::nested_rev_autodiff nested;
    stan::math::var theta = x;
    FunDef f = unary_rhs(name, seed);
    const std::map<std::string, const FunDef*> defs{{f.name, &f}};
    MirInterp<stan::math::var> interp(defs, "unary var");
    auto out = interp.call(f,
                           {{stan::math::var(0)},
                            {stan::math::var(1)},
                            {theta},
                            {stan::math::var(1.25)}},
                           {{2}});
    stan::math::grad(out.at(0).vi_);
    return std::pair{out[0].val(), theta.adj()};
  });
}

Observation graph(uint16_t opcode, double x, std::optional<double> seed = {}) {
  return capture([&] {
    stanli::Graph g;
    const int input = g.add_slot(1, true);
    const int unary = g.add_slot(1, false);
    int multiplier = -1;
    g.add_op(opcode, {input}, unary);
    if (seed) {
      multiplier = g.add_slot(1, false);
      const int output = g.add_slot(1, false);
      g.add_op(stanli::OP_MUL, {unary, multiplier}, output);
      g.result_slot = output;
    } else {
      g.result_slot = unary;
    }
    stanli::Executor executor(std::move(g));
    *executor.param_ptr(input) = x;
    if (seed) *executor.value_ptr(multiplier) = *seed;
    double gradient = 0;
    return std::pair{executor.gradient(&gradient), gradient};
  });
}

Observation program(const std::string& name, double x, double seed) {
  return capture([&] {
    FunDef f = unary_rhs(name, seed);
    const std::map<std::string, const FunDef*> defs{{f.name, &f}};
    const std::vector<int> x_i{2};
    const RhsProgram p = compile_rhs(f, defs, 1, 1, 1, x_i);
    if (!p.ok) throw std::runtime_error("Program refused: " + p.why);
    stan::math::nested_rev_autodiff nested;
    stan::math::var t = 0, y = 1, theta = x;
    const double x_r = 1.25;
    std::vector<stan::math::var> out;
    stanli::run_rhs(p, t, &y, &theta, &x_r, out);
    stan::math::grad(out.at(0).vi_);
    return std::pair{out[0].val(), theta.adj()};
  });
}

template <class Oracle>
Observation stan(double x, std::optional<double> seed, Oracle&& oracle) {
  return capture([&] {
    stan::math::nested_rev_autodiff nested;
    stan::math::var input = x;
    stan::math::var output = oracle(input);
    if (seed) output *= *seed;
    stan::math::grad(output.vi_);
    return std::pair{output.val(), input.adj()};
  });
}

double probe(const std::string& name) {
  if (name == "acosh") return 1.7;
  if (name == "log1m_exp") return -0.37;
  if (name == "inv") return 0.3;
  return 0.37;
}

template <class Oracle>
void check_row(const std::string& name, uint16_t opcode, double x,
               std::optional<double> seed, Oracle&& oracle) {
  const Observation want = stan(x, seed, std::forward<Oracle>(oracle));
  Observation want_double = want;
  want_double.gradient = 0;
  expect_same(name + " MIR<double>", mir_double(name, x, seed), want_double);
  expect_same(name + " MIR<var>", mir_var(name, x, seed), want);
  expect_same(name + " graph", graph(opcode, x, seed), want);
}

void test_manifest_and_rows() {
  std::string manifest;
#define APPEND_MANIFEST(code, fn, value, delta, topology) \
  manifest += std::string(#code) + ":" #fn ";";
  STANLI_SCALAR_UNARY_LIST(APPEND_MANIFEST)
#undef APPEND_MANIFEST
  const std::string expected =
      "OP_LGAMMA:lgamma;OP_DIGAMMA:digamma;OP_LOG1P:log1p;OP_EXPM1:expm1;"
      "OP_PHI:Phi;OP_INV_PHI:inv_Phi;OP_ERF:erf;OP_ERFC:erfc;OP_INV:inv;"
      "OP_INV_SQRT:inv_sqrt;OP_INV_SQUARE:inv_square;OP_LOG1M_EXP:log1m_exp;"
      "OP_LOG1P_EXP:log1p_exp;OP_LOG_INV_LOGIT:log_inv_logit;"
      "OP_LOG1M_INV_LOGIT:log1m_inv_logit;OP_INV_CLOGLOG:inv_cloglog;"
      "OP_SIN:sin;OP_COS:cos;OP_TAN:tan;OP_ASIN:asin;OP_ACOS:acos;"
      "OP_ATAN:atan;OP_SINH:sinh;OP_COSH:cosh;OP_ASINH:asinh;OP_ACOSH:acosh;"
      "OP_ATANH:atanh;OP_CBRT:cbrt;OP_EXP2:exp2;OP_LOG2:log2;OP_LOG10:log10;"
      "OP_ABS:abs;OP_FLOOR:floor;OP_CEIL:ceil;OP_ROUND:round;OP_TRUNC:trunc;"
      "OP_STEP:step;";
  if (manifest != expected) {
    ++failures;
    std::printf("FAIL shared unary manifest\n");
  }

#define CHECK_ROW(code, fn, value, delta, topology)                      \
  check_row(#fn, stanli::code, probe(#fn), {},                           \
            [](const stan::math::var& x) { return stan::math::fn(x); }); \
  if (stanli::topology != stanli::UnaryTopology::Disconnected)           \
    check_row(#fn, stanli::code, probe(#fn), 0.7,                        \
              [](const stan::math::var& x) { return stan::math::fn(x); });
  STANLI_SCALAR_UNARY_LIST(CHECK_ROW)
#undef CHECK_ROW
}

template <class Oracle>
void check_program(const std::string& label, const std::string& name,
                   uint16_t opcode, double x, Oracle&& oracle,
                   bool all_routes = false) {
  const Observation want = stan(x, 0.7, std::forward<Oracle>(oracle));
  expect_same(label + " Program", program(name, x, 0.7), want);
  if (!all_routes) return;
  expect_same(label + " graph", graph(opcode, x, 0.7), want);
  expect_same(label + " MIR", mir_var(name, x, 0.7), want);
}

void expect_disconnected(const std::string& name, const Observation& got) {
  if (got.category == Category::Value && std::isfinite(got.gradient) &&
      got.gradient == 0.0)
    return;
  ++failures;
  std::printf("FAIL %-35s gradient %.17g category %d\n", name.c_str(),
              got.gradient, (int)got.category);
}

void test_program_and_topology() {
  check_program("inv", "inv", stanli::OP_INV, 0.3,
                [](const stan::math::var& x) { return stan::math::inv(x); });
  check_program("abs positive", "abs", stanli::OP_ABS, 0.3,
                [](const stan::math::var& x) { return stan::math::fabs(x); });
  check_program(
      "abs negative", "abs", stanli::OP_ABS, -0.3,
      [](const stan::math::var& x) { return stan::math::fabs(x); }, true);
  check_program(
      "abs NaN", "abs", stanli::OP_ABS,
      std::numeric_limits<double>::quiet_NaN(),
      [](const stan::math::var& x) { return stan::math::fabs(x); }, true);

  const double inf = std::numeric_limits<double>::infinity();
  for (double zero : {0.0, -0.0}) {
    expect_disconnected("abs Program", program("abs", zero, inf));
    expect_disconnected("abs graph", graph(stanli::OP_ABS, zero, inf));
    expect_disconnected("abs MIR", mir_var("abs", zero, inf));
  }
  const std::pair<const char*, uint16_t> discrete[] = {
      {"floor", stanli::OP_FLOOR},
      {"ceil", stanli::OP_CEIL},
      {"round", stanli::OP_ROUND},
      {"trunc", stanli::OP_TRUNC},
      {"step", stanli::OP_STEP}};
  for (const auto& item : discrete)
    for (double seed : {inf, std::numeric_limits<double>::quiet_NaN()}) {
      expect_disconnected(std::string(item.first) + " graph",
                          graph(item.second, 0.37, seed));
      expect_disconnected(std::string(item.first) + " MIR",
                          mir_var(item.first, 0.37, seed));
    }
}

void test_blocker_edges() {
#define EDGE(label, fn, opcode, x) \
  check_row(label, opcode, x, {},  \
            [](const stan::math::var& a) { return stan::math::fn(a); })
  EDGE("acosh", acosh, stanli::OP_ACOSH, 1.0);
  EDGE("acosh", acosh, stanli::OP_ACOSH, 0.5);
  EDGE("atanh", atanh, stanli::OP_ATANH, 1.0);
  EDGE("atanh", atanh, stanli::OP_ATANH, 1.1);
  EDGE("expm1", expm1, stanli::OP_EXPM1, 1e-16);
  EDGE("expm1", expm1, stanli::OP_EXPM1, -40.0);
  EDGE("log1m_exp", log1m_exp, stanli::OP_LOG1M_EXP, -1e-16);
  EDGE("log1m_exp", log1m_exp, stanli::OP_LOG1M_EXP, 0.0);
  EDGE("log1m_exp", log1m_exp, stanli::OP_LOG1M_EXP,
       std::numeric_limits<double>::infinity());
  EDGE("inv_Phi", inv_Phi, stanli::OP_INV_PHI, 1e-12);
  EDGE("tan", tan, stanli::OP_TAN, 0x1.921fb54442d17p+0);
#undef EDGE
}

void expect_close(const std::string& name, double got, double want,
                  double tolerance = 1e-12) {
  if (std::fabs(got - want) <= tolerance * std::max(1.0, std::fabs(want)))
    return;
  ++failures;
  std::printf("FAIL %-35s got %.17g want %.17g\n", name.c_str(), got, want);
}

void test_vector_jacobian() {
  FunDef f;
  f.name = "vector_rhs";
  f.arg_names = {"t", "y", "theta", "x_r", "x_i"};
  f.arg_types = {"UReal", "UVector", "UVector", "(UArray UReal)",
                 "(UArray UInt)"};
  f.body = {returning(call("sin", {variable("theta", "UVector")}, "UVector"))};
  const std::map<std::string, const FunDef*> defs{{f.name, &f}};
  stan::math::nested_rev_autodiff nested;
  const std::vector<stan::math::var> theta{-0.4, 0.2, 0.7};
  MirInterp<stan::math::var> interp(defs, "vector unary");
  auto out = interp.call(f,
                         {{stan::math::var(0)},
                          {stan::math::var(1)},
                          theta,
                          {stan::math::var(1.25)}},
                         {{2}});
  for (size_t row = 0; row < out.size(); ++row) {
    stan::math::set_zero_all_adjoints_nested();
    stan::math::grad(out[row].vi_);
    for (size_t col = 0; col < theta.size(); ++col)
      expect_close("vector Jacobian", theta[col].adj(),
                   row == col ? std::cos(theta[col].val()) : 0.0);
  }
}

void test_ode_fallback() {
  Expr rate = call("sin", {element(variable("theta", "UVector"), 1)}, "UReal");
  Expr value =
      call("Times__", {std::move(rate), element(variable("y", "UVector"), 1)},
           "UReal");
  FunDef f = rhs("ode_rhs", std::move(value));
  const std::map<std::string, const FunDef*> defs{{f.name, &f}};
  const std::vector<int> x_i{2};
  auto spec = std::make_shared<stanli::OdeSpec>();
  spec->adopt(defs);
  spec->rhs_name = f.name;
  spec->ts = {0.4};
  spec->x_r = {1.25};
  spec->x_i = x_i;
  spec->rtol = spec->atol = 1e-10;
  spec->max_steps = 100000;
  spec->args.resize(3);
  spec->args[0].is_param = true;
  spec->args[0].len = 1;
  spec->args[1].len = 1;
  spec->args[2].is_int = true;
  spec->args[2].ints = x_i;
  spec->prog = compile_rhs(f, defs, 1, 1, 1, x_i);

  stanli::Graph g;
  const int initial = g.add_slot(1, false);
  const int parameter = g.add_slot(1, true);
  const int solution = g.add_slot(1, false);
  const int op =
      g.add_op(stanli::OP_ODE, {initial, parameter}, solution, {1, 1});
  g.ops[(size_t)op].udata = spec.get();
  g.udata_pool.push_back(spec);
  g.result_slot = solution;
  stanli::Executor executor(std::move(g));
  *executor.value_ptr(initial) = 1.1;
  *executor.param_ptr(parameter) = 0.2;
  double gradient = 0;
  const double got = executor.gradient(&gradient);
  const double want = 1.1 * std::exp(std::sin(0.2) * 0.4);
  if (spec->prog.ok ||
      spec->prog.why.find("function sin") == std::string::npos) {
    ++failures;
    std::printf("FAIL ODE did not exercise unary fallback: %s\n",
                spec->prog.why.c_str());
  }
  expect_close("ODE fallback value", got, want, 2e-8);
  expect_close("ODE fallback gradient", gradient, want * 0.4 * std::cos(0.2),
               2e-8);
}

}  // namespace

int main() {
  test_manifest_and_rows();
  test_program_and_topology();
  test_blocker_edges();
  test_vector_jacobian();
  test_ode_fallback();
  if (failures == 0) std::printf("test_mir_unary_fallback: all cases passed\n");
  return failures == 0 ? 0 : 1;
}
