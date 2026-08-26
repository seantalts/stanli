// Compatibility and rejection tests for the portable-MIR decoder. The JSON
// writer in this file is deliberately test-only: the production writer lives
// in OCaml beside stanc3's typed MIR.
#include <stanli/compile.hpp>
#include <stanli/mir_decode.hpp>

#include "../runtime/third_party/nlohmann_json.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

using json = nlohmann::json;
using namespace stanli;

int failures = 0;

void check(bool ok, const std::string& what) {
  if (!ok) {
    ++failures;
    std::printf("FAIL %s\n", what.c_str());
  }
}

std::string slurp(const std::string& path) {
  std::ifstream input(path);
  std::ostringstream contents;
  contents << input.rdbuf();
  return contents.str();
}

template <typename T, typename Write>
json write_array(const std::vector<T>& values, Write write) {
  json result = json::array();
  for (const T& value : values) result.push_back(write(value));
  return result;
}

json write_strings(const std::vector<std::string>& values) {
  json result = json::array();
  for (const std::string& value : values) result.push_back(value);
  return result;
}

json write_bools(const std::vector<bool>& values) {
  json result = json::array();
  for (bool value : values) result.push_back(value);
  return result;
}

const char* write_leaf(mir::UnsizedLeaf value) {
  switch (value) {
    case mir::UnsizedLeaf::Unknown:
      return "Unknown";
    case mir::UnsizedLeaf::Int:
      return "Int";
    case mir::UnsizedLeaf::Real:
      return "Real";
    case mir::UnsizedLeaf::Complex:
      return "Complex";
    case mir::UnsizedLeaf::Vector:
      return "Vector";
    case mir::UnsizedLeaf::RowVector:
      return "RowVector";
    case mir::UnsizedLeaf::Matrix:
      return "Matrix";
  }
  return "Unknown";
}

json write_unsized(const mir::UnsizedView& value) {
  return {{"depth", value.depth}, {"leaf", write_leaf(value.leaf)}};
}

const char* write_expr_kind(mir::Expr::Kind value) {
  switch (value) {
    case mir::Expr::Var:
      return "Var";
    case mir::Expr::LitInt:
      return "LitInt";
    case mir::Expr::LitReal:
      return "LitReal";
    case mir::Expr::LitStr:
      return "LitStr";
    case mir::Expr::FunApp:
      return "FunApp";
    case mir::Expr::Promotion:
      return "Promotion";
    case mir::Expr::Indexed:
      return "Indexed";
    case mir::Expr::TernaryIf:
      return "TernaryIf";
    case mir::Expr::EOr:
      return "EOr";
    case mir::Expr::EAnd:
      return "EAnd";
    case mir::Expr::Unsupported:
      return "Unsupported";
  }
  return "Unsupported";
}

const char* write_expr_lib(mir::Expr::Lib value) {
  switch (value) {
    case mir::Expr::Lib::StanLib:
      return "StanLib";
    case mir::Expr::Lib::Internal:
      return "Internal";
    case mir::Expr::Lib::UserDefined:
      return "UserDefined";
  }
  return "StanLib";
}

std::string write_f64(double value) {
  static_assert(sizeof(double) == sizeof(uint64_t) &&
                    std::numeric_limits<double>::is_iec559 &&
                    std::numeric_limits<double>::digits == 53,
                "portable MIR requires binary64 doubles");
  uint64_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  char text[21];
  std::snprintf(text, sizeof(text), "f64:%016llx",
                static_cast<unsigned long long>(bits));
  return text;
}

json write_expr(const mir::Expr& value) {
  return {{"kind", write_expr_kind(value.kind)},
          {"name", value.name},
          {"fn_lib", write_expr_lib(value.fn_lib)},
          {"fn_propto", value.fn_propto},
          {"lit_i", std::to_string(value.lit_i)},
          {"lit", write_f64(value.lit)},
          {"lit_s", value.lit_s},
          {"args", write_array(value.args, write_expr)},
          {"type_", value.type_},
          {"unsized", write_unsized(value.unsized)},
          {"data_only", value.data_only},
          {"promoted", value.promoted},
          {"raw", value.raw}};
}

