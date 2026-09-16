// Compatibility and rejection tests for the portable-MIR decoder. The JSON
// objects in this file are test-only deep-comparison values; the wire encoder
// below mirrors the production OCaml layout for malformed-input coverage.
#include <stanli/compile.hpp>
#include <stanli/mir_decode.hpp>
#include <stanli/wa_interp.hpp>

#include "../runtime/third_party/nlohmann_json.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
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

void append_u8(std::string& bytes, uint8_t value) {
  bytes.push_back(static_cast<char>(value));
}

void append_u32(std::string& bytes, uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8)
    append_u8(bytes, static_cast<uint8_t>(value >> shift));
}

void append_u64(std::string& bytes, uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8)
    append_u8(bytes, static_cast<uint8_t>(value >> shift));
}

void append_string(std::string& bytes, std::string_view value) {
  append_u32(bytes, static_cast<uint32_t>(value.size()));
  bytes.append(value);
}

std::string encode_base64(std::string_view bytes) {
  constexpr std::string_view alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string result;
  result.reserve(((bytes.size() + 2) / 3) * 4);
  for (size_t offset = 0; offset < bytes.size(); offset += 3) {
    const uint32_t a = static_cast<unsigned char>(bytes[offset]);
    const bool have_b = offset + 1 < bytes.size();
    const bool have_c = offset + 2 < bytes.size();
    const uint32_t b =
        have_b ? static_cast<unsigned char>(bytes[offset + 1]) : 0;
    const uint32_t c =
        have_c ? static_cast<unsigned char>(bytes[offset + 2]) : 0;
    result.push_back(alphabet[a >> 2]);
    result.push_back(alphabet[((a & 0x03) << 4) | (b >> 4)]);
    result.push_back(have_b ? alphabet[((b & 0x0f) << 2) | (c >> 6)] : '=');
    result.push_back(have_c ? alphabet[c & 0x3f] : '=');
  }
  return result;
}

std::string v2_wire(std::string_view payload) {
  return "STANLI2:" + encode_base64(payload);
}

void append_bool(std::string& bytes, bool value) {
  append_u8(bytes, value ? 1 : 0);
}

template <typename T, typename Write>
void append_list(std::string& bytes, const std::vector<T>& values,
                 Write write) {
  append_u32(bytes, static_cast<uint32_t>(values.size()));
  for (const T& value : values) write(bytes, value);
}

void append_view(std::string& bytes, const mir::UnsizedView& value) {
  append_u8(bytes, value.depth);
  append_u8(bytes, static_cast<uint8_t>(value.leaf));
}

void append_expr(std::string& bytes, const mir::Expr& value);
void append_stmt(std::string& bytes, const mir::Stmt& value);

void append_expr_list(std::string& bytes,
                      const std::vector<mir::Expr>& values) {
  append_list(bytes, values, [](std::string& out, const mir::Expr& expression) {
    append_expr(out, expression);
  });
}

void append_expr(std::string& bytes, const mir::Expr& value) {
  append_u8(bytes, static_cast<uint8_t>(value.kind));
  switch (value.kind) {
    case mir::Expr::Var:
      append_string(bytes, value.name);
      break;
    case mir::Expr::LitInt:
      append_u32(bytes,
                 static_cast<uint32_t>(static_cast<int32_t>(value.lit_i)));
      break;
    case mir::Expr::LitReal: {
      uint64_t bits = 0;
      std::memcpy(&bits, &value.lit, sizeof(bits));
      append_u64(bytes, bits);
      break;
    }
    case mir::Expr::LitStr:
      append_string(bytes, value.lit_s);
      break;
    case mir::Expr::FunApp:
      append_u8(bytes, static_cast<uint8_t>(value.fn_lib));
      append_string(bytes, value.name);
      append_bool(bytes, value.fn_propto);
      append_expr_list(bytes, value.args);
      break;
    case mir::Expr::Promotion:
    case mir::Expr::Indexed:
    case mir::Expr::TernaryIf:
    case mir::Expr::EOr:
    case mir::Expr::EAnd:
      append_expr_list(bytes, value.args);
      break;
    case mir::Expr::Unsupported:
      break;
  }
  append_string(bytes, value.type_);
  append_view(bytes, value.unsized);
  append_bool(bytes, value.data_only);
  append_bool(bytes, value.promoted);
  append_string(bytes, value.raw);
}

