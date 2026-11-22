// Compatibility and rejection tests for the portable-MIR decoder. The JSON
// objects in this file are test-only deep-comparison values; the wire encoder
// below mirrors the production OCaml layout for malformed-input coverage.
#include <stanli/compile.hpp>
#include <stanli/mir_interp.hpp>
#include <stanli/graph.hpp>
#include <stanli/mir_decode.hpp>
#include <stanli/wa_interp.hpp>

#include "../runtime/src/mir_tuples.hpp"
#include "portable_mir_writer.hpp"
#include "../runtime/ext/nlohmann_json.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using json = nlohmann::json;
using namespace stanli;
using namespace stanli::portable_writer;

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

std::string empty_v2_payload() {
  std::string payload;
  for (int i = 0; i < 10; ++i) append_u32(payload, 0);
  return payload;
}

std::string real_target_v2_payload(uint64_t bits, uint8_t data_only = 1,
                                   uint8_t leaf = 2) {
  std::string payload;
  append_u32(payload, 0);  // input_vars
  append_u32(payload, 0);  // prepare_data
  append_u32(payload, 1);  // log_prob
  append_u8(payload, 2);   // TargetPE
  append_u8(payload, 2);   // LitReal
  append_u64(payload, bits);
  append_u8(payload, 0);  // unsized array depth
  append_u8(payload, leaf);
  append_u8(payload, data_only);
  append_u8(payload, 0);  // promoted
  append_string(payload, "");
  append_string(payload, "");  // statement raw
  append_u32(payload, 0);      // generate_quantities
  append_u32(payload, 0);      // transformed_parameters_begin
  append_u32(payload, 0);      // generated_quantities_begin
  append_u32(payload, 0);      // fun_defs
  append_u32(payload, 0);      // output_vars
  append_u32(payload, 0);      // transform_inits
  append_u32(payload, 0);      // tuple_leaves
  return payload;
}

// Write each whole assignment `lhs = value` whose value has `view` as the
// full-span assignment `lhs[:] = value`; return how many there were.
size_t replace_assignment_with_all(std::string& text, const std::string& lhs,
                                   mir::UnsizedView view) {
  mir::Program program = decode_program(text);
  size_t count = 0;
  for_each_stmt(program, [&](mir::Stmt& statement) {
    if (statement.kind == mir::Stmt::Assignment && statement.lhs == lhs &&
        statement.lhs_idx.empty() &&
        statement.rhs.unsized.depth == view.depth &&
        statement.rhs.unsized.leaf == view.leaf) {
      statement.lhs_idx = {index(mir::Expr::IndexAll, {})};
      ++count;
    }
  });
  text = write_v2(program);
  return count;
}

size_t count_full_span_assignments(const std::vector<mir::Stmt>& body,
                                   const std::string& lhs) {
  size_t count = 0;
  for (const mir::Stmt& statement : body) {
    count += statement.kind == mir::Stmt::Assignment && statement.lhs == lhs &&
             statement.lhs_idx.size() == 1 &&
             statement.lhs_idx[0].kind == stanli::mir::Expr::IndexAll;
    count += count_full_span_assignments(statement.body, lhs);
  }
  return count;
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
    case mir::Expr::IndexAll:
      return "IndexAll";
    case mir::Expr::IndexSingle:
      return "IndexSingle";
    case mir::Expr::IndexUpfrom:
      return "IndexUpfrom";
    case mir::Expr::IndexBetween:
      return "IndexBetween";
    case mir::Expr::IndexMulti:
      return "IndexMulti";
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

json write_function_type(const std::optional<mir::FunctionType>& value) {
  if (!value) return nullptr;
  return {{"args", write_array(value->args, write_unsized)},
          {"suffix", static_cast<int>(value->suffix)},
          {"propto", value->propto}};
}

json write_expr(const mir::Expr& value) {
  return {{"kind", write_expr_kind(value.kind)},
          {"name", value.name},
          {"fn_lib", write_expr_lib(value.fn_lib)},
          {"fn_suffix", static_cast<int>(value.fn_suffix)},
          {"fn_propto", value.fn_propto},
          {"fn_overload", value.fn_overload},
          {"fn_type", write_function_type(value.fn_type)},
          {"lit_i", std::to_string(value.lit_i)},
          {"lit", write_f64(value.lit)},
          {"lit_s", value.lit_s},
          {"args", write_array(value.args, write_expr)},
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
    case mir::Transform::StochasticColumn:
      return "StochasticColumn";
    case mir::Transform::StochasticRow:
      return "StochasticRow";
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
          {"raw", value.raw},
          {"unsized", write_unsized(value.unsized)}};
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
    case mir::Stmt::Break:
      return "Break";
    case mir::Stmt::Continue:
      return "Continue";
    case mir::Stmt::Skip:
      return "Skip";
    case mir::Stmt::Unsupported:
      return "Unsupported";
    case mir::Stmt::MultiAssign:
      return "MultiAssign";
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
          {"jacobian", value.jacobian},
          {"fn_lib", write_expr_lib(value.fn_lib)},
          {"fn_name", value.fn_name},
          {"fn_suffix", static_cast<int>(value.fn_suffix)},
          {"fn_overload", value.fn_overload},
          {"fn_args", write_array(value.fn_args, write_expr)},
          {"check_transform", value.check_transform
                                  ? write_transform(*value.check_transform)
                                  : json(nullptr)},
          {"check_var_name", value.check_var_name},
          {"write_transform", value.write_transform
                                  ? write_transform(*value.write_transform)
                                  : json(nullptr)},
          {"loopvar", value.loopvar},
          {"lower", write_expr(value.lower)},
          {"upper", write_expr(value.upper)},
          {"cond", write_expr(value.cond)},
          {"body", write_array(value.body, write_stmt)},
          {"raw", value.raw}};
}

json write_fun_def(const mir::FunDef& value) {
  return {{"name", value.name},
          {"overload", value.overload},
          {"arg_names", write_strings(value.arg_names)},
          {"arg_views", write_array(value.arg_views, write_unsized)},
          {"arg_data_only", write_bools(value.arg_data_only)},
          {"results", write_array(value.results, write_unsized)},
          {"body", write_array(value.body, write_stmt)}};
}

json write_output_var(const mir::OutputVar& value) {
  return {{"name", value.name},
          {"block", static_cast<int>(value.block)},
          {"type", write_sized_type(value.type)},
          {"annotations", write_strings(value.annotations)}};
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
          {"transformed_parameters_begin", value.transformed_parameters_begin},
          {"generated_quantities_begin", value.generated_quantities_begin},
          {"transform_inits", write_array(value.transform_inits, write_stmt)},
          {"fun_defs", write_array(value.fun_defs, write_fun_def)},
          {"output_vars", write_array(value.output_vars, write_output_var)}};
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