const char* write_transform_kind(mir::Transform::Kind value) {
  switch (value) {
    case mir::Transform::Identity:
      return "Identity";
    case mir::Transform::Lower:
      return "Lower";
    case mir::Transform::Upper:
      return "Upper";
    case mir::Transform::LowerUpper:
      return "LowerUpper";
    case mir::Transform::Offset:
      return "Offset";
    case mir::Transform::Multiplier:
      return "Multiplier";
    case mir::Transform::OffsetMultiplier:
      return "OffsetMultiplier";
    case mir::Transform::Simplex:
      return "Simplex";
    case mir::Transform::Ordered:
      return "Ordered";
    case mir::Transform::PositiveOrdered:
      return "PositiveOrdered";
    case mir::Transform::CholeskyCorr:
      return "CholeskyCorr";
    case mir::Transform::UnitVector:
      return "UnitVector";
    case mir::Transform::SumToZero:
      return "SumToZero";
    case mir::Transform::Correlation:
      return "Correlation";
    case mir::Transform::Covariance:
      return "Covariance";
    case mir::Transform::CholeskyCov:
      return "CholeskyCov";
    case mir::Transform::Unsupported:
      return "Unsupported";
  }
  return "Unsupported";
}

json write_transform(const mir::Transform& value) {
  return {{"kind", write_transform_kind(value.kind)},
          {"args", write_array(value.args, write_expr)},
          {"raw", value.raw}};
}

json write_sized_type(const mir::SizedType& value) {
  return {{"base", value.base},
          {"dims", write_array(value.dims, write_expr)},
          {"elem_base", value.elem_base},
          {"raw", value.raw}};
}

const char* write_stmt_kind(mir::Stmt::Kind value) {
  switch (value) {
    case mir::Stmt::Decl:
      return "Decl";
    case mir::Stmt::Assignment:
      return "Assignment";
    case mir::Stmt::TargetPE:
      return "TargetPE";
    case mir::Stmt::Block:
      return "Block";
    case mir::Stmt::SList:
      return "SList";
    case mir::Stmt::For:
      return "For";
    case mir::Stmt::IfElse:
      return "IfElse";
    case mir::Stmt::While:
      return "While";
    case mir::Stmt::NRFunApp:
      return "NRFunApp";
    case mir::Stmt::Return:
      return "Return";
    case mir::Stmt::Skip:
      return "Skip";
    case mir::Stmt::Unsupported:
      return "Unsupported";
  }
  return "Unsupported";
}

json write_stmt(const mir::Stmt& value) {
  return {{"kind", write_stmt_kind(value.kind)},
          {"decl_id", value.decl_id},
          {"decl_type", write_sized_type(value.decl_type)},
          {"decl_data_only", value.decl_data_only},
          {"has_init", value.has_init},
          {"init", write_expr(value.init)},
          {"read_transform", value.read_transform
                                 ? write_transform(*value.read_transform)
                                 : json(nullptr)},
          {"read_dims", write_array(value.read_dims, write_expr)},
          {"lhs", value.lhs},
          {"lhs_idx", write_array(value.lhs_idx, write_expr)},
          {"rhs", write_expr(value.rhs)},
          {"target", write_expr(value.target)},
          {"fn_name", value.fn_name},
          {"fn_args", write_array(value.fn_args, write_expr)},
          {"check_transform", value.check_transform
                                  ? write_transform(*value.check_transform)
                                  : json(nullptr)},
          {"check_var_name", value.check_var_name},
          {"loopvar", value.loopvar},
          {"lower", write_expr(value.lower)},
          {"upper", write_expr(value.upper)},
          {"cond", write_expr(value.cond)},
          {"body", write_array(value.body, write_stmt)},
          {"raw", value.raw}};
}

json write_fun_def(const mir::FunDef& value) {
  return {{"name", value.name},
          {"arg_names", write_strings(value.arg_names)},
          {"arg_types", write_strings(value.arg_types)},
          {"arg_views", write_array(value.arg_views, write_unsized)},
          {"arg_data_only", write_bools(value.arg_data_only)},
          {"body", write_array(value.body, write_stmt)}};
}