void append_transform(std::string& bytes, const mir::Transform& value) {
  append_u8(bytes, static_cast<uint8_t>(value.kind));
  append_expr_list(bytes, value.args);
  append_string(bytes, value.raw);
}

void append_optional_transform(std::string& bytes,
                               const std::optional<mir::Transform>& value) {
  append_bool(bytes, value.has_value());
  if (value) append_transform(bytes, *value);
}

void append_sized(std::string& bytes, const mir::SizedType& value) {
  append_string(bytes, value.base);
  append_expr_list(bytes, value.dims);
  append_string(bytes, value.elem_base);
  append_string(bytes, value.raw);
}

void append_stmt_list(std::string& bytes,
                      const std::vector<mir::Stmt>& values) {
  append_list(bytes, values, [](std::string& out, const mir::Stmt& statement) {
    append_stmt(out, statement);
  });
}

void append_stmt(std::string& bytes, const mir::Stmt& value) {
  append_u8(bytes, static_cast<uint8_t>(value.kind));
  switch (value.kind) {
    case mir::Stmt::Decl:
      append_string(bytes, value.decl_id);
      append_sized(bytes, value.decl_type);
      append_bool(bytes, value.decl_data_only);
      append_bool(bytes, value.has_init);
      if (value.has_init) append_expr(bytes, value.init);
      append_optional_transform(bytes, value.read_transform);
      append_expr_list(bytes, value.read_dims);
      append_string(bytes, value.raw);
      break;
    case mir::Stmt::Assignment:
      append_string(bytes, value.lhs);
      append_expr_list(bytes, value.lhs_idx);
      append_expr(bytes, value.rhs);
      append_string(bytes, value.raw);
      break;
    case mir::Stmt::TargetPE:
      append_expr(bytes, value.target);
      append_string(bytes, value.raw);
      break;
    case mir::Stmt::Block:
    case mir::Stmt::SList:
      append_stmt_list(bytes, value.body);
      append_string(bytes, value.raw);
      break;
    case mir::Stmt::For:
      append_string(bytes, value.loopvar);
      append_expr(bytes, value.lower);
      append_expr(bytes, value.upper);
      append_stmt_list(bytes, value.body);
      append_string(bytes, value.raw);
      break;
    case mir::Stmt::IfElse:
    case mir::Stmt::While:
      append_expr(bytes, value.cond);
      append_stmt_list(bytes, value.body);
      append_string(bytes, value.raw);
      break;
    case mir::Stmt::NRFunApp:
      append_string(bytes, value.fn_name);
      append_expr_list(bytes, value.fn_args);
      append_optional_transform(bytes, value.fn_name == "FnWriteParam"
                                           ? value.write_transform
                                           : value.check_transform);
      append_string(bytes, value.check_var_name);
      append_string(bytes, value.raw);
      break;
    case mir::Stmt::Return:
      append_bool(bytes, value.has_init);
      if (value.has_init) append_expr(bytes, value.rhs);
      append_string(bytes, value.raw);
      break;
    case mir::Stmt::Break:
    case mir::Stmt::Continue:
      break;
    case mir::Stmt::Skip:
    case mir::Stmt::Unsupported:
      append_string(bytes, value.raw);
      break;
  }
}

void append_strings(std::string& bytes,
                    const std::vector<std::string>& values) {
  append_list(bytes, values, [](std::string& out, const std::string& value) {
    append_string(out, value);
  });
}

void append_fun(std::string& bytes, const mir::FunDef& value) {
  append_string(bytes, value.name);
  append_strings(bytes, value.arg_names);
  append_strings(bytes, value.arg_types);
  append_list(bytes, value.arg_views,
              [](std::string& out, const mir::UnsizedView& view) {
                append_view(out, view);
              });
  append_u32(bytes, static_cast<uint32_t>(value.arg_data_only.size()));
  for (bool data_only : value.arg_data_only) append_bool(bytes, data_only);
  append_stmt_list(bytes, value.body);
}