// Exercise the reader as well as lowering: constructing an Expr directly
// bypasses the arity validator that previously rejected log2()/log10().
void check_nullary_math_constants() {
  for (const auto& item : std::vector<std::pair<const char*, double>>{
           {"e", 0x1.5bf0a8b145769p+1},
           {"pi", 0x1.921fb54442d18p+1},
           {"log2", 0x1.62e42fefa39efp-1},
           {"log10", 0x1.26bb1bbb55516p+1},
           {"sqrt2", 0x1.6a09e667f3bcdp+0},
           {"machine_precision", std::numeric_limits<double>::epsilon()}}) {
    mir::Expr call = literal();
    call.kind = mir::Expr::FunApp;
    call.name = item.first;
    CompiledModel model =
        compile_model(write_v2(target_program(call)), DataMap{});
    Executor executor(std::move(model.graph));
    model.bind(executor);
    check(executor.gradient(nullptr) == item.second,
          std::string(item.first) + " nullary constant decodes and evaluates");

    // The two logarithm names still accept their ordinary unary overloads.
    if (call.name == "log2" || call.name == "log10") {
      call.args = {literal(call.name == "log2" ? 8.0 : 1000.0)};
      CompiledModel unary =
          compile_model(write_v2(target_program(call)), DataMap{});
      Executor unary_executor(std::move(unary.graph));
      unary.bind(unary_executor);
      check(unary_executor.gradient(nullptr) == 3.0,
            call.name + " unary overload preserved");
      call.args.push_back(literal());
    } else {
      call.args = {literal()};
    }
    expect_error(write_v2(target_program(call)), call.name + " call",
                 call.name + " wrong arity rejected");
  }
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

void strip_overload_suffixes(mir::Stmt& value) {
  mir::for_each_expr(value, [](mir::Expr& expression) {
    strip_overload_suffixes(expression);
  });
  for (mir::Stmt& child : value.body) strip_overload_suffixes(child);
}

void strip_overload_suffixes(mir::Program& value) {
  for (auto& input : value.input_vars)
    for (mir::Expr& dim : input.second.dims) strip_overload_suffixes(dim);
  for (std::vector<mir::Stmt>* body :
       {&value.prepare_data, &value.log_prob, &value.generate_quantities,
        &value.transform_inits})
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

// A decoded fixture survives a round trip through the test writer.
void check_round_trip(const std::string& path) {
  const mir::Program decoded = read_fixture(path);
  const mir::Program again = decode_program(write_v2(decoded));
  check(write_program_object(again) == write_program_object(decoded),
        path + " survives a round trip through the writer");
}

// The eight schools fixture lowers to the same graph after a round trip
// through the test writer.
void check_round_trip_lowering() {
  const std::string text = slurp("tests/fixtures/es.mir");
  const std::string again = write_v2(decode_program(text));

  DataMap data;
  data.set_int("J", 8);
  data.set_real_array("y", {28, 8, -3, 7, -1, 1, 18, 12}, {8});
  data.set_real_array("sigma", {15, 10, 16, 11, 9, 11, 10, 18}, {8});

  CompiledModel fixture = compile_model(text, data);
  CompiledModel written = compile_model(again, data);
  check(fixture.graph.ops.size() == written.graph.ops.size(),
        "round-trip lowering op count");
  check(fixture.graph.slots.size() == written.graph.slots.size(),
        "round-trip lowering slot count");
  check(fixture.fills == written.fills, "round-trip lowering fills");
  check(fixture.param_names == written.param_names,
        "round-trip lowering parameter names");
  check(fixture.n_unconstrained == written.n_unconstrained,
        "round-trip lowering parameter count");

  Executor fixture_executor(std::move(fixture.graph));
  Executor written_executor(std::move(written.graph));
  fixture.bind(fixture_executor);
  written.bind(written_executor);
  check(fixture_executor.n_params() == written_executor.n_params(),
        "round-trip executor parameter count");
  const int64_t count = fixture_executor.n_params();
  for (int64_t i = 0; i < count; ++i) {
    const double value = 0.03 * static_cast<double>(i) - 0.1;
    fixture_executor.params_data()[i] = value;
    written_executor.params_data()[i] = value;
  }
  std::vector<double> fixture_gradient(static_cast<size_t>(count));
  std::vector<double> written_gradient(static_cast<size_t>(count));
  const double fixture_lp = fixture_executor.gradient(fixture_gradient.data());
  const double written_lp = written_executor.gradient(written_gradient.data());
  check(fixture_lp == written_lp, "round-trip lowering lp bitwise");
  check(fixture_gradient == written_gradient,
        "round-trip lowering gradient bitwise");
}

void check_full_span_assignment_lowering() {
  const auto decoded_pair = [](const std::string& fixture_text,
                               const std::string& what) {
    const mir::Program fixture_program = decode_program(fixture_text);
    const std::string written_text = write_v2(fixture_program);
    const mir::Program written_program = decode_program(written_text);
    check(write_program_object(written_program) ==
              write_program_object(fixture_program),
          what + " survives a round trip through the writer");
    return std::make_pair(written_text, fixture_program);
  };
  const auto run_log = [](CompiledModel& model,
                          const std::vector<double>& params,
                          const std::string& what) {
    Executor executor(std::move(model.graph));
    model.bind(executor);
    check(executor.n_params() == static_cast<int64_t>(params.size()),
          what + " parameter width");
    const size_t n =
        std::min(params.size(), static_cast<size_t>(executor.n_params()));
    std::copy_n(params.begin(), n, executor.params_data());
    std::vector<double> gradient(static_cast<size_t>(executor.n_params()));
    const double lp = executor.gradient(gradient.data());
    return std::make_pair(lp, gradient);
  };

  // Arrays with vector and row-vector leaves exercise the complete logical
  // view check in graph lowering, including the leaf orientation carried by
  // the portable format.
  {
    std::string fixture =
        slurp("tests/fixtures/view_array_container_extract.mir");
    check(replace_assignment_with_all(fixture, "vs",
                                      view(mir::UnsizedLeaf::Vector, 1)) == 1,
          "vs assignment rewritten for fixture");
    check(replace_assignment_with_all(
              fixture, "rs", view(mir::UnsizedLeaf::RowVector, 1)) == 1,
          "rs assignment rewritten for fixture");
    const auto decoded =
        decoded_pair(fixture, "full-span array-container fixture");
    check(count_full_span_assignments(decoded.second.log_prob, "vs") == 1 &&
              count_full_span_assignments(decoded.second.log_prob, "rs") == 1,
          "decoder preserves array-container IndexAll LHS");
    CompiledModel fixture_model = compile_model(fixture, DataMap{});
    CompiledModel written_model = compile_model(decoded.first, DataMap{});
    const auto fixture_result =
        run_log(fixture_model, {0.25}, "array-container full-span");
    const auto written_result =
        run_log(written_model, {0.25}, "portable array-container full-span");
    check(fixture_result == written_result,
          "array-container full-span log_prob round-trip parity");
    check(fixture_result.first == 1630.25 &&
              fixture_result.second == std::vector<double>({1.0}),
          "array-container full-span log_prob value");
  }

  // Zero storage does not erase the declared array/vector geometry. A
  // complete assignment of array[2] vector[0] therefore succeeds only when
  // the logical views, rather than flat widths alone, agree.
  {
    std::string fixture =
        slurp("tests/fixtures/viewa_explicit_zero_vectors.mir");
    check(replace_assignment_with_all(fixture, "values",
                                      view(mir::UnsizedLeaf::Vector, 1)) == 1,
          "values assignment rewritten for fixture");
    const auto decoded =
        decoded_pair(fixture, "full-span zero-vector-array fixture");
    CompiledModel fixture_model = compile_model(fixture, DataMap{});
    CompiledModel written_model = compile_model(decoded.first, DataMap{});
    const auto fixture_result =
        run_log(fixture_model, {0.25}, "zero-vector-array full-span");
    const auto written_result =
        run_log(written_model, {0.25}, "portable zero-vector-array full-span");
    check(fixture_result == written_result && fixture_result.first == 200.25 &&
              fixture_result.second == std::vector<double>({1.0}),
          "zero-vector-array full-span value and round-trip parity");
  }

  // The row-vector form uses the same storage width as a vector but a
  // distinct logical view, so it is a focused orientation check.
  {
    std::string fixture = slurp("tests/fixtures/rep_row_vector.mir");
    check(replace_assignment_with_all(fixture, "x",
                                      view(mir::UnsizedLeaf::RowVector)) == 1,
          "x assignment rewritten for fixture");
    const auto decoded = decoded_pair(fixture, "full-span row-vector fixture");
    check(count_full_span_assignments(decoded.second.log_prob, "x") == 1,
          "decoder preserves row-vector IndexAll LHS");
    CompiledModel fixture_model = compile_model(fixture, DataMap{});
    CompiledModel written_model = compile_model(decoded.first, DataMap{});
    const auto fixture_result =
        run_log(fixture_model, {0.25}, "row-vector full-span");
    const auto written_result =
        run_log(written_model, {0.25}, "portable row-vector full-span");
    check(fixture_result == written_result,
          "row-vector full-span log_prob round-trip parity");

    mir::Program mismatch = decoded.second;
    std::function<bool(std::vector<mir::Stmt>&)> shorten =
        [&](std::vector<mir::Stmt>& body) {
          for (mir::Stmt& statement : body) {
            if (statement.kind == mir::Stmt::Assignment &&
                statement.lhs == "x" && statement.lhs_idx.size() == 1 &&
                statement.lhs_idx[0].kind == stanli::mir::Expr::IndexAll &&
                statement.rhs.args.size() == 2) {
              statement.rhs.args[1].lit_i = 3;
              statement.rhs.args[1].lit = 3.0;
              return true;
            }
            if (shorten(statement.body)) return true;
          }
          return false;
        };
    check(shorten(mismatch.log_prob),
          "full-span mismatch fixture found its assignment");
    expect_compile_error(write_v2(mismatch), "assignment width mismatch for x",
                         "full-span row-vector width mismatch");

    mir::Program wrong_view = decoded.second;
    std::function<bool(std::vector<mir::Stmt>&)> reorient =
        [&](std::vector<mir::Stmt>& body) {
          for (mir::Stmt& statement : body) {
            if (statement.kind == mir::Stmt::Assignment &&
                statement.lhs == "x" && statement.lhs_idx.size() == 1 &&
                statement.lhs_idx[0].kind == stanli::mir::Expr::IndexAll) {
              statement.rhs.name = "rep_vector";
              statement.rhs.unsized.leaf = mir::UnsizedLeaf::Vector;
              return true;
            }
            if (reorient(statement.body)) return true;
          }
          return false;
        };
    check(reorient(wrong_view.log_prob),
          "full-span view mismatch fixture found its assignment");
    expect_compile_error(write_v2(wrong_view),
                         "assignment logical view mismatch for x",
                         "full-span vector orientation mismatch");
  }

  // A transformed-data scalar array reaches MirInterp during concrete data
  // specialization, then the graph consumes the assigned values.
  {
    std::string fixture = slurp("tests/fixtures/rangeclamp.mir");
    check(replace_assignment_with_all(fixture, "xs",
                                      view(mir::UnsizedLeaf::Real, 1)) == 1,
          "xs assignment rewritten for fixture");
    const auto decoded =
        decoded_pair(fixture, "full-span scalar-array fixture");
    check(count_full_span_assignments(decoded.second.prepare_data, "xs") == 1,
          "decoder preserves scalar-array IndexAll LHS");
    DataMap data;
    data.set_int("K", 3);
    data.set_int("lo", 1);
    data.set_int("hi", 4);
    data.set_real_array("Y", {0, 0, 0, 0}, {4});
    data.set_real_array("Zm", std::vector<double>(8, 0.0), {4, 2});
    CompiledModel fixture_model = compile_model(fixture, data);
    CompiledModel written_model = compile_model(decoded.first, data);
    const auto fixture_result =
        run_log(fixture_model, {0.125}, "scalar-array full-span");
    const auto written_result =
        run_log(written_model, {0.125}, "portable scalar-array full-span");
    check(fixture_result == written_result,
          "scalar-array full-span specialization round-trip parity");
  }

  // An integer array initialized through a complete assignment is consumed
  // later as integer data. Compiling the downstream ldexp expressions proves
  // the assignment retained both initialized-prefix and observed-value
  // specialization instead of leaving a typeless real slot behind.
  {
    std::string fixture = slurp("tests/fixtures/binint.mir");
    check(replace_assignment_with_all(fixture, "expo",
                                      view(mir::UnsizedLeaf::Int, 2)) == 1,
          "expo assignment rewritten for fixture");
    const auto decoded = decoded_pair(fixture, "full-span int-array fixture");
    const DataMap data = DataMap::from_json_file("tests/fixtures/binint.json");
    const CompiledModel fixture_model = compile_model(fixture, data);
    const CompiledModel written_model = compile_model(decoded.first, data);
    check(fixture_model.n_unconstrained == written_model.n_unconstrained &&
              fixture_model.fills == written_model.fills &&
              fixture_model.graph.ops.size() == written_model.graph.ops.size(),
          "int-array full-span specialization round-trip parity");
  }

  // Generated quantities execute a vector full-span assignment whose RHS
  // uses a draw-dependent array index. Run both formats with identical RNG
  // streams and compare every emitted column and the stream position.
  {
    std::string fixture = slurp("tests/fixtures/gq_runtime_control.mir");
    check(replace_assignment_with_all(fixture, "chosen",
                                      view(mir::UnsizedLeaf::Vector)) == 1,
          "chosen assignment rewritten for fixture");
    const auto decoded =
        decoded_pair(fixture, "full-span generated-vector fixture");
    check(count_full_span_assignments(decoded.second.generate_quantities,
                                      "chosen") == 1,
          "decoder preserves generated-vector IndexAll LHS");
    CompiledModel fixture_model = compile_model(fixture, DataMap{});
    CompiledModel written_model = compile_model(decoded.first, DataMap{});
    const std::vector<double> params = {
        0.2,           -0.3,          0.1,  -0.2, 1.0,  2.0,  3.0,  4.0,
        std::log(1.1), std::log(1.7), 0.25, -0.5, 0.75, -1.0, 1.25, -1.5};
    const auto fixture_log =
        run_log(fixture_model, params, "generated-vector full-span");
    const auto written_log =
        run_log(written_model, params, "portable generated-vector full-span");
    check(fixture_log == written_log,
          "generated-vector full-span log_prob round-trip parity");

    check(fixture_model.write_array && written_model.write_array,
          "full-span generated-vector write_array exists");
    if (fixture_model.write_array && written_model.write_array) {
      check(!fixture_model.write_array->interp &&
                !fixture_model.write_array->truncated &&
                !written_model.write_array->interp &&
                !written_model.write_array->truncated,
            "full-span generated-vector write_array lowers completely");
      check(CompiledModel::csv_names(fixture_model.write_array->columns) ==
                CompiledModel::csv_names(written_model.write_array->columns),
            "full-span generated-vector output columns");
      Executor fixture_write(std::move(fixture_model.write_array->graph));
      Executor written_write(std::move(written_model.write_array->graph));
      fixture_model.write_array->bind(fixture_write);
      written_model.write_array->bind(written_write);
      check(fixture_write.n_params() == static_cast<int64_t>(params.size()) &&
                written_write.n_params() == static_cast<int64_t>(params.size()),
            "full-span generated-vector write_array parameter width");
      std::copy(params.begin(), params.end(), fixture_write.params_data());
      std::copy(params.begin(), params.end(), written_write.params_data());
      WaRng fixture_rng(81), written_rng(81);
      fixture_write.run_forward_only(EvalState{&fixture_rng});
      written_write.run_forward_only(EvalState{&written_rng});
      const auto collect = [](const CompiledModel::WriteArray& write_array,
                              Executor& executor) {
        std::vector<double> row;
        for (const auto& column : write_array.columns) {
          const double* values = executor.value_ptr(column.slot);
          for (int64_t i = 0; i < column.len; ++i)
            row.push_back(values[column.storage_index(i)]);
        }
        return row;
      };
      const std::vector<double> fixture_row =
          collect(*fixture_model.write_array, fixture_write);
      const std::vector<double> written_row =
          collect(*written_model.write_array, written_write);
      const bool same_row =
          fixture_row.size() == written_row.size() &&
          std::memcmp(fixture_row.data(), written_row.data(),
                      fixture_row.size() * sizeof(double)) == 0;
      check(same_row,
            "full-span generated-vector write_array value round-trip parity");
      check(fixture_rng.gen()() == written_rng.gen()(),
            "full-span generated-vector RNG stream parity");
    }
  }
}

void check_overload_finalization() {
  const mir::Program resolved = read_fixture("tests/fixtures/overload.mir");
  mir::Program unresolved = resolved;
  strip_overload_suffixes(unresolved);
  bool has_overloads = false;
  for (size_t i = 0; i < unresolved.fun_defs.size(); ++i)
    for (size_t j = i + 1; j < unresolved.fun_defs.size(); ++j)
      has_overloads |=
          unresolved.fun_defs[i].name == unresolved.fun_defs[j].name;
  check(has_overloads, "overload fixture contains pre-resolution collisions");
  const mir::Program decoded = decode_program(write_v2(unresolved));
  check(write_program_object(decoded) == write_program_object(resolved),
        "portable decoder applies overload finalization once");
}

namespace build {

const mir::UnsizedView kInt{0, mir::UnsizedLeaf::Int};
const mir::UnsizedView kReal{0, mir::UnsizedLeaf::Real};
const mir::UnsizedView kVector{0, mir::UnsizedLeaf::Vector};
const mir::UnsizedView kMatrix{0, mir::UnsizedLeaf::Matrix};

mir::Expr integer(long value) {
  mir::Expr e;
  e.kind = mir::Expr::LitInt;
  e.lit_i = value;
  e.lit = static_cast<double>(value);
  e.unsized = kInt;
  e.data_only = true;
  return e;
}

mir::Expr real(double value) { return literal(value); }

mir::Expr var(const std::string& name, mir::UnsizedView view,
              bool data_only = false) {
  mir::Expr e;
  e.kind = mir::Expr::Var;
  e.name = name;
  e.unsized = view;
  e.data_only = data_only;
  return e;
}

mir::Expr call(const std::string& name, std::vector<mir::Expr> args,
               mir::UnsizedView view,
               mir::Expr::Lib lib = mir::Expr::Lib::StanLib) {
  mir::Expr e;
  e.kind = mir::Expr::FunApp;
  e.name = name;
  e.fn_lib = lib;
  e.unsized = view;
  e.data_only = true;
  for (const mir::Expr& arg : args) e.data_only = e.data_only && arg.data_only;
  e.args = std::move(args);
  return e;
}

mir::Expr index(mir::Expr base, long i, mir::UnsizedView view) {
  mir::Expr single;
  single.kind = mir::Expr::IndexSingle;
  single.args.push_back(integer(i));
  mir::Expr e;
  e.kind = mir::Expr::Indexed;
  e.unsized = view;
  e.data_only = base.data_only;
  e.args = {std::move(base), std::move(single)};
  return e;
}

mir::SizedType sized(const std::string& base, std::vector<long> dims = {},
                     const std::string& elem_base = "") {
  mir::SizedType type;
  type.base = base;
  type.elem_base = elem_base;
  for (long d : dims) type.dims.push_back(integer(d));
  return type;
}

mir::Stmt decl(const std::string& name, mir::SizedType type,
               bool data_only = false) {
  mir::Stmt s;
  s.kind = mir::Stmt::Decl;
  s.decl_id = name;
  s.decl_type = std::move(type);
  s.decl_data_only = data_only;
  return s;
}

mir::Stmt assign(const std::string& lhs, mir::Expr rhs) {
  mir::Stmt s;
  s.kind = mir::Stmt::Assignment;
  s.lhs = lhs;
  s.rhs = std::move(rhs);
  return s;
}

mir::Stmt target(mir::Expr e) {
  mir::Stmt s;
  s.kind = mir::Stmt::TargetPE;
  s.target = std::move(e);
  return s;
}

mir::Stmt block(std::vector<mir::Stmt> body) {
  mir::Stmt s;
  s.kind = mir::Stmt::Block;
  s.body = std::move(body);
  return s;
}

mir::Stmt multi_assign(std::vector<mir::Expr> targets, mir::Expr call) {
  mir::Stmt s;
  s.kind = mir::Stmt::MultiAssign;
  s.fn_args = std::move(targets);
  s.rhs = std::move(call);
  return s;
}

mir::Stmt read_data(const std::string& name, mir::UnsizedView view,
                    const std::string& key) {
  mir::Stmt s;
  s.kind = mir::Stmt::NRFunApp;
  s.fn_lib = mir::Expr::Lib::Internal;
  s.fn_name = "FnReadData";
  mir::Expr text;
  text.kind = mir::Expr::LitStr;
  text.lit_s = key;
  text.data_only = true;
  s.fn_args = {var(name, view, true), var("context__", {}, true), text};
  return s;
}

// A real parameter read whole, unconstrained.
mir::Stmt real_parameter(const std::string& name) {
  mir::Stmt s = decl(name, sized("SReal"));
  s.has_init = true;
  s.init = call("FnReadParam", {}, kReal, mir::Expr::Lib::Internal);
  s.init.data_only = false;
  s.read_transform = mir::Transform{};
  return s;
}

}  // namespace build

struct Density {
  double value;
  double gradient;
};

Density density(const std::string& wire, const DataMap& data, double theta) {
  CompiledModel model = compile_model(wire, data);
  Executor executor(std::move(model.graph));
  model.bind(executor);
  if (executor.n_params() > 0) executor.params_data()[0] = theta;
  double gradient = 0;
  const double value = executor.gradient(&gradient);
  return {value, gradient};
}

// f(x) = (x * x, [x, x]'): a function returning a tuple of two leaves.
mir::FunDef two_results() {
  using namespace build;
  mir::FunDef f;
  f.name = "f";
  f.arg_names = {"x"};
  f.arg_views = {kReal};
  f.arg_data_only = {false};
  f.results = {kReal, kVector};
  mir::Stmt ret;
  ret.kind = mir::Stmt::Return;
  ret.fn_args = {call("Times__", {var("x", kReal), var("x", kReal)}, kReal),
                 call("rep_vector", {var("x", kReal), integer(2)}, kVector)};
  f.body = {ret};
  return f;
}

mir::Expr call_f(mir::Expr argument) {
  mir::Expr e =
      build::call("f", {std::move(argument)}, {}, mir::Expr::Lib::UserDefined);
  e.data_only = false;
  return e;
}

// `(a, b) = f(x)`, `target += a + sum(b)`.
std::vector<mir::Stmt> use_f(mir::Expr x) {
  using namespace build;
  return {
      multi_assign({var("a", kReal), var("b", kVector)}, call_f(x)),
      target(call("Plus__",
                  {var("a", kReal), call("sum", {var("b", kVector)}, kReal)},
                  kReal))};
}

void check_multi_results() {
  using namespace build;
  mir::Program program;
  program.fun_defs = {two_results()};
  program.log_prob = {real_parameter("theta"), decl("a", sized("SReal")),
                      decl("b", sized("SVector", {2}))};
  for (mir::Stmt& s : use_f(var("theta", kReal))) program.log_prob.push_back(s);
  const std::string wire = write_v2(program);
  const mir::Program decoded = decode_program(wire);
  check(decoded.fun_defs.at(0).results.size() == 2 &&
            decoded.fun_defs[0].body.at(0).fn_args.size() == 2 &&
            !decoded.fun_defs[0].body[0].has_init &&
            decoded.log_prob.at(3).kind == mir::Stmt::MultiAssign &&
            decoded.log_prob[3].fn_args.size() == 2 &&
            decoded.log_prob[3].fn_args[1].name == "b" &&
            decoded.log_prob[3].rhs.name == "f",
        "MultiAssign and a return of several values decode");
  // theta^2 + 2 theta at 3, and its derivative 2 theta + 2.
  const Density at_three = density(wire, DataMap{}, 3.0);
  check(at_three.value == 15.0 && at_three.gradient == 8.0,
        "graph lowering assigns each result of f");

  // In a loop, three times.
  mir::Program looped = program;
  looped.log_prob.resize(3);
  mir::Stmt loop;
  loop.kind = mir::Stmt::For;
  loop.loopvar = "i";
  loop.lower = integer(1);
  loop.upper = integer(3);
  loop.body = {block(use_f(var("theta", kReal)))};
  looped.log_prob.push_back(loop);
  const Density thrice = density(write_v2(looped), DataMap{}, 3.0);
  check(thrice.value == 45.0 && thrice.gradient == 24.0,
        "a MultiAssign in a loop assigns on every trip");

  // In a while loop whose condition reads the parameter:
  // k = theta; while (k < 10) { (a, b) = f(k); k = a; target += sum(b); }
  // At theta = 3 the body runs for k = 3 and k = 9, adding 2k each time:
  // 6 + 18, with derivative 2 + 4 theta.
  mir::Program dynamic = program;
  dynamic.log_prob.resize(3);
  mir::Stmt k = decl("k", sized("SReal"));
  dynamic.log_prob.push_back(k);
  dynamic.log_prob.push_back(assign("k", var("theta", kReal)));
  mir::Stmt w;
  w.kind = mir::Stmt::While;
  w.cond = call("Less__", {var("k", kReal), real(10)}, kInt);
  w.cond.data_only = false;
  w.body = {block({multi_assign({var("a", kReal), var("b", kVector)},
                                call_f(var("k", kReal))),
                   assign("k", var("a", kReal)),
                   target(call("sum", {var("b", kVector)}, kReal))})};
  dynamic.log_prob.push_back(w);
  const Density looping = density(write_v2(dynamic), DataMap{}, 3.0);
  check(looping.value == 24.0 && looping.gradient == 14.0,
        "a MultiAssign in a parameter-dependent while loop");

  // Transformed data runs in the MIR interpreter: f(2) = (4, [2, 2]').
  mir::Program data = program;
  data.prepare_data = {decl("a", sized("SReal"), true),
                       decl("b", sized("SVector", {2}), true)};
  data.prepare_data.push_back(multi_assign(
      {var("a", kReal, true), var("b", kVector, true)}, call_f(real(2))));
  data.prepare_data.back().rhs.data_only = true;
  data.log_prob = {
      real_parameter("theta"),
      target(call("Plus__",
                  {var("a", kReal, true),
                   call("Times__",
                        {call("sum", {var("b", kVector, true)}, kReal),
                         var("theta", kReal)},
                        kReal)},
                  kReal))};
  data.log_prob.back().target.data_only = false;
  data.log_prob.back().target.args[1].data_only = false;
  const Density prepared = density(write_v2(data), DataMap{}, 3.0);
  check(prepared.value == 16.0 && prepared.gradient == 4.0,
        "the MIR interpreter assigns each result in transformed data");

  // The interpreter's entry point for a function returning a tuple.
  std::map<std::string, const mir::FunDef*> functions;
  for (const mir::FunDef& f : decoded.fun_defs)
    functions[mir::function_key(f)] = &f;
  MirInterp<double> interpreter(functions, "test");
  DataMap::Entry x;
  x.r = {3.0};
  const std::vector<DataMap::Entry> results =
      interpreter.call_values(decoded.fun_defs[0], {x});
  check(results.size() == 2 && results[0].r == std::vector<double>{9.0} &&
            results[1].r == std::vector<double>{3.0, 3.0},
        "the interpreter returns each result of a function returning a "
        "tuple");

  // Validation.
  mir::Program short_targets = program;
  short_targets.log_prob[3].fn_args.pop_back();
  expect_error(write_v2(short_targets), "MultiAssign",
               "a MultiAssign of one value");
  mir::Program three = program;
  three.log_prob.push_back(decl("c", sized("SReal")));
  three.log_prob[3].fn_args.push_back(var("c", kReal));
  std::swap(three.log_prob[3], three.log_prob.back());
  std::swap(three.log_prob[3], three.log_prob[4]);
  expect_error(write_v2(three), "f returns 2 values, not 3",
               "a MultiAssign with more targets than results");
  mir::Program in_expression = program;
  in_expression.log_prob.resize(3);
  in_expression.log_prob.push_back(target(call_f(var("theta", kReal))));
  expect_error(write_v2(in_expression), "returns several values outside",
               "a call of a tuple function outside a MultiAssign");
  mir::Program bad_return = program;
  bad_return.fun_defs[0].body[0].fn_args.push_back(real(1));
  expect_error(write_v2(bad_return), "gives 3 values, not 2",
               "a return with the wrong number of values");
  mir::Program undeclared = program;
  undeclared.log_prob.erase(undeclared.log_prob.begin() + 2);
  expect_error(write_v2(undeclared), "target b is not declared",
               "a MultiAssign target without a declaration");
}

// Stan Math's tuple-returning decomposition of theta * A, summed over every
// entry of every result, and its derivative in theta: what CmdStan computes
// for `target += sum(r1) + sum(r2) ...`.
template <typename F>
Density decomposition_reference(const Eigen::MatrixXd& a, double theta, F f) {
  stan::math::nested_rev_autodiff nested;
  stan::math::var t = theta;
  const Eigen::Matrix<stan::math::var, -1, -1> m = t * a;
  stan::math::var total = 0;
  std::apply(
      [&](const auto&... part) { ((total += stan::math::sum(part)), ...); },
      f(m));
  stan::math::grad(total.vi_);
  return {total.val(), t.adj()};
}

bool close_ulp(double got, double want, double ulps) {
  return std::isfinite(got) &&
         std::fabs(got - want) <= ulps *
                                      std::numeric_limits<double>::epsilon() *
                                      std::max(std::fabs(want), 1.0);
}

// qr, qr_thin, eigendecompose_sym and svd factor their argument once and
// slice every result out of the one evaluation, which is what Stan Math's
// tuple-returning function computes. csr_extract runs when its matrix is
// data; its sizes depend on the values otherwise.
void check_decompositions() {
  using namespace build;
  DataMap data;
  data.set_real_array("A", {4, 1, 2, 1, 3, 0, 2, 0, 5}, {3, 3});
  Eigen::MatrixXd a(3, 3);
  a << 4, 1, 2, 1, 3, 0, 2, 0, 5;
  const mir::UnsizedView kMatrix{0, mir::UnsizedLeaf::Matrix};
  const mir::UnsizedView kVector{0, mir::UnsizedLeaf::Vector};
  using M = Eigen::Matrix<stan::math::var, -1, -1>;
  const std::vector<std::pair<std::string, std::vector<mir::UnsizedView>>>
      cases = {{"qr", {kMatrix, kMatrix}},
               {"qr_thin", {kMatrix, kMatrix}},
               {"eigendecompose_sym", {kMatrix, kVector}},
               {"svd", {kMatrix, kVector, kMatrix}}};
  for (const auto& [name, views] : cases) {
    mir::Program program;
    program.input_vars.emplace_back("A", sized("SMatrix", {3, 3}));
    program.prepare_data = {decl("A", sized("SMatrix", {3, 3}), true),
                            read_data("A", kMatrix, "A")};
    program.log_prob = {real_parameter("theta")};
    std::vector<mir::Expr> targets;
    mir::Expr total;
    for (size_t k = 0; k < views.size(); ++k) {
      const std::string target_name = "r" + std::to_string(k);
      program.log_prob.push_back(decl(target_name,
                                      views[k].leaf == mir::UnsizedLeaf::Vector
                                          ? sized("SVector", {3})
                                          : sized("SMatrix", {3, 3}),
                                      false));
      targets.push_back(var(target_name, views[k], false));
      const mir::Expr sum = call("sum", {targets.back()}, kReal);
      total = k == 0 ? sum : call("Plus__", {total, sum}, kReal);
    }
    total.data_only = false;
    mir::Expr argument = call(
        "Times__", {var("theta", kReal), var("A", kMatrix, true)}, kMatrix);
    argument.data_only = false;
    mir::Expr decomposition = call(name, {argument}, {});
    decomposition.data_only = false;
    program.log_prob.push_back(multi_assign(targets, decomposition));
    program.log_prob.push_back(target(total));
    const Density want =
        name == "qr" ? decomposition_reference(
                           a, 0.5, [](const M& m) { return stan::math::qr(m); })
        : name == "qr_thin"
            ? decomposition_reference(
                  a, 0.5, [](const M& m) { return stan::math::qr_thin(m); })
        : name == "svd"
            ? decomposition_reference(
                  a, 0.5, [](const M& m) { return stan::math::svd(m); })
            : decomposition_reference(a, 0.5, [](const M& m) {
                return stan::math::eigendecompose_sym(m);
              });
    Density got{std::nan(""), std::nan("")};
    std::string error;
    try {
      got = density(write_v2(program), data, 0.5);
    } catch (const std::exception& e) {
      error = e.what();
    }
    check(close_ulp(got.value, want.value, 4) &&
              close_ulp(got.gradient, want.gradient, 4),
          name + " shares one factorization like Stan Math's " + name + " (" +
              error + ")");
  }

  // csr_extract of a data matrix, folded at preparation; of a parameter, a
  // clear refusal.
  data.set_real_array("S", {1, 0, 0, 3, 2, 0}, {2, 3});
  mir::Program csr;
  csr.input_vars.emplace_back("S", sized("SMatrix", {2, 3}));
  csr.prepare_data = {decl("S", sized("SMatrix", {2, 3}), true),
                      read_data("S", kMatrix, "S")};
  const mir::UnsizedView ints{1, mir::UnsizedLeaf::Int};
  csr.log_prob = {
      real_parameter("theta"), decl("w", sized("SVector", {3}), true),
      decl("v", sized("SArray", {3}, "SInt"), true),
      decl("u", sized("SArray", {3}, "SInt"), true),
      multi_assign(
          {var("w", kVector, true), var("v", ints, true), var("u", ints, true)},
          call("csr_extract", {var("S", kMatrix, true)}, {}))};
  // S = [[1, 0, 2], [0, 3, 0]]: w = [1, 2, 3] row by row, v = [1, 3, 2]
  // and u = [1, 3, 4], so the target is 6 * theta + 6 + 8.
  mir::Expr weighted =
      call("Times__",
           {call("sum", {var("w", kVector, true)}, kReal), var("theta", kReal)},
           kReal);
  weighted.data_only = false;
  mir::Expr csr_total =
      call("Plus__",
           {weighted, call("Plus__",
                           {call("sum", {var("v", ints, true)}, kInt),
                            call("sum", {var("u", ints, true)}, kInt)},
                           kInt)},
           kReal);
  csr_total.data_only = false;
  csr.log_prob.push_back(target(csr_total));
  Density folded{std::nan(""), std::nan("")};
  std::string csr_error;
  try {
    folded = density(write_v2(csr), data, 2.0);
  } catch (const std::exception& e) {
    csr_error = e.what();
  }
  check(
      folded.value == 6.0 * 2.0 + 6.0 + 8.0 && folded.gradient == 6.0,
      "csr_extract of a data matrix folds at preparation (" + csr_error + ")");
  mir::Program active = csr;
  mir::Expr scaled =
      call("Times__", {var("theta", kReal), var("S", kMatrix, true)}, kMatrix);
  scaled.data_only = false;
  active.log_prob[4].rhs.args = {scaled};
  active.log_prob[4].rhs.data_only = false;
  for (size_t k = 1; k < 4; ++k) active.log_prob[k].decl_data_only = false;
  for (mir::Expr& t : active.log_prob[4].fn_args) t.data_only = false;
  bool refused = false;
  try {
    (void)compile_model(write_v2(active), data);
  } catch (const std::exception& e) {
    refused = std::string(e.what()).find(
                  "csr_extract: the number of nonzero entries depends on the "
                  "values, so the matrix must be data") != std::string::npos;
  }
  check(refused, "csr_extract of a parameter matrix is refused");
  mir::Program short_qr = csr;
  short_qr.log_prob[4].rhs.name = "qr_thin";
  short_qr.log_prob[4].rhs.args = {var("S", kMatrix, true)};
  expect_error(write_v2(short_qr), "qr_thin returns 2 values, not 3",
               "a library decomposition with too many targets");
}

// The optimizer can turn every write of a tuple output into a literal, so
// the cursor places writes by the columns of each leaf. A write of no
// columns takes the next empty leaf of the current variable, before or
// after its other leaves, and only then moves to the next variable.
void check_output_cursor_leaves() {
  std::vector<mir::OutputVar> vars(3);
  vars[0].name = "a";  // tuple(array[0] real, real, vector[0])
  vars[1].name = "b";  // tuple(matrix[0, 0], int)
  vars[2].name = "c";  // real
  const std::map<std::string, std::vector<int64_t>> parts = {
      {"a", {0, 1, 0}}, {"b", {0, 1}}, {"c", {1}}};
  mir::OutputCursor cursor(vars);
  std::string got;
  for (const int64_t width : {0, 1, 0, 0, 1, 1}) {
    const mir::OutputVar* var =
        cursor.take("", std::nullopt, width,
                    [&](const mir::OutputVar& v) { return parts.at(v.name); });
    got += var ? var->name : std::string("none");
    got += cursor.part() ? std::to_string(*cursor.part()) : std::string("-");
    got += " ";
  }
  check(got == "a0 a1 a2 b0 b1 c0 ",
        "literal writes take the leaves of tuple outputs in order: " + got);

  // tuple(array[2] vector[0], vector[0], real): the first leaf is written
  // element by element, so it gets no write, and a write that reads the
  // second leaf belongs to it.
  std::vector<mir::OutputVar> one(1);
  one[0].name = "g";
  mir::OutputCursor reads(one);
  const auto g_parts = [](const mir::OutputVar&) {
    return std::vector<int64_t>{0, 0, 1};
  };
  (void)reads.take("g", size_t{1}, 0, g_parts);
  check(reads.part() == std::optional<size_t>(1),
        "a write of an empty leaf is placed in the leaf it reads");

  // A tuple with no array of tuples inside gets one write per leaf that is
  // written whole, so once its leaves are all taken, a write of no columns
  // that reads no output belongs to the next variable. t is
  // tuple(array[0] vector[0], tuple(matrix[0, 0], row_vector[0])): its first
  // leaf is written element by element and gets no write. An array of empty
  // vectors, x, still takes each such write.
  std::vector<mir::OutputVar> empties(3);
  empties[0].name = "t";
  empties[1].name = "u";
  empties[2].name = "x";
  const std::map<std::string, std::vector<int64_t>> empty_parts = {
      {"t", {0, 0, 0}}, {"u", {0, 0}}, {"x", {0}}};
  mir::OutputCursor after(empties, {"t", "u"});
  got.clear();
  const std::vector<std::pair<std::string, std::optional<size_t>>> writes = {
      {"t", 1},           {"t", 2},           {"", std::nullopt},
      {"", std::nullopt}, {"", std::nullopt}, {"", std::nullopt}};
  for (const auto& [name, part] : writes) {
    const mir::OutputVar* var = after.take(
        name, part, 0,
        [&](const mir::OutputVar& v) { return empty_parts.at(v.name); });
    got += var ? var->name : std::string("none");
    got += after.part() ? std::to_string(*after.part()) : std::string("-");
    got += " ";
  }
  check(got == "t1 t2 u0 u1 x0 x- ",
        "a write of no columns after a complete tuple moves on: " + got);
}

// `array[3] tuple(int, vector[2]) d` arrives as two leaf variables with the
// tuple's type beside them; nothing reads their names. CmdStan's JSON reader
// stores each leaf element by element under its key.
void check_tuple_interface() {
  using namespace build;
  mir::detail::TupleType tuple;
  tuple.type = sized("SArray", {3}, "STuple");
  tuple.components = {{sized("SInt"), {}}, {sized("SVector", {2}), {}}};
  WireTuples wire;
  wire.types["d"] = tuple;
  wire.leaves = {{"d_a", "d", {{1, 1}}}, {"d_b", "d", {{1, 2}}}};
  const mir::UnsizedView ints{1, mir::UnsizedLeaf::Int};
  const mir::UnsizedView vectors{1, mir::UnsizedLeaf::Vector};
  mir::Program program;
  program.input_vars.emplace_back("d", tuple.type);
  program.prepare_data = {decl("d_a", sized("SArray", {3}, "SInt"), true),
                          read_data("d_a", ints, "d.1"),
                          decl("d_b", sized("SArray", {3, 2}, "SVector"), true),
                          read_data("d_b", vectors, "d.2")};
  program.log_prob = {
      real_parameter("theta"),
      target(
          call("sum", {index(var("d_b", vectors, true), 1, kVector)}, kReal)),
      target(call(
          "Times__",
          {call("sum", {var("d_a", ints, true)}, kInt), var("theta", kReal)},
          kReal))};
  program.log_prob.back().target.data_only = false;
  const std::string text = write_v2(program, wire);
  const mir::Program decoded = decode_program(text);
  check(decoded.input_vars.size() == 2 &&
            decoded.input_vars[0].first == "d_a" &&
            decoded.input_vars[0].second.base == "SArray" &&
            decoded.input_vars[0].second.elem_base == "SInt" &&
            decoded.input_vars[1].first == "d_b" &&
            decoded.input_vars[1].second.dims.size() == 2,
        "a tuple input becomes its leaf variables");
  const mir::detail::TupleLeaf* leaf = mir::detail::tuple_leaf(decoded, "d_b");
  check(leaf && leaf->base == "d" && leaf->key == "d.2" &&
            leaf->path.size() == 1 && leaf->path[0].before == 1 &&
            leaf->path[0].component == 2 && leaf->type.dims.size() == 2,
        "the leaf records its tuple variable, slot and CmdStan key");
  const auto name =
      mir::detail::output_name("d", leaf, std::vector<int64_t>{3, 1});
  check(name && name->name == "d.3:2.1" && name->variable == "d.3:2" &&
            name->index == std::vector<int64_t>{1},
        "CmdStan names a tuple slot with ':' and indices with '.'");
  check(!mir::detail::output_name("d", leaf, {}),
        "a whole array of tuples has no single column name");

  DataMap data;
  data.set_int_array("d.1", {1, 2, 3}, {3});
  // d[1].2 = [1, 2], d[2].2 = [3, 4], d[3].2 = [5, 6], element by element.
  data.set_real_array("d.2", {1, 2, 3, 4, 5, 6}, {3, 2});
  const Density at = density(text, data, 2.0);
  check(at.value == 3.0 + 6.0 * 2.0 && at.gradient == 6.0,
        "tuple data is read under its keys in CmdStan's element order");

  // stanc3 prefixes a C++ keyword with _stan_, which the finalizer strips;
  // the CmdStan key is the source name.
  WireTuples mangled_wire;
  mangled_wire.types["_stan_class"] = tuple;
  mangled_wire.leaves = {{"_stan_class.1", "_stan_class", {{1, 1}}},
                         {"_stan_class.2", "_stan_class", {{1, 2}}}};
  mir::Program mangled = program;
  mangled.input_vars[0].first = "_stan_class";
  for (size_t k : {0u, 2u})
    mangled.prepare_data[k].decl_id =
        k == 0 ? "_stan_class.1" : "_stan_class.2";
  mangled.prepare_data[1].fn_args[0].name = "_stan_class.1";
  mangled.prepare_data[1].fn_args[2].lit_s = "class.1";
  mangled.prepare_data[3].fn_args[0].name = "_stan_class.2";
  mangled.prepare_data[3].fn_args[2].lit_s = "class.2";
  mangled.log_prob[1].target.args[0].args[0].name = "_stan_class.2";
  mangled.log_prob[2].target.args[0].args[0].name = "_stan_class.1";
  const mir::Program unmangled =
      decode_program(write_v2(mangled, mangled_wire));
  const mir::detail::TupleLeaf* keyword =
      mir::detail::tuple_leaf(unmangled, "class.2");
  check(keyword && keyword->base == "class" && keyword->key == "class.2" &&
            unmangled.input_vars[1].first == "class.2",
        "a tuple named by a C++ keyword is unmangled with its leaves");

  WireTuples missing = wire;
  missing.leaves.pop_back();
  expect_error(write_v2(program, missing), "a leaf of d has no variable",
               "a tuple leaf without a variable");
  WireTuples stray = wire;
  stray.leaves.push_back({"d_c", "d", {{1, 3}}});
  expect_error(write_v2(program, stray), "no leaf of a tuple variable",
               "a leaf the tuple type does not have");
  mir::Program wrong_key = program;
  wrong_key.prepare_data[3].fn_args[2].lit_s = "d_b";
  expect_error(write_v2(wrong_key, wire), "not read under its key d.2",
               "a data leaf read under another key");
  mir::Program local = program;
  local.log_prob.push_back(decl("g", sized("STuple")));
  expect_error(write_v2(local, wire), "a tuple type outside",
               "a tuple type in a declaration");

  // An array of tuples prints as stan_print spells it.
  mir::Program printed = program;
  mir::Stmt print;
  print.kind = mir::Stmt::NRFunApp;
  print.fn_lib = mir::Expr::Lib::Internal;
  print.fn_name = "FnReject";
  mir::Expr label;
  label.kind = mir::Expr::LitStr;
  label.lit_s = "d=";
  print.fn_args = {label, var("d_a", ints, true), var("d_b", vectors, true)};
  mir::PrintItem item;
  item.kind = mir::PrintItem::Kind::Tuple;
  item.depth = 1;
  item.components.resize(2);
  print.print_layout = {mir::PrintItem{}, item};
  printed.log_prob.push_back(print);
  const mir::Program decoded_print = decode_program(write_v2(printed, wire));
  check(
      decoded_print.log_prob.back().print_layout.size() == 2 &&
          decoded_print.log_prob.back().print_layout[1].components.size() == 2,
      "a print layout decodes");
  std::string message;
  try {
    (void)density(write_v2(printed, wire), data, 2.0);
  } catch (const std::exception& error) {
    message = error.what();
  }
  check(message.find("d=[(1,[1,2]),(2,[3,4]),(3,[5,6])]") != std::string::npos,
        "an array of tuples is printed as stan_print spells it: " + message);
  printed.log_prob.back().print_layout[1].components.pop_back();
  expect_error(write_v2(printed, wire), "print layout does not take every",
               "a print layout that leaves an argument");
}

void check_exact_float_bits() {
  mir::Program program;
  mir::Stmt target;
  target.kind = mir::Stmt::TargetPE;
  target.target.kind = mir::Expr::LitReal;
  target.target.unsized.leaf = mir::UnsizedLeaf::Real;
  target.target.data_only = true;
  program.log_prob.push_back(target);

  const std::vector<uint64_t> patterns = {
      0x0000000000000000ULL, 0x8000000000000000ULL, 0x7ff0000000000000ULL,
      0xfff0000000000000ULL, 0x7ff8000000000042ULL, 0x0000000000000001ULL};
  for (uint64_t bits : patterns) {
    std::memcpy(&program.log_prob[0].target.lit, &bits, sizeof(bits));
    const mir::Program decoded = decode_program(write_v2(program));
    uint64_t got = 0;
    std::memcpy(&got, &decoded.log_prob[0].target.lit, sizeof(got));
    check(got == bits,
          "f64 payload " + write_f64(program.log_prob[0].target.lit));
  }
}

void check_output_annotations() {
  mir::Program annotated = decode_program(slurp("tests/fixtures/es.mir"));
  check(!annotated.output_vars.empty() &&
            annotated.output_vars[0].annotations.empty(),
        "fixture has an unannotated output variable");
  if (annotated.output_vars.empty()) return;
  const std::vector<std::string> want = {"latent", "foo"};
  annotated.output_vars[0].annotations = want;
  const std::string text = write_v2(annotated);

  const mir::Program portable = decode_program(text);
  check(portable.output_vars[0].annotations == want,
        "portable reader keeps output annotation names");
  for (size_t i = 1; i < portable.output_vars.size(); ++i)
    check(portable.output_vars[i].annotations.empty(),
          "unannotated output variables stay empty");

  DataMap data;
  data.set_int("J", 8);
  data.set_real_array("y", {28, 8, -3, 7, -1, 1, 18, 12}, {8});
  data.set_real_array("sigma", {15, 10, 16, 11, 9, 11, 10, 18}, {8});
  const CompiledModel model = compile_model(text, data);
  check(model.annotations.size() == 1 &&
            model.annotations[0].first == portable.output_vars[0].name &&
            model.annotations[0].second == want,
        "compiled model carries output annotations");
}

void check_structural_rejections() {
  mir::Expr indexed;
  indexed.kind = mir::Expr::Indexed;
  indexed.unsized.leaf = mir::UnsizedLeaf::Real;
  expect_compile_error(write_v2(target_program(indexed)),
                       "Indexed expression arity",
                       "empty Indexed rejected at compile boundary");

  indexed.args.push_back(literal());
  const mir::Program base_only =
      decode_program(write_v2(target_program(indexed)));
  check(base_only.log_prob[0].target.args.size() == 1,
        "base-only Indexed wrapper");

  mir::Expr bad_index;
  bad_index.kind = stanli::mir::Expr::IndexSingle;
  indexed.args.push_back(bad_index);
  expect_error(write_v2(target_program(indexed)), "index arity",
               "IndexSingle without operand");

  bad_index.kind = stanli::mir::Expr::IndexBetween;
  bad_index.args.push_back(literal());
  indexed.args[1] = bad_index;
  expect_error(write_v2(target_program(indexed)), "IndexBetween arity",
               "IndexBetween with one endpoint");

  bad_index.kind = stanli::mir::Expr::IndexUpfrom;
  bad_index.args.clear();
  indexed.args[1] = bad_index;
  expect_error(write_v2(target_program(indexed)), "index arity",
               "IndexUpfrom without lower bound");
  bad_index.args.push_back(literal());
  indexed.args[1] = bad_index;
  const mir::Program upfrom = decode_program(write_v2(target_program(indexed)));
  check(
      upfrom.log_prob[0].target.args[1].kind == stanli::mir::Expr::IndexUpfrom,
      "IndexUpfrom portable shape");

  bad_index.kind = stanli::mir::Expr::IndexAll;
  indexed.args[1] = bad_index;
  expect_error(write_v2(target_program(indexed)), "IndexAll arity",
               "IndexAll with an operand");

  bad_index.kind = mir::Expr::FunApp;
  bad_index.name = "IndexSingle";
  bad_index.args = {literal()};
  indexed.args[1] = bad_index;
  expect_error(write_v2(target_program(indexed)), "index descriptor",
               "a call in index position");

  for (const auto& test : std::vector<std::pair<mir::Expr::Kind, const char*>>{
           {mir::Expr::Promotion, "Promotion expression arity"},
           {mir::Expr::TernaryIf, "TernaryIf expression arity"},
           {mir::Expr::EOr, "EOr expression arity"},
           {mir::Expr::EAnd, "EAnd expression arity"}}) {
    mir::Expr expression;
    expression.kind = test.first;
    expression.unsized.leaf = mir::UnsizedLeaf::Real;
    expect_error(write_v2(target_program(expression)), test.second,
                 test.second);
  }

  mir::Expr bad_call;
  bad_call.kind = mir::Expr::FunApp;
  bad_call.name = "Plus__";
  bad_call.unsized.leaf = mir::UnsizedLeaf::Real;
  bad_call.args.push_back(literal());
  expect_compile_error(write_v2(target_program(bad_call)), "Plus__ call",
                       "known function arity");
  for (const char* name :
       {"pow", "std_normal_qf", "trigamma", "is_nan", "tcrossprod", "diagonal",
        "matrix_exp", "append_array", "map_rect", "algebra_solver"}) {
    bad_call.name = name;
    bad_call.args.clear();
    expect_error(write_v2(target_program(bad_call)),
                 std::string(name) + " call",
                 std::string(name) + " malformed arity");
  }
  bad_call.name = "pow";
  bad_call.fn_lib = mir::Expr::Lib::Internal;
  expect_error(write_v2(target_program(bad_call)), "pow call",
               "internal function cannot bypass name-dispatched arity");

  mir::Expr wiener;
  wiener.kind = mir::Expr::FunApp;
  wiener.name = "wiener_lpdf";
  wiener.unsized.leaf = mir::UnsizedLeaf::Real;
  wiener.args.assign(7, literal());
  const std::string extended_wiener = write_v2(target_program(wiener));
  const mir::Program decoded_wiener = decode_program(extended_wiener);
  check(decoded_wiener.log_prob.size() == 1 &&
            decoded_wiener.log_prob[0].target.args.size() == 7,
        "portable decoder accepts seven-argument wiener_lpdf");
  try {
    (void)compile_model(extended_wiener, DataMap());
  } catch (const std::exception& error) {
    check(false,
          std::string("seven-argument wiener compiles: ") + error.what());
  }
  wiener.args.resize(10);
  expect_error(write_v2(target_program(wiener)), "expected 5 to 9 argument(s)",
               "ten-argument wiener rejected structurally");

  mir::Program bad_binding;
  mir::SizedType real_type;
  real_type.base = "SReal";
  mir::Expr x;
  x.kind = mir::Expr::Var;
  x.name = "x";
  x.unsized.leaf = mir::UnsizedLeaf::Int;
  bad_binding = target_program(x);
  bad_binding.input_vars.emplace_back("x", real_type);
  expect_error(write_v2(bad_binding),
               "variable type disagrees with its binding",
               "variable binding type mismatch");

  mir::Program unsized_assignment;
  mir::Stmt unsized_declaration;
  unsized_declaration.kind = mir::Stmt::Decl;
  unsized_declaration.decl_id = "tmp";
  unsized_declaration.decl_type.unsized = {1, mir::UnsizedLeaf::Real};
  unsized_assignment.log_prob.push_back(std::move(unsized_declaration));
  mir::Stmt first_assignment;
  first_assignment.kind = mir::Stmt::Assignment;
  first_assignment.lhs = "tmp";
  first_assignment.rhs.kind = mir::Expr::Unsupported;
  first_assignment.rhs.unsized = {1, mir::UnsizedLeaf::Real};
  unsized_assignment.log_prob.push_back(std::move(first_assignment));
  const mir::Program restored_unsized =
      decode_program(write_v2(unsized_assignment));
  check(restored_unsized.log_prob[0].decl_type.unsized.depth == 1 &&
            restored_unsized.log_prob[0].decl_type.unsized.leaf ==
                mir::UnsizedLeaf::Real,
        "portable decoder restores unsized declaration metadata");

  mir::Program wrong_unsized_leaf = unsized_assignment;
  wrong_unsized_leaf.log_prob[1].rhs.unsized.leaf = mir::UnsizedLeaf::Int;
  expect_error(write_v2(wrong_unsized_leaf),
               "assignment type disagrees with its binding",
               "unsized first assignment leaf mismatch");
  mir::Program wrong_unsized_rank = unsized_assignment;
  wrong_unsized_rank.log_prob[1].rhs.unsized.depth = 2;
  expect_error(write_v2(wrong_unsized_rank),
               "assignment type disagrees with its binding",
               "unsized first assignment rank mismatch");
  mir::Program wrong_unsized_initializer;
  mir::Stmt initialized_unsized = unsized_assignment.log_prob[0];
  initialized_unsized.has_init = true;
  initialized_unsized.init = wrong_unsized_leaf.log_prob[1].rhs;
  wrong_unsized_initializer.log_prob.push_back(std::move(initialized_unsized));
  expect_error(write_v2(wrong_unsized_initializer),
               "declaration initializer type disagrees with its binding",
               "unsized declaration initializer mismatch");

  mir::Program colliding_functions;
  const auto add_function = [&](const std::string& name,
                                mir::UnsizedLeaf leaf) {
    mir::FunDef definition;
    definition.name = name;
    definition.arg_names.push_back("x");
    definition.arg_views.push_back(mir::UnsizedView{0, leaf});
    definition.arg_data_only.push_back(false);
    colliding_functions.fun_defs.push_back(std::move(definition));
  };
  add_function("f", mir::UnsizedLeaf::Real);
  add_function("f", mir::UnsizedLeaf::Int);
  add_function("f(int)", mir::UnsizedLeaf::Int);
  expect_error(write_v2(colliding_functions),
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
  expect_error(write_v2(bad_body(mir::Stmt::For, 0)),
               "For statement body arity", "empty For body");
  expect_error(write_v2(bad_body(mir::Stmt::For, 2)),
               "For statement body arity", "multiple For bodies");
  expect_error(write_v2(bad_body(mir::Stmt::While, 0)),
               "While statement body arity", "empty While body");
  expect_error(write_v2(bad_body(mir::Stmt::IfElse, 0)),
               "IfElse statement body arity", "empty IfElse body");
  expect_error(write_v2(bad_body(mir::Stmt::IfElse, 3)),
               "IfElse statement body arity", "three IfElse bodies");
  mir::Program bad_transform;
  mir::Stmt declaration;
  declaration.kind = mir::Stmt::Decl;
  declaration.decl_type.base = "SReal";
  declaration.read_transform = mir::Transform{};
  declaration.read_transform->kind = mir::Transform::Lower;
  bad_transform.log_prob.push_back(std::move(declaration));
  expect_error(write_v2(bad_transform), "transform arity",
               "transform argument arity");

  mir::Program bad_sized;
  mir::SizedType matrix_type;
  matrix_type.base = "SMatrix";
  matrix_type.dims.push_back(literal());
  bad_sized.input_vars.emplace_back("M", std::move(matrix_type));
  expect_error(write_v2(bad_sized), "SMatrix sized type dimensions",
               "sized matrix dimensions");
  mir::Program incomplete_function;
  mir::FunDef incomplete_definition;
  incomplete_definition.name = "bad";
  incomplete_definition.arg_names.push_back("x");
  incomplete_function.fun_defs.push_back(std::move(incomplete_definition));
  expect_error(write_v2(incomplete_function),
               "function argument field lengths disagree",
               "parallel function argument arrays");
}

void check_v2_rejections() {
  expect_error("STANLI6:", "is from a different stanli compiler",
               "unknown portable version");
  expect_error("STANLI4:AAAAAA==", "is from a different stanli compiler",
               "retired portable v4 envelope");
  expect_error("STANLI3:AAAAAA==", "is from a different stanli compiler",
               "retired portable v3 envelope");
  expect_error("STANLI2:AAAAAA==", "is from a different stanli compiler",
               "retired portable v2 envelope");
  expect_error("{\"stanli_ir\":1}", "not portable MIR",
               "retired portable v1 envelope");
  expect_error(" STANLI5:", "not portable MIR", "v2 leading whitespace");
  expect_error("[]", "not portable MIR", "unknown input format");
  // Stock stanc's s-expression MIR, which an earlier reader accepted.
  expect_error("((functions_block ()) (input_vars ()) (log_prob ()))",
               "Stock stanc's s-expression MIR (--debug-optimized-mir) is no "
               "longer read",
               "stock stanc s-expression");
  expect_error(" \n\t", "empty input", "empty input");
  const std::string empty_wire = v2_wire(empty_v2_payload());
  const mir::Program empty = decode_program(empty_wire);
  check(empty.input_vars.empty() && empty.prepare_data.empty() &&
            empty.log_prob.empty() && empty.generate_quantities.empty() &&
            empty.fun_defs.empty() && empty.output_vars.empty(),
        "v2 empty program");
  check(empty.has_transform_inits && empty.transform_inits.empty(),
        "v3 empty transform_inits section");
  check(empty_wire.find('\0') == std::string::npos,
        "v2 envelope is safe for C-string transports");

  mir::Program loop_control;
  mir::Stmt break_statement;
  break_statement.kind = mir::Stmt::Break;
  mir::Stmt continue_statement;
  continue_statement.kind = mir::Stmt::Continue;
  loop_control.log_prob = {break_statement, continue_statement};
  const mir::Program decoded_loop_control =
      decode_program(write_v2(loop_control));
  check(decoded_loop_control.log_prob.size() == 2 &&
            decoded_loop_control.log_prob[0].kind == mir::Stmt::Break &&
            decoded_loop_control.log_prob[1].kind == mir::Stmt::Continue,
        "v2 preserves break and continue statement tags");

  // transform_inits round trip: the section survives, and the transform on
  // its FnWriteParam comes back as write_transform rather than as the
  // check_transform it shares a wire slot with. A write_array FnWriteParam
  // carries neither.
  mir::Program inits;
  mir::Stmt free_write;
  free_write.kind = mir::Stmt::NRFunApp;
  free_write.fn_name = "FnWriteParam";
  free_write.fn_lib = mir::Expr::Lib::Internal;
  mir::Transform lower;
  lower.kind = mir::Transform::Lower;
  mir::Expr bound;
  bound.kind = mir::Expr::LitInt;
  bound.lit_i = 0;
  lower.args.push_back(bound);
  free_write.write_transform = lower;
  inits.transform_inits.push_back(free_write);
  mir::Stmt constrained_write;
  constrained_write.kind = mir::Stmt::NRFunApp;
  constrained_write.fn_name = "FnWriteParam";
  constrained_write.fn_lib = mir::Expr::Lib::Internal;
  inits.generate_quantities.push_back(constrained_write);
  const mir::Program decoded_inits = decode_program(write_v2(inits));
  check(
      decoded_inits.has_transform_inits &&
          decoded_inits.transform_inits.size() == 1 &&
          decoded_inits.transform_inits[0].write_transform.has_value() &&
          decoded_inits.transform_inits[0].write_transform->kind ==
              mir::Transform::Lower &&
          decoded_inits.transform_inits[0].write_transform->args.size() == 1 &&
          !decoded_inits.transform_inits[0].check_transform.has_value(),
      "v2 carries the transform to invert on transform_inits FnWriteParam");
  check(decoded_inits.generate_quantities.size() == 1 &&
            !decoded_inits.generate_quantities[0].write_transform.has_value() &&
            !decoded_inits.generate_quantities[0].check_transform.has_value(),
        "v2 leaves write_array FnWriteParam without a transform");

  const auto check_real_bits = [](uint64_t expected, const std::string& what) {
    const mir::Program decoded =
        decode_program(v2_wire(real_target_v2_payload(expected)));
    uint64_t actual = 0;
    std::memcpy(&actual, &decoded.log_prob.at(0).target.lit, sizeof(actual));
    check(actual == expected, what);
  };
  check_real_bits(UINT64_C(0x8000000000000000), "v2 preserves negative zero");
  check_real_bits(UINT64_C(0x7ff8000000000042),
                  "v2 preserves binary64 NaN payload bits");

  expect_error("STANLI5:AAA", "multiple of 4", "v2 incomplete base64 quartet");
  expect_error("STANLI5:!!!!", "invalid base64 character",
               "v2 invalid base64 character");
  expect_error("STANLI5:AA==AAAA", "padding before final quartet",
               "v2 early base64 padding");
  expect_error("STANLI5:AB==", "non-canonical base64 tail",
               "v2 non-canonical one-byte tail");
  expect_error("STANLI5:AAB=", "non-canonical base64 tail",
               "v2 non-canonical two-byte tail");

  std::string truncated = empty_v2_payload();
  truncated.pop_back();
  expect_error(v2_wire(truncated), "truncated input", "v2 truncated payload");
  std::string trailing = empty_v2_payload();
  append_u8(trailing, 0);
  expect_error(v2_wire(trailing), "trailing bytes", "v2 trailing payload");
  std::string no_transform_inits = empty_v2_payload();
  no_transform_inits.resize(no_transform_inits.size() - 8);
  expect_error(v2_wire(no_transform_inits), "truncated input",
               "missing transform_inits section");

  std::string oversized_list;
  append_u32(oversized_list, 1000001);
  expect_error(v2_wire(oversized_list), "list exceeds 1000000 items",
               "v2 per-list limit");
  std::string impossible_list;
  append_u32(impossible_list, 100);
  expect_error(v2_wire(impossible_list), "list count exceeds remaining input",
               "v2 impossible list count");
  std::string oversized_list_storage;
  append_u32(oversized_list_storage, 0);  // input_vars
  const uint32_t statement_count =
      static_cast<uint32_t>(268435456 / sizeof(mir::Stmt) + 1);
  append_u32(oversized_list_storage, statement_count);  // prepare_data
  oversized_list_storage.append(statement_count, '\0');
  expect_error(v2_wire(oversized_list_storage),
               "decoded list storage exceeds 268435456 bytes",
               "v2 aggregate list storage limit");
  std::string oversized_string;
  append_u32(oversized_string, 1);         // one input
  append_u32(oversized_string, 16777217);  // its name
  expect_error(v2_wire(oversized_string), "string exceeds 16777216 bytes",
               "v2 per-string limit");
  const auto invalid_utf8_name = [](std::initializer_list<uint8_t> bytes) {
    std::string payload;
    append_u32(payload, 1);  // one input variable
    append_u32(payload, static_cast<uint32_t>(bytes.size()));
    for (uint8_t byte : bytes) append_u8(payload, byte);
    return v2_wire(payload);
  };
  expect_error(invalid_utf8_name({0xff}), "invalid UTF-8 string",
               "v2 invalid UTF-8 leading byte");
  expect_error(invalid_utf8_name({0xc0, 0x80}), "invalid UTF-8 string",
               "v2 overlong UTF-8");
  expect_error(invalid_utf8_name({0xe0, 0x80, 0x80}), "invalid UTF-8 string",
               "v2 overlong three-byte UTF-8");
  expect_error(invalid_utf8_name({0xed, 0xa0, 0x80}), "invalid UTF-8 string",
               "v2 UTF-8 surrogate");
  expect_error(invalid_utf8_name({0xf4, 0x90, 0x80, 0x80}),
               "invalid UTF-8 string", "v2 UTF-8 beyond scalar range");
  expect_error(invalid_utf8_name({0xf0, 0x9f}), "invalid UTF-8 string",
               "v2 truncated UTF-8");

  std::string unknown_stmt;
  append_u32(unknown_stmt, 0);
  append_u32(unknown_stmt, 0);
  append_u32(unknown_stmt, 1);
  append_u8(unknown_stmt, 0xff);
  expect_error(v2_wire(unknown_stmt), "unknown statement tag",
               "v2 unknown statement tag");

  std::string unknown_expr;
  append_u32(unknown_expr, 0);
  append_u32(unknown_expr, 0);
  append_u32(unknown_expr, 1);
  append_u8(unknown_expr, 2);  // TargetPE
  append_u8(unknown_expr, 0xff);
  expect_error(v2_wire(unknown_expr), "unknown expression tag",
               "v2 unknown expression tag");

  std::string unknown_library;
  append_u32(unknown_library, 0);
  append_u32(unknown_library, 0);
  append_u32(unknown_library, 1);
  append_u8(unknown_library, 2);  // TargetPE
  append_u8(unknown_library, 4);  // FunApp
  append_u8(unknown_library, 0xff);
  expect_error(v2_wire(unknown_library), "unknown function-library tag",
               "v2 unknown function-library tag");

  const auto declaration_through_transform = [](uint8_t present,
                                                uint8_t transform_tag) {
    std::string payload;
    append_u32(payload, 0);
    append_u32(payload, 0);
    append_u32(payload, 1);
    append_u8(payload, 0);  // Decl
    append_string(payload, "x");
    append_u8(payload, 0);  // sized
    append_string(payload, "SReal");
    append_u32(payload, 0);  // dimensions
    append_string(payload, "");
    append_string(payload, "");
    append_bool(payload, true);
    append_bool(payload, false);
    append_u8(payload, present);
    if (present == 1) append_u8(payload, transform_tag);
    return payload;
  };
  expect_error(v2_wire(declaration_through_transform(2, 0)),
               "optional marker is not 0 or 1", "v2 invalid optional marker");
  expect_error(v2_wire(declaration_through_transform(1, 0xff)),
               "unknown transform tag", "v2 unknown transform tag");
  std::string unknown_declaration_type;
  append_u32(unknown_declaration_type, 0);
  append_u32(unknown_declaration_type, 0);
  append_u32(unknown_declaration_type, 1);
  append_u8(unknown_declaration_type, 0);  // Decl
  append_string(unknown_declaration_type, "x");
  append_u8(unknown_declaration_type, 2);
  expect_error(v2_wire(unknown_declaration_type),
               "unknown declaration-type tag", "unknown declaration-type tag");
  std::string check_without_payload;
  append_u32(check_without_payload, 0);
  append_u32(check_without_payload, 0);
  append_u32(check_without_payload, 1);
  append_u8(check_without_payload, 8);  // NRFunApp
  append_u8(check_without_payload, 1);  // Internal
  append_string(check_without_payload, "FnCheck");
  append_u8(check_without_payload, 0);   // FnPlain
  append_u32(check_without_payload, 0);  // arguments
  append_u8(check_without_payload, 0);   // no payload
  expect_error(v2_wire(check_without_payload),
               "statement payload does not match its function",
               "FnCheck without its payload");

  expect_error(v2_wire(real_target_v2_payload(0, 2)), "boolean is not 0 or 1",
               "v2 invalid boolean");
  expect_error(v2_wire(real_target_v2_payload(0, 1, 0xff)),
               "unknown unsized-view tag", "v2 unknown view tag");

  const auto nested_program = [](int block_count) {
    std::string nested = real_target_v2_payload(0);
    // Strip the root framing, retain the one TargetPE statement, and wrap it
    // in one-child blocks.
    std::string statement = nested.substr(12, nested.size() - 24);
    for (int i = 0; i < block_count; ++i) {
      std::string block;
      append_u8(block, 3);  // Block
      append_u32(block, 1);
      block += statement;
      append_string(block, "");
      statement = std::move(block);
    }
    std::string program;
    append_u32(program, 0);
    append_u32(program, 0);
    append_u32(program, 1);
    program += statement;
    append_u32(program, 0);
    append_u32(program, 0);
    append_u32(program, 0);
    return program;
  };
  const mir::Program legal_depth = decode_program(v2_wire(nested_program(200)));
  check(legal_depth.log_prob.size() == 1 &&
            legal_depth.log_prob.front().kind == mir::Stmt::Block,
        "v2 accepts a nested program on the calling thread");
  const mir::Program deep = decode_program(v2_wire(nested_program(5000)));
  check(deep.log_prob.size() == 1 && deep.nesting > 5000,
        "v2 decodes a program nested past the calling thread's limit");
  expect_error(v2_wire(nested_program(100000)), "nested too deeply",
               "v2 nesting limit");
}

}  // namespace

int main() {
  check_round_trip("tests/fixtures/es.mir");
  check_round_trip("tests/fixtures/tdvocab.mir");
  check_round_trip("tests/fixtures/wanames.mir");
  check_round_trip("tests/fixtures/odefns.mir");
  check_round_trip("tests/fixtures/view_udf_local_data_branch.mir");
  check_round_trip("tests/fixtures/paramcond_break.mir");
  check_round_trip("tests/fixtures/shape_named_guard.mir");
  check_round_trip("tests/fixtures/shape_indexed_guard.mir");
  check_round_trip("tests/fixtures/shape_guard_lazy.mir");
  check_round_trip("tests/fixtures/shape_partial_guard.mir");
  check_round_trip_lowering();
  check_full_span_assignment_lowering();
  check_overload_finalization();
  check_multi_results();
  check_decompositions();
  check_tuple_interface();
  check_output_cursor_leaves();
  check_exact_float_bits();
  check_nullary_math_constants();
  check_output_annotations();
  check_structural_rejections();
  check_v2_rejections();

  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::puts("ok");
  return 0;
}