json write_program_object(const mir::Program& value) {
  json input_vars = json::array();
  for (const auto& input : value.input_vars)
    input_vars.push_back(
        {{"name", input.first}, {"type", write_sized_type(input.second)}});
  return {{"input_vars", std::move(input_vars)},
          {"prepare_data", write_array(value.prepare_data, write_stmt)},
          {"log_prob", write_array(value.log_prob, write_stmt)},
          {"generate_quantities",
           write_array(value.generate_quantities, write_stmt)},
          {"fun_defs", write_array(value.fun_defs, write_fun_def)},
          {"output_vars", write_strings(value.output_vars)}};
}

json write_envelope(const mir::Program& value) {
  return {{"stanli_ir", 1}, {"program", write_program_object(value)}};
}

bool expect_error(const std::string& text, const std::string& needle,
                  const std::string& what) {
  try {
    (void)decode_program(text);
  } catch (const std::exception& error) {
    const bool found =
        std::string(error.what()).find(needle) != std::string::npos;
    check(found, what + " diagnostic: " + error.what());
    return true;
  }
  check(false, what + " was accepted");
  return false;
}

bool expect_compile_error(const std::string& text, const std::string& needle,
                          const std::string& what) {
  try {
    DataMap data;
    (void)compile_model(text, data);
  } catch (const std::exception& error) {
    const bool found =
        std::string(error.what()).find(needle) != std::string::npos;
    check(found, what + " diagnostic: " + error.what());
    return true;
  }
  check(false, what + " was accepted");
  return false;
}

mir::Expr literal(double value = 1.0) {
  mir::Expr expression;
  expression.kind = mir::Expr::LitReal;
  expression.lit = value;
  expression.type_ = "UReal";
  expression.unsized.leaf = mir::UnsizedLeaf::Real;
  expression.data_only = true;
  return expression;
}

mir::Program target_program(mir::Expr expression) {
  mir::Program program;
  mir::Stmt statement;
  statement.kind = mir::Stmt::TargetPE;
  statement.target = std::move(expression);
  program.log_prob.push_back(std::move(statement));
  return program;
}

void strip_overload_suffix(std::string& name) {
  const size_t signature = name.find('(');
  if (signature != std::string::npos) name.erase(signature);
}

void strip_overload_suffixes(mir::Expr& value) {
  if (value.kind == mir::Expr::FunApp &&
      value.fn_lib == mir::Expr::Lib::UserDefined)
    strip_overload_suffix(value.name);
  for (mir::Expr& arg : value.args) strip_overload_suffixes(arg);
}

void strip_overload_suffixes(mir::Transform& value) {
  for (mir::Expr& arg : value.args) strip_overload_suffixes(arg);
}

void strip_overload_suffixes(mir::Stmt& value) {
  for (mir::Expr* expression : {&value.init, &value.rhs, &value.target,
                                &value.lower, &value.upper, &value.cond})
    strip_overload_suffixes(*expression);
  for (std::vector<mir::Expr>* expressions :
       {&value.read_dims, &value.lhs_idx, &value.fn_args})
    for (mir::Expr& expression : *expressions)
      strip_overload_suffixes(expression);
  for (mir::Expr& dim : value.decl_type.dims) strip_overload_suffixes(dim);
  if (value.read_transform) strip_overload_suffixes(*value.read_transform);
  if (value.check_transform) strip_overload_suffixes(*value.check_transform);
  for (mir::Stmt& child : value.body) strip_overload_suffixes(child);
}

void strip_overload_suffixes(mir::Program& value) {
  for (auto& input : value.input_vars)
    for (mir::Expr& dim : input.second.dims) strip_overload_suffixes(dim);
  for (std::vector<mir::Stmt>* body :
       {&value.prepare_data, &value.log_prob, &value.generate_quantities})
    for (mir::Stmt& stmt : *body) strip_overload_suffixes(stmt);
  for (mir::FunDef& function : value.fun_defs) {
    strip_overload_suffix(function.name);
    for (mir::Stmt& stmt : function.body) strip_overload_suffixes(stmt);
  }
}