std::string write_v2(const mir::Program& value) {
  std::string payload;
  append_u32(payload, static_cast<uint32_t>(value.input_vars.size()));
  for (const auto& input : value.input_vars) {
    append_string(payload, input.first);
    append_sized(payload, input.second);
  }
  append_stmt_list(payload, value.prepare_data);
  append_stmt_list(payload, value.log_prob);
  append_stmt_list(payload, value.generate_quantities);
  append_list(payload, value.fun_defs,
              [](std::string& out, const mir::FunDef& definition) {
                append_fun(out, definition);
              });
  append_strings(payload, value.output_vars);
  append_stmt_list(payload, value.transform_inits);
  return v2_wire(payload);
}

std::string empty_v2_payload() {
  std::string payload;
  for (int i = 0; i < 6; ++i) append_u32(payload, 0);
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
  append_string(payload, "UReal");
  append_u8(payload, 0);  // unsized array depth
  append_u8(payload, leaf);
  append_u8(payload, data_only);
  append_u8(payload, 0);  // promoted
  append_string(payload, "");
  append_string(payload, "");  // statement raw
  append_u32(payload, 0);      // generate_quantities
  append_u32(payload, 0);      // fun_defs
  append_u32(payload, 0);      // output_vars
  return payload;
}

size_t replace_assignment_with_all(std::string& text, const std::string& lhs,
                                   const std::string& type) {
  const std::string from = "(Assignment ((LVariable " + lhs + ") ()) " + type;
  const std::string to = "(Assignment ((LVariable " + lhs + ") (All)) " + type;
  size_t count = 0;
  for (size_t at = 0; (at = text.find(from, at)) != std::string::npos;) {
    text.replace(at, from.size(), to);
    at += to.size();
    ++count;
  }
  return count;
}

size_t count_full_span_assignments(const std::vector<mir::Stmt>& body,
                                   const std::string& lhs) {
  size_t count = 0;
  for (const mir::Stmt& statement : body) {
    count += statement.kind == mir::Stmt::Assignment && statement.lhs == lhs &&
             statement.lhs_idx.size() == 1 &&
             statement.lhs_idx[0].name == "IndexAll";
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
          {"transform_inits", write_array(value.transform_inits, write_stmt)},
          {"fun_defs", write_array(value.fun_defs, write_fun_def)},
          {"output_vars", write_strings(value.output_vars)}};
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
       {&value.prepare_data, &value.log_prob, &value.generate_quantities,
        &value.transform_inits})
    for (mir::Stmt& statement : *body) clear_raw(statement);
  for (mir::FunDef& function : value.fun_defs)
    for (mir::Stmt& statement : function.body) clear_raw(statement);
}

bool report_first_difference(const json& actual, const json& expected,
                             const std::string& path) {
  if (actual.type() != expected.type()) {
    std::printf("DIFF %s type actual=%s expected=%s\n", path.c_str(),
                actual.type_name(), expected.type_name());
    return true;
  }
  if (actual.is_object()) {
    if (actual.size() != expected.size()) {
      std::printf("DIFF %s object size actual=%zu expected=%zu\n", path.c_str(),
                  actual.size(), expected.size());
      return true;
    }
    for (auto it = expected.begin(); it != expected.end(); ++it) {
      if (!actual.contains(it.key())) {
        std::printf("DIFF %s missing key %s\n", path.c_str(), it.key().c_str());
        return true;
      }
      if (report_first_difference(actual.at(it.key()), it.value(),
                                  path + "." + it.key()))
        return true;
    }
    return false;
  }
  if (actual.is_array()) {
    if (actual.size() != expected.size()) {
      std::printf("DIFF %s array size actual=%zu expected=%zu\n", path.c_str(),
                  actual.size(), expected.size());
      if (actual.size() <= 8 && expected.size() <= 8)
        std::printf("  actual=%s\n  expected=%s\n", actual.dump().c_str(),
                    expected.dump().c_str());
      return true;
    }
    for (size_t i = 0; i < actual.size(); ++i)
      if (report_first_difference(actual[i], expected[i],
                                  path + "[" + std::to_string(i) + "]"))
        return true;
    return false;
  }
  if (actual != expected) {
    std::printf("DIFF %s actual=%s expected=%s\n", path.c_str(),
                actual.dump().c_str(), expected.dump().c_str());
    return true;
  }
  return false;
}