mir::Program read_fixture(const std::string& path) {
  const std::string text = slurp(path);
  check(!text.empty(), path + " exists");
  return decode_program(text);
}

void clear_raw(mir::Expr& value) {
  value.raw.clear();
  for (mir::Expr& arg : value.args) clear_raw(arg);
}

void clear_raw(mir::Transform& value) {
  value.raw.clear();
  for (mir::Expr& arg : value.args) clear_raw(arg);
}

void clear_raw(mir::SizedType& value) {
  value.raw.clear();
  for (mir::Expr& dim : value.dims) clear_raw(dim);
}

void clear_raw(mir::Stmt& value) {
  value.raw.clear();
  clear_raw(value.decl_type);
  for (mir::Expr* expression : {&value.init, &value.rhs, &value.target,
                                &value.lower, &value.upper, &value.cond})
    clear_raw(*expression);
  for (std::vector<mir::Expr>* expressions :
       {&value.read_dims, &value.lhs_idx, &value.fn_args})
    for (mir::Expr& expression : *expressions) clear_raw(expression);
  if (value.read_transform) clear_raw(*value.read_transform);
  if (value.check_transform) clear_raw(*value.check_transform);
  for (mir::Stmt& child : value.body) clear_raw(child);
}

void clear_raw(mir::Program& value) {
  for (auto& input : value.input_vars) clear_raw(input.second);
  for (std::vector<mir::Stmt>* body :
       {&value.prepare_data, &value.log_prob, &value.generate_quantities})
    for (mir::Stmt& statement : *body) clear_raw(statement);
  for (mir::FunDef& function : value.fun_defs)
    for (mir::Stmt& statement : function.body) clear_raw(statement);
}

void check_program_equivalence(const std::string& legacy_path,
                               const std::string& portable_path) {
  mir::Program legacy = read_fixture(legacy_path);
  mir::Program portable = read_fixture(portable_path);
  // Portable v1 keeps complete opaque diagnostic payloads; the legacy reader
  // truncates some of them. They do not participate in execution, so compare
  // every structural/semantic field after removing only those payloads.
  clear_raw(legacy);
  clear_raw(portable);
  check(write_program_object(portable) == write_program_object(legacy),
        portable_path + " decoded fields match " + legacy_path);
}

void check_round_trip(const std::string& path) {
  const mir::Program legacy = read_fixture(path);
  const mir::Program portable = decode_program(write_envelope(legacy).dump());
  check(write_program_object(portable) == write_program_object(legacy),
        path + " portable fields match legacy fields");
}

void check_lowering_equivalence(const char* legacy_fixture,
                                const char* portable_fixture) {
  const std::string legacy_text =
      slurp(legacy_fixture ? legacy_fixture : "tests/fixtures/es.tmir.sexp");
  const mir::Program legacy_program = decode_program(legacy_text);
  const std::string portable_text = portable_fixture
                                        ? slurp(portable_fixture)
                                        : write_envelope(legacy_program).dump();
  check(!portable_text.empty(), "portable lowering fixture exists");
  if (legacy_fixture && portable_fixture)
    check_program_equivalence(legacy_fixture, portable_fixture);

  DataMap data;
  data.set_int("J", 8);
  data.set_real_array("y", {28, 8, -3, 7, -1, 1, 18, 12}, {8});
  data.set_real_array("sigma", {15, 10, 16, 11, 9, 11, 10, 18}, {8});

  CompiledModel legacy = compile_model(legacy_text, data);
  CompiledModel portable = compile_model(portable_text, data);
  check(legacy.graph.ops.size() == portable.graph.ops.size(),
        "portable lowering op count");
  check(legacy.graph.slots.size() == portable.graph.slots.size(),
        "portable lowering slot count");
  check(legacy.fills == portable.fills, "portable lowering fills");
  check(legacy.param_names == portable.param_names,
        "portable lowering parameter names");
  check(legacy.n_unconstrained == portable.n_unconstrained,
        "portable lowering parameter count");

  Executor legacy_executor(std::move(legacy.graph));
  Executor portable_executor(std::move(portable.graph));
  legacy.bind(legacy_executor);
  portable.bind(portable_executor);
  check(legacy_executor.n_params() == portable_executor.n_params(),
        "portable executor parameter count");
  const int64_t count = legacy_executor.n_params();
  for (int64_t i = 0; i < count; ++i) {
    const double value = 0.03 * static_cast<double>(i) - 0.1;
    legacy_executor.params_data()[i] = value;
    portable_executor.params_data()[i] = value;
  }
  std::vector<double> legacy_gradient(static_cast<size_t>(count));
  std::vector<double> portable_gradient(static_cast<size_t>(count));
  const double legacy_lp = legacy_executor.gradient(legacy_gradient.data());
  const double portable_lp =
      portable_executor.gradient(portable_gradient.data());
  check(legacy_lp == portable_lp, "portable lowering lp bitwise");
  check(legacy_gradient == portable_gradient,
        "portable lowering gradient bitwise");
}

void check_overload_finalization() {
  const mir::Program resolved =
      read_fixture("tests/fixtures/overload.tmir.sexp");
  mir::Program unresolved = resolved;
  strip_overload_suffixes(unresolved);
  bool has_overloads = false;
  for (size_t i = 0; i < unresolved.fun_defs.size(); ++i)
    for (size_t j = i + 1; j < unresolved.fun_defs.size(); ++j)
      has_overloads |=
          unresolved.fun_defs[i].name == unresolved.fun_defs[j].name;
  check(has_overloads, "overload fixture contains pre-resolution collisions");
  const mir::Program decoded =
      decode_program(write_envelope(unresolved).dump());
  check(write_program_object(decoded) == write_program_object(resolved),
        "portable decoder applies legacy overload finalization once");
}

void check_exact_float_bits() {
  mir::Program program;
  mir::Stmt target;
  target.kind = mir::Stmt::TargetPE;
  target.target.kind = mir::Expr::LitReal;
  target.target.type_ = "UReal";
  target.target.unsized.leaf = mir::UnsizedLeaf::Real;
  target.target.data_only = true;
  program.log_prob.push_back(target);

  const std::vector<uint64_t> patterns = {
      0x0000000000000000ULL, 0x8000000000000000ULL, 0x7ff0000000000000ULL,
      0xfff0000000000000ULL, 0x7ff8000000000042ULL, 0x0000000000000001ULL};
  for (uint64_t bits : patterns) {
    std::memcpy(&program.log_prob[0].target.lit, &bits, sizeof(bits));
    const mir::Program decoded = decode_program(write_envelope(program).dump());
    uint64_t got = 0;
    std::memcpy(&got, &decoded.log_prob[0].target.lit, sizeof(got));
    check(got == bits,
          "f64 payload " + write_f64(program.log_prob[0].target.lit));
  }
}