void check_program_equivalence(const std::string& legacy_path,
                               const std::string& portable_path) {
  mir::Program legacy = read_fixture(legacy_path);
  mir::Program portable = read_fixture(portable_path);
  // Portable MIR keeps complete opaque diagnostic payloads; the legacy reader
  // truncates some of them. They do not participate in execution, so compare
  // every structural/semantic field after removing only those payloads.
  clear_raw(legacy);
  clear_raw(portable);
  const json portable_object = write_program_object(portable);
  const json legacy_object = write_program_object(legacy);
  const bool equal = portable_object == legacy_object;
  if (!equal)
    report_first_difference(portable_object, legacy_object, "$program");
  check(equal, portable_path + " decoded fields match " + legacy_path);
}

void check_round_trip(const std::string& path) {
  const mir::Program legacy = read_fixture(path);
  const mir::Program portable = decode_program(write_v2(legacy));
  check(write_program_object(portable) == write_program_object(legacy),
        path + " portable fields match legacy fields");
}

void check_lowering_equivalence(const char* legacy_fixture,
                                const char* portable_fixture) {
  const std::string legacy_text =
      slurp(legacy_fixture ? legacy_fixture : "tests/fixtures/es.tmir.sexp");
  const mir::Program legacy_program = decode_program(legacy_text);
  const std::string portable_text =
      portable_fixture ? slurp(portable_fixture) : write_v2(legacy_program);
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

void check_full_span_assignment_lowering() {
  const auto decoded_pair = [](const std::string& legacy_text,
                               const std::string& what) {
    const mir::Program legacy_program = decode_program(legacy_text);
    const std::string portable_text = write_v2(legacy_program);
    const mir::Program portable_program = decode_program(portable_text);
    check(write_program_object(portable_program) ==
              write_program_object(legacy_program),
          what + " portable decode matches legacy decode");
    return std::make_pair(portable_text, legacy_program);
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
    std::string legacy =
        slurp("tests/fixtures/view_array_container_extract.tmir.sexp");
    check(replace_assignment_with_all(legacy, "vs", "(UArray UVector)") == 2,
          "legacy array-vector assignments rewritten for fixture");
    check(replace_assignment_with_all(legacy, "rs", "(UArray URowVector)") == 2,
          "legacy array-row assignments rewritten for fixture");
    const auto decoded =
        decoded_pair(legacy, "full-span array-container fixture");
    check(count_full_span_assignments(decoded.second.log_prob, "vs") == 1 &&
              count_full_span_assignments(decoded.second.log_prob, "rs") == 1,
          "legacy decoder preserves array-container IndexAll LHS");
    CompiledModel legacy_model = compile_model(legacy, DataMap{});
    CompiledModel portable_model = compile_model(decoded.first, DataMap{});
    const auto legacy_result =
        run_log(legacy_model, {0.25}, "legacy array-container full-span");
    const auto portable_result =
        run_log(portable_model, {0.25}, "portable array-container full-span");
    check(legacy_result == portable_result,
          "array-container full-span log_prob format parity");
    check(legacy_result.first == 1630.25 &&
              legacy_result.second == std::vector<double>({1.0}),
          "array-container full-span log_prob value");
  }

  // Zero storage does not erase the declared array/vector geometry. A
  // complete assignment of array[2] vector[0] therefore succeeds only when
  // the logical views, rather than flat widths alone, agree.
  {
    std::string legacy =
        slurp("tests/fixtures/viewa_explicit_zero_vectors.tmir.sexp");
    check(
        replace_assignment_with_all(legacy, "values", "(UArray UVector)") == 2,
        "legacy zero-vector-array assignments rewritten for fixture");
    const auto decoded =
        decoded_pair(legacy, "full-span zero-vector-array fixture");
    CompiledModel legacy_model = compile_model(legacy, DataMap{});
    CompiledModel portable_model = compile_model(decoded.first, DataMap{});
    const auto legacy_result =
        run_log(legacy_model, {0.25}, "legacy zero-vector-array full-span");
    const auto portable_result =
        run_log(portable_model, {0.25}, "portable zero-vector-array full-span");
    check(legacy_result == portable_result && legacy_result.first == 200.25 &&
              legacy_result.second == std::vector<double>({1.0}),
          "zero-vector-array full-span value and format parity");
  }

  // The row-vector form uses the same storage width as a vector but a
  // distinct logical view, so it is a focused orientation check.
  {
    std::string legacy = slurp("tests/fixtures/rep_row_vector.tmir.sexp");
    check(replace_assignment_with_all(legacy, "x", "URowVector") == 2,
          "legacy row-vector assignments rewritten for fixture");
    const auto decoded = decoded_pair(legacy, "full-span row-vector fixture");
    check(count_full_span_assignments(decoded.second.log_prob, "x") == 1,
          "legacy decoder preserves row-vector IndexAll LHS");
    CompiledModel legacy_model = compile_model(legacy, DataMap{});
    CompiledModel portable_model = compile_model(decoded.first, DataMap{});
    const auto legacy_result =
        run_log(legacy_model, {0.25}, "legacy row-vector full-span");
    const auto portable_result =
        run_log(portable_model, {0.25}, "portable row-vector full-span");
    check(legacy_result == portable_result,
          "row-vector full-span log_prob format parity");

    mir::Program mismatch = decoded.second;
    std::function<bool(std::vector<mir::Stmt>&)> shorten =
        [&](std::vector<mir::Stmt>& body) {
          for (mir::Stmt& statement : body) {
            if (statement.kind == mir::Stmt::Assignment &&
                statement.lhs == "x" && statement.lhs_idx.size() == 1 &&
                statement.lhs_idx[0].name == "IndexAll" &&
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
                statement.lhs_idx[0].name == "IndexAll") {
              statement.rhs.name = "rep_vector";
              statement.rhs.type_ = "UVector";
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
    std::string legacy = slurp("tests/fixtures/rangeclamp.tmir.sexp");
    check(replace_assignment_with_all(legacy, "xs", "(UArray UReal)") == 1,
          "legacy scalar-array assignment rewritten for fixture");
    const auto decoded = decoded_pair(legacy, "full-span scalar-array fixture");
    check(count_full_span_assignments(decoded.second.prepare_data, "xs") == 1,
          "legacy decoder preserves scalar-array IndexAll LHS");
    DataMap data;
    data.set_int("K", 3);
    data.set_int("lo", 1);
    data.set_int("hi", 4);
    data.set_real_array("Y", {0, 0, 0, 0}, {4});
    data.set_real_array("Zm", std::vector<double>(8, 0.0), {4, 2});
    CompiledModel legacy_model = compile_model(legacy, data);
    CompiledModel portable_model = compile_model(decoded.first, data);
    const auto legacy_result =
        run_log(legacy_model, {0.125}, "legacy scalar-array full-span");
    const auto portable_result =
        run_log(portable_model, {0.125}, "portable scalar-array full-span");
    check(legacy_result == portable_result,
          "scalar-array full-span specialization format parity");
  }

  // An integer array initialized through a complete assignment is consumed
  // later as integer data. Compiling the downstream ldexp expressions proves
  // the assignment retained both initialized-prefix and observed-value
  // specialization instead of leaving a typeless real slot behind.
  {
    std::string legacy = slurp("tests/fixtures/binint.tmir.sexp");
    check(replace_assignment_with_all(legacy, "expo",
                                      "(UArray (UArray UInt))") == 1,
          "legacy int-array assignment rewritten for fixture");
    const auto decoded = decoded_pair(legacy, "full-span int-array fixture");
    const DataMap data = DataMap::from_json_file("tests/fixtures/binint.json");
    const CompiledModel legacy_model = compile_model(legacy, data);
    const CompiledModel portable_model = compile_model(decoded.first, data);
    check(legacy_model.n_unconstrained == portable_model.n_unconstrained &&
              legacy_model.fills == portable_model.fills &&
              legacy_model.graph.ops.size() == portable_model.graph.ops.size(),
          "int-array full-span specialization format parity");
  }

  // Generated quantities execute a vector full-span assignment whose RHS
  // uses a draw-dependent array index. Run both formats with identical RNG
  // streams and compare every emitted column and the stream position.
  {
    std::string legacy = slurp("tests/fixtures/gq_runtime_control.tmir.sexp");
    check(replace_assignment_with_all(legacy, "chosen", "UVector") == 1,
          "legacy generated vector assignment rewritten for fixture");
    const auto decoded =
        decoded_pair(legacy, "full-span generated-vector fixture");
    check(count_full_span_assignments(decoded.second.generate_quantities,
                                      "chosen") == 1,
          "legacy decoder preserves generated-vector IndexAll LHS");
    CompiledModel legacy_model = compile_model(legacy, DataMap{});
    CompiledModel portable_model = compile_model(decoded.first, DataMap{});
    const std::vector<double> params = {
        0.2,           -0.3,          0.1,  -0.2, 1.0,  2.0,  3.0,  4.0,
        std::log(1.1), std::log(1.7), 0.25, -0.5, 0.75, -1.0, 1.25, -1.5};
    const auto legacy_log =
        run_log(legacy_model, params, "legacy generated-vector full-span");
    const auto portable_log =
        run_log(portable_model, params, "portable generated-vector full-span");
    check(legacy_log == portable_log,
          "generated-vector full-span log_prob format parity");

    check(legacy_model.write_array && portable_model.write_array,
          "full-span generated-vector write_array exists");
    if (legacy_model.write_array && portable_model.write_array) {
      check(!legacy_model.write_array->interp &&
                legacy_model.write_array->truncated.empty() &&
                !portable_model.write_array->interp &&
                portable_model.write_array->truncated.empty(),
            "full-span generated-vector write_array lowers completely");
      check(CompiledModel::csv_names(legacy_model.write_array->columns) ==
                CompiledModel::csv_names(portable_model.write_array->columns),
            "full-span generated-vector output columns");
      Executor legacy_write(std::move(legacy_model.write_array->graph));
      Executor portable_write(std::move(portable_model.write_array->graph));
      legacy_model.write_array->bind(legacy_write);
      portable_model.write_array->bind(portable_write);
      check(
          legacy_write.n_params() == static_cast<int64_t>(params.size()) &&
              portable_write.n_params() == static_cast<int64_t>(params.size()),
          "full-span generated-vector write_array parameter width");
      std::copy(params.begin(), params.end(), legacy_write.params_data());
      std::copy(params.begin(), params.end(), portable_write.params_data());
      WaRng legacy_rng(81), portable_rng(81);
      legacy_write.run_forward_only(EvalState{&legacy_rng});
      portable_write.run_forward_only(EvalState{&portable_rng});
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
      const std::vector<double> legacy_row =
          collect(*legacy_model.write_array, legacy_write);
      const std::vector<double> portable_row =
          collect(*portable_model.write_array, portable_write);
      const bool same_row =
          legacy_row.size() == portable_row.size() &&
          std::memcmp(legacy_row.data(), portable_row.data(),
                      legacy_row.size() * sizeof(double)) == 0;
      check(same_row,
            "full-span generated-vector write_array value format parity");
      check(legacy_rng.gen()() == portable_rng.gen()(),
            "full-span generated-vector RNG stream parity");
    }
  }
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
  const mir::Program decoded = decode_program(write_v2(unresolved));
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
    const mir::Program decoded = decode_program(write_v2(program));
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
  expect_compile_error(write_v2(target_program(indexed)),
                       "Indexed expression arity",
                       "empty Indexed rejected at compile boundary");

  indexed.args.push_back(literal());
  const mir::Program base_only =
      decode_program(write_v2(target_program(indexed)));
  check(base_only.log_prob[0].target.args.size() == 1,
        "base-only Indexed wrapper");

  mir::Expr bad_index;
  bad_index.kind = mir::Expr::FunApp;
  bad_index.name = "IndexSingle";
  indexed.args.push_back(bad_index);
  expect_error(write_v2(target_program(indexed)), "IndexSingle call",
               "IndexSingle without operand");

  bad_index.name = "IndexBetween";
  bad_index.args.push_back(literal());
  indexed.args[1] = bad_index;
  expect_error(write_v2(target_program(indexed)), "IndexBetween call",
               "IndexBetween with one endpoint");

  bad_index.name = "IndexUpfrom";
  bad_index.args.clear();
  indexed.args[1] = bad_index;
  expect_error(write_v2(target_program(indexed)), "IndexUpfrom call",
               "IndexUpfrom without lower bound");
  bad_index.args.push_back(literal());
  indexed.args[1] = bad_index;
  const mir::Program upfrom = decode_program(write_v2(target_program(indexed)));
  check(upfrom.log_prob[0].target.args[1].name == "IndexUpfrom",
        "IndexUpfrom portable shape");

  bad_index.name = "IndexAll";
  indexed.args[1] = bad_index;
  expect_error(write_v2(target_program(indexed)), "IndexAll call",
               "IndexAll with an operand");

  bad_index.name = "FutureIndex";
  indexed.args[1] = bad_index;
  expect_error(write_v2(target_program(indexed)), "index descriptor",
               "unknown synthetic index");

  bad_index.name = "IndexSingle";
  bad_index.fn_lib = mir::Expr::Lib::UserDefined;
  indexed.args[1] = bad_index;
  expect_error(write_v2(target_program(indexed)), "index descriptor",
               "noncanonical synthetic index library");

  for (const auto& test : std::vector<std::pair<mir::Expr::Kind, const char*>>{
           {mir::Expr::Promotion, "Promotion expression arity"},
           {mir::Expr::TernaryIf, "TernaryIf expression arity"},
           {mir::Expr::EOr, "EOr expression arity"},
           {mir::Expr::EAnd, "EAnd expression arity"}}) {
    mir::Expr expression;
    expression.kind = test.first;
    expression.type_ = "UReal";
    expression.unsized.leaf = mir::UnsizedLeaf::Real;
    expect_error(write_v2(target_program(expression)), test.second,
                 test.second);
  }

  mir::Expr bad_call;
  bad_call.kind = mir::Expr::FunApp;
  bad_call.name = "Plus__";
  bad_call.type_ = "UReal";
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
  wiener.type_ = "UReal";
  wiener.unsized.leaf = mir::UnsizedLeaf::Real;
  wiener.args.assign(7, literal());
  const std::string extended_wiener = write_v2(target_program(wiener));
  const mir::Program decoded_wiener = decode_program(extended_wiener);
  check(decoded_wiener.log_prob.size() == 1 &&
            decoded_wiener.log_prob[0].target.args.size() == 7,
        "portable decoder accepts seven-argument wiener_lpdf");
  expect_compile_error(extended_wiener, "unsupported function wiener_lpdf",
                       "seven-argument wiener reaches execution boundary");
  wiener.args.resize(6);
  expect_error(write_v2(target_program(wiener)), "expected 5 or 7 argument(s)",
               "six-argument wiener rejected structurally");

  mir::Expr bad_metadata = literal();
  bad_metadata.unsized.leaf = mir::UnsizedLeaf::Int;
  expect_error(write_v2(target_program(bad_metadata)),
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
  expect_error(write_v2(bad_binding),
               "variable type disagrees with its binding",
               "variable binding type mismatch");

  mir::Program unsized_assignment;
  mir::Stmt unsized_declaration;
  unsized_declaration.kind = mir::Stmt::Decl;
  unsized_declaration.decl_id = "tmp";
  unsized_declaration.decl_type.raw = "(Unsized (UArray UReal))";
  unsized_assignment.log_prob.push_back(std::move(unsized_declaration));
  mir::Stmt first_assignment;
  first_assignment.kind = mir::Stmt::Assignment;
  first_assignment.lhs = "tmp";
  first_assignment.rhs.kind = mir::Expr::Unsupported;
  first_assignment.rhs.type_ = "UArray";
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
  mir::Program malformed_unsized = unsized_assignment;
  malformed_unsized.log_prob[0].decl_type.raw = "(Unsized (UArray UFuture))";
  expect_error(write_v2(malformed_unsized), "unsized declaration type",
               "malformed unsized declaration spelling");

  mir::Program bad_function;
  mir::FunDef function;
  function.name = "f";
  function.arg_names.push_back("x");
  function.arg_types.push_back("(UArray UReal)");
  function.arg_views.push_back(mir::UnsizedView{0, mir::UnsizedLeaf::Real});
  function.arg_data_only.push_back(true);
  bad_function.fun_defs.push_back(std::move(function));
  expect_error(write_v2(bad_function),
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
  expect_error("STANLI3:", "unrecognized input format", "unknown v2 version");
  expect_error("{\"stanli_ir\":1}", "unrecognized input format",
               "retired portable v1 envelope");
  expect_error(" STANLI2:", "unrecognized input format",
               "v2 leading whitespace");
  expect_error("[]", "unrecognized input format", "unknown input format");
  expect_error(" \n\t", "empty input", "empty input");
  const std::string empty_wire = v2_wire(empty_v2_payload());
  const mir::Program empty = decode_program(empty_wire);
  check(empty.input_vars.empty() && empty.prepare_data.empty() &&
            empty.log_prob.empty() && empty.generate_quantities.empty() &&
            empty.fun_defs.empty() && empty.output_vars.empty(),
        "v2 empty program");
  // empty_v2_payload() stops after output_vars, which is exactly the shape a
  // producer that predates transform_inits emits. It must decode, with no
  // inverse parameter transforms rather than an error.
  check(!empty.has_transform_inits && empty.transform_inits.empty(),
        "v2 document without the trailing section decodes");
  check(empty_wire.find('\0') == std::string::npos,
        "v2 envelope is safe for C-string transports");

  // The current producer writes the section even when it is empty. Presence
  // must survive independently of content so a parameterless current model
  // can unconstrain an empty object, while an old document remains unsupported.
  const mir::Program current_empty = decode_program(write_v2(mir::Program{}));
  check(current_empty.has_transform_inits &&
            current_empty.transform_inits.empty(),
        "v2 explicitly empty transform_inits section stays present");

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

  expect_error("STANLI2:AAA", "multiple of 4", "v2 incomplete base64 quartet");
  expect_error("STANLI2:!!!!", "invalid base64 character",
               "v2 invalid base64 character");
  expect_error("STANLI2:AA==AAAA", "padding before final quartet",
               "v2 early base64 padding");
  expect_error("STANLI2:AB==", "non-canonical base64 tail",
               "v2 non-canonical one-byte tail");
  expect_error("STANLI2:AAB=", "non-canonical base64 tail",
               "v2 non-canonical two-byte tail");

  std::string truncated = empty_v2_payload();
  truncated.pop_back();
  expect_error(v2_wire(truncated), "truncated input", "v2 truncated payload");
  std::string trailing = empty_v2_payload();
  append_u32(trailing, 0);  // an empty transform_inits, which is well formed
  append_u8(trailing, 0);
  expect_error(v2_wire(trailing), "trailing bytes", "v2 trailing payload");
  std::string short_section = empty_v2_payload();
  append_u8(short_section, 0);
  expect_error(v2_wire(short_section), "truncated input",
               "v2 partial trailing section");

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
        "v2 accepts a deeply nested program within the reader-scope budget");
  expect_error(v2_wire(nested_program(520)), "nesting exceeds 512",
               "v2 nesting limit");
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
  if (program_only) {
    check_program_equivalence(argv[2], argv[3]);
    if (failures) {
      std::printf("%d failure(s)\n", failures);
      return 1;
    }
    std::puts("ok");
    return 0;
  }
  check_round_trip("tests/fixtures/es.tmir.sexp");
  check_round_trip("tests/fixtures/tdvocab.tmir.sexp");
  check_round_trip("tests/fixtures/wanames.tmir.sexp");
  check_round_trip("tests/fixtures/odefns.tmir.sexp");
  check_round_trip("tests/fixtures/view_udf_local_data_branch.tmir.sexp");
  check_round_trip("tests/fixtures/paramcond_break.tmir.sexp");
  check_round_trip("tests/fixtures/shape_named_guard.tmir.sexp");
  check_round_trip("tests/fixtures/shape_indexed_guard.tmir.sexp");
  check_round_trip("tests/fixtures/shape_guard_lazy.tmir.sexp");
  check_round_trip("tests/fixtures/shape_partial_guard.tmir.sexp");
  check_lowering_equivalence(argc == 3 ? argv[1] : nullptr,
                             argc == 3 ? argv[2] : nullptr);
  check_full_span_assignment_lowering();
  check_overload_finalization();
  check_exact_float_bits();
  check_nullary_math_constants();
  check_structural_rejections();
  check_v2_rejections();

  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::puts("ok");
  return 0;
}