void check_structural_rejections() {
  mir::Expr indexed;
  indexed.kind = mir::Expr::Indexed;
  indexed.type_ = "UReal";
  indexed.unsized.leaf = mir::UnsizedLeaf::Real;
  expect_compile_error(write_envelope(target_program(indexed)).dump(),
                       "Indexed expression arity",
                       "empty Indexed rejected at compile boundary");

  indexed.args.push_back(literal());
  const mir::Program base_only =
      decode_program(write_envelope(target_program(indexed)).dump());
  check(base_only.log_prob[0].target.args.size() == 1,
        "base-only Indexed wrapper");

  mir::Expr bad_index;
  bad_index.kind = mir::Expr::FunApp;
  bad_index.name = "IndexSingle";
  indexed.args.push_back(bad_index);
  expect_error(write_envelope(target_program(indexed)).dump(),
               "IndexSingle call", "IndexSingle without operand");

  bad_index.name = "IndexBetween";
  bad_index.args.push_back(literal());
  indexed.args[1] = bad_index;
  expect_error(write_envelope(target_program(indexed)).dump(),
               "IndexBetween call", "IndexBetween with one endpoint");

  bad_index.name = "IndexUpfrom";
  bad_index.args.clear();
  indexed.args[1] = bad_index;
  expect_error(write_envelope(target_program(indexed)).dump(),
               "IndexUpfrom call", "IndexUpfrom without lower bound");
  bad_index.args.push_back(literal());
  indexed.args[1] = bad_index;
  const mir::Program upfrom =
      decode_program(write_envelope(target_program(indexed)).dump());
  check(upfrom.log_prob[0].target.args[1].name == "IndexUpfrom",
        "IndexUpfrom portable shape");

  bad_index.name = "FutureIndex";
  indexed.args[1] = bad_index;
  expect_error(write_envelope(target_program(indexed)).dump(),
               "index descriptor", "unknown synthetic index");

  bad_index.name = "IndexSingle";
  bad_index.fn_lib = mir::Expr::Lib::UserDefined;
  indexed.args[1] = bad_index;
  expect_error(write_envelope(target_program(indexed)).dump(),
               "index descriptor", "noncanonical synthetic index library");

  for (const auto& test : std::vector<std::pair<mir::Expr::Kind, const char*>>{
           {mir::Expr::Promotion, "Promotion expression arity"},
           {mir::Expr::TernaryIf, "TernaryIf expression arity"},
           {mir::Expr::EOr, "EOr expression arity"},
           {mir::Expr::EAnd, "EAnd expression arity"}}) {
    mir::Expr expression;
    expression.kind = test.first;
    expression.type_ = "UReal";
    expression.unsized.leaf = mir::UnsizedLeaf::Real;
    expect_error(write_envelope(target_program(expression)).dump(), test.second,
                 test.second);
  }

  mir::Expr bad_call;
  bad_call.kind = mir::Expr::FunApp;
  bad_call.name = "Plus__";
  bad_call.type_ = "UReal";
  bad_call.unsized.leaf = mir::UnsizedLeaf::Real;
  bad_call.args.push_back(literal());
  expect_compile_error(write_envelope(target_program(bad_call)).dump(),
                       "Plus__ call", "known function arity");
  for (const char* name : {"pow", "std_normal_qf", "trigamma", "is_nan",
                           "tcrossprod", "map_rect", "algebra_solver"}) {
    bad_call.name = name;
    bad_call.args.clear();
    expect_error(write_envelope(target_program(bad_call)).dump(),
                 std::string(name) + " call",
                 std::string(name) + " malformed arity");
  }
  bad_call.name = "pow";
  bad_call.fn_lib = mir::Expr::Lib::Internal;
  expect_error(write_envelope(target_program(bad_call)).dump(), "pow call",
               "internal function cannot bypass name-dispatched arity");

  mir::Expr wiener;
  wiener.kind = mir::Expr::FunApp;
  wiener.name = "wiener_lpdf";
  wiener.type_ = "UReal";
  wiener.unsized.leaf = mir::UnsizedLeaf::Real;
  wiener.args.assign(7, literal());
  const std::string extended_wiener =
      write_envelope(target_program(wiener)).dump();
  const mir::Program decoded_wiener = decode_program(extended_wiener);
  check(decoded_wiener.log_prob.size() == 1 &&
            decoded_wiener.log_prob[0].target.args.size() == 7,
        "portable decoder accepts seven-argument wiener_lpdf");
  expect_compile_error(extended_wiener, "unsupported function wiener_lpdf",
                       "seven-argument wiener reaches execution boundary");
  wiener.args.resize(6);
  expect_error(write_envelope(target_program(wiener)).dump(),
               "expected 5 or 7 argument(s)",
               "six-argument wiener rejected structurally");

  mir::Expr bad_metadata = literal();
  bad_metadata.unsized.leaf = mir::UnsizedLeaf::Int;
  expect_error(write_envelope(target_program(bad_metadata)).dump(),
               "expression type metadata", "expression type/view mismatch");

  mir::Program bad_binding;
  mir::SizedType real_type;
  real_type.base = "SReal";
  mir::Expr x;
  x.kind = mir::Expr::Var;
  x.name = "x";
  x.type_ = "UInt";
  x.unsized.leaf = mir::UnsizedLeaf::Int;
  bad_binding = target_program(x);
  bad_binding.input_vars.emplace_back("x", real_type);
  expect_error(write_envelope(bad_binding).dump(),
               "variable type disagrees with its binding",
               "variable binding type mismatch");

  mir::Program bad_function;
  mir::FunDef function;
  function.name = "f";
  function.arg_names.push_back("x");
  function.arg_types.push_back("(UArray UReal)");
  function.arg_views.push_back(mir::UnsizedView{0, mir::UnsizedLeaf::Real});
  function.arg_data_only.push_back(true);
  bad_function.fun_defs.push_back(std::move(function));
  expect_error(write_envelope(bad_function).dump(),
               "function argument type disagrees with its view",
               "function argument type/view mismatch");

  mir::Program colliding_functions;
  const auto add_function = [&](const std::string& name,
                                const std::string& type,
                                mir::UnsizedLeaf leaf) {
    mir::FunDef definition;
    definition.name = name;
    definition.arg_names.push_back("x");
    definition.arg_types.push_back(type);
    definition.arg_views.push_back(mir::UnsizedView{0, leaf});
    definition.arg_data_only.push_back(false);
    colliding_functions.fun_defs.push_back(std::move(definition));
  };
  add_function("f", "UReal", mir::UnsizedLeaf::Real);
  add_function("f", "UInt", mir::UnsizedLeaf::Int);
  add_function("f(int)", "UInt", mir::UnsizedLeaf::Int);
  expect_error(write_envelope(colliding_functions).dump(),
               "duplicate function name after overload resolution",
               "overload suffix collision");

  const auto bad_body = [](mir::Stmt::Kind kind, size_t children) {
    mir::Program program;
    mir::Stmt statement;
    statement.kind = kind;
    statement.body.resize(children);
    program.log_prob.push_back(std::move(statement));
    return program;
  };
  expect_error(write_envelope(bad_body(mir::Stmt::For, 0)).dump(),
               "For statement body arity", "empty For body");
  expect_error(write_envelope(bad_body(mir::Stmt::For, 2)).dump(),
               "For statement body arity", "multiple For bodies");
  expect_error(write_envelope(bad_body(mir::Stmt::While, 0)).dump(),
               "While statement body arity", "empty While body");
  expect_error(write_envelope(bad_body(mir::Stmt::IfElse, 0)).dump(),
               "IfElse statement body arity", "empty IfElse body");
  expect_error(write_envelope(bad_body(mir::Stmt::IfElse, 3)).dump(),
               "IfElse statement body arity", "three IfElse bodies");
  expect_error(write_envelope(bad_body(mir::Stmt::TargetPE, 1)).dump(),
               "malformed statement body", "body on leaf statement");

  mir::Program bad_transform;
  mir::Stmt declaration;
  declaration.kind = mir::Stmt::Decl;
  declaration.decl_type.base = "SReal";
  declaration.read_transform = mir::Transform{};
  declaration.read_transform->kind = mir::Transform::Lower;
  bad_transform.log_prob.push_back(std::move(declaration));
  expect_error(write_envelope(bad_transform).dump(), "transform arity",
               "transform argument arity");

  mir::Program bad_sized;
  mir::SizedType matrix_type;
  matrix_type.base = "SMatrix";
  matrix_type.dims.push_back(literal());
  bad_sized.input_vars.emplace_back("M", std::move(matrix_type));
  expect_error(write_envelope(bad_sized).dump(),
               "SMatrix sized type dimensions", "sized matrix dimensions");
}

void check_rejections() {
  const mir::Program empty;
  const json valid = write_envelope(empty);

  json changed = valid;
  changed["stanli_ir"] = 2;
  expect_error(changed.dump(), "unsupported version", "unknown version");

  changed = valid;
  changed["extra"] = true;
  expect_error(changed.dump(), "unknown field", "unknown envelope field");

  changed = valid;
  changed.erase("program");
  expect_error(changed.dump(), "missing required field", "missing program");

  changed = valid;
  changed["program"]["extra"] = true;
  expect_error(changed.dump(), "unknown field", "unknown program field");

  const std::string duplicate =
      "{\"stanli_ir\":1,\"stanli_ir\":1,\"program\":" +
      write_program_object(empty).dump() + "}";
  expect_error(duplicate, "duplicate JSON key", "duplicate key");

  const std::string nested_duplicate =
      "{\"stanli_ir\":1,\"program\":{\"input_vars\":[],\"input_vars\":[],"
      "\"prepare_data\":[],\"log_prob\":[],\"generate_quantities\":[],"
      "\"fun_defs\":[],\"output_vars\":[]}}";
  expect_error(nested_duplicate, "duplicate JSON key", "nested duplicate key");

  expect_error(valid.dump() + " trailing", "invalid JSON",
               "trailing portable content");
  expect_error("{ definitely not an s-expression", "invalid JSON",
               "malformed portable input does not fall back");
  expect_error("[]", "unrecognized input format", "unknown input format");
  expect_error(" \n\t", "empty input", "empty input");

  mir::Program literal_program;
  mir::Stmt stmt;
  stmt.kind = mir::Stmt::TargetPE;
  stmt.target.kind = mir::Expr::LitReal;
  stmt.target.type_ = "UReal";
  stmt.target.unsized.leaf = mir::UnsizedLeaf::Real;
  stmt.target.data_only = true;
  literal_program.log_prob.push_back(stmt);
  changed = write_envelope(literal_program);
  changed["program"]["log_prob"][0]["target"]["lit"] = "f64:000000000000000G";
  expect_error(changed.dump(), "lowercase hexadecimal", "invalid f64 bits");

  changed = write_envelope(literal_program);
  changed["program"]["log_prob"][0]["target"]["lit_i"] = 0;
  expect_error(changed.dump(), "expected string", "numeric Stan integer");

  changed = write_envelope(literal_program);
  changed["program"]["log_prob"][0]["target"]["lit_i"] = "2147483648";
  expect_error(changed.dump(), "signed 32-bit", "oversized Stan integer");

  changed = write_envelope(literal_program);
  changed["program"]["log_prob"][0]["target"]["kind"] = "FutureExpr";
  expect_error(changed.dump(), "unknown Expr::Kind", "unknown expression tag");

  mir::Program bad_function;
  mir::FunDef function;
  function.name = "bad";
  function.arg_names.push_back("x");
  bad_function.fun_defs.push_back(function);
  expect_error(write_envelope(bad_function).dump(),
               "function argument field lengths disagree",
               "parallel function argument arrays");

  std::string too_deep = "{\"stanli_ir\":1,\"program\":";
  too_deep.append(514, '[');
  too_deep += "0";
  too_deep.append(514, ']');
  too_deep += "}";
  expect_error(too_deep, "nesting exceeds", "JSON nesting limit");

  json too_many_members = json::object();
  for (int i = 0; i < 65; ++i)
    too_many_members["field_" + std::to_string(i)] = i;
  expect_error(too_many_members.dump(), "object exceeds 64 members",
               "JSON object-member limit");

  std::string oversized_string = "{\"";
  oversized_string.append(size_t{16} * 1024 * 1024 + 1, 'x');
  oversized_string += "\":0}";
  expect_error(oversized_string, "string exceeds 16777216 bytes",
               "JSON per-string limit");
}

}  // namespace

int main(int argc, char** argv) {
  const bool program_only =
      argc == 4 && std::string(argv[1]) == "--program-only";
  if (argc != 1 && argc != 3 && !program_only) {
    std::fprintf(stderr,
                 "usage: %s [legacy-es-mir portable-es-mir]\n"
                 "       %s --program-only legacy-mir portable-mir\n",
                 argv[0], argv[0]);
    return 2;
  }
  check_round_trip("tests/fixtures/es.tmir.sexp");
  check_round_trip("tests/fixtures/tdvocab.tmir.sexp");
  check_round_trip("tests/fixtures/wanames.tmir.sexp");
  check_round_trip("tests/fixtures/odefns.tmir.sexp");
  check_round_trip("tests/fixtures/view_udf_local_data_branch.tmir.sexp");
  if (program_only)
    check_program_equivalence(argv[2], argv[3]);
  else
    check_lowering_equivalence(argc == 3 ? argv[1] : nullptr,
                               argc == 3 ? argv[2] : nullptr);
  check_overload_finalization();
  check_exact_float_bits();
  check_structural_rejections();
  check_rejections();

  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::puts("ok");
  return 0;
}
