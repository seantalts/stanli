#include "mir_reader_internal.hpp"

#include "../third_party/nlohmann_json.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace stanli {
namespace mir {
namespace detail {
namespace {

using json = nlohmann::json;

constexpr size_t kMaxInputBytes = size_t{256} * 1024 * 1024;
constexpr int kMaxJsonDepth = 512;
constexpr size_t kMaxJsonValues = 10000000;
constexpr size_t kMaxArrayItems = 1000000;
constexpr size_t kMaxObjectMembers = 64;
constexpr size_t kMaxStringBytes = size_t{16} * 1024 * 1024;
constexpr size_t kMaxTotalStringBytes = size_t{256} * 1024 * 1024;

[[noreturn]] void fail(const std::string& path, const std::string& message) {
  throw std::runtime_error("portable mir: " + path + ": " + message);
}

std::string child_path(const std::string& path, std::string_view field) {
  return path + "." + std::string(field);
}

std::string item_path(const std::string& path, size_t index) {
  return path + "[" + std::to_string(index) + "]";
}

// nlohmann's DOM parser normally keeps the last occurrence of a duplicate
// object key. Track keys in the parse callback so the wire format rejects the
// duplicate before that information is lost. The callback also bounds the DOM
// while the parser is constructing it rather than after accepting it.
class ParseLimits {
 public:
  bool operator()(int depth, json::parse_event_t event, json& parsed) {
    const bool starts_container = event == json::parse_event_t::object_start ||
                                  event == json::parse_event_t::array_start;
    // Callback depth is zero-based for a container start. A scalar directly
    // inside 512 nested containers arrives at depth 512 and is still valid;
    // a 513th container would start at that same depth and is not.
    if (depth > kMaxJsonDepth || (starts_container && depth >= kMaxJsonDepth))
      throw std::runtime_error("portable mir: JSON nesting exceeds 512");

    switch (event) {
      case json::parse_event_t::object_start:
        count_value();
        count_parent_array_item(depth);
        set_container(depth, Container::Object);
        object_keys_.resize(static_cast<size_t>(depth) + 1);
        object_keys_[static_cast<size_t>(depth)].clear();
        break;
      case json::parse_event_t::array_start:
        count_value();
        count_parent_array_item(depth);
        set_container(depth, Container::Array);
        break;
      case json::parse_event_t::key: {
        count_string(parsed.get_ref<const std::string&>());
        const size_t slot = depth == 0 ? 0 : static_cast<size_t>(depth - 1);
        if (object_keys_.size() <= slot) object_keys_.resize(slot + 1);
        const std::string& key = parsed.get_ref<const std::string&>();
        if (!object_keys_[slot].insert(key).second)
          throw std::runtime_error("portable mir: duplicate JSON key: " + key);
        if (object_keys_[slot].size() > kMaxObjectMembers)
          throw std::runtime_error(
              "portable mir: JSON object exceeds 64 members");
        break;
      }
      case json::parse_event_t::value:
        count_value();
        count_parent_array_item(depth);
        if (parsed.is_string())
          count_string(parsed.get_ref<const std::string&>());
        break;
      case json::parse_event_t::array_end:
        if (parsed.size() > kMaxArrayItems)
          throw std::runtime_error(
              "portable mir: JSON array exceeds 1000000 items");
        clear_container(depth);
        break;
      case json::parse_event_t::object_end:
        if (parsed.size() > kMaxObjectMembers)
          throw std::runtime_error(
              "portable mir: JSON object exceeds 64 members");
        clear_container(depth);
        break;
    }
    return true;
  }

 private:
  enum class Container : uint8_t { None, Object, Array };

  void set_container(int depth, Container kind) {
    const size_t slot = static_cast<size_t>(depth);
    if (containers_.size() <= slot) containers_.resize(slot + 1);
    if (array_items_.size() <= slot) array_items_.resize(slot + 1);
    containers_[slot] = kind;
    array_items_[slot] = 0;
  }

  void clear_container(int depth) {
    const size_t slot = static_cast<size_t>(depth);
    if (slot < containers_.size()) containers_[slot] = Container::None;
    if (slot < array_items_.size()) array_items_[slot] = 0;
  }

  // A container-start callback and a scalar-value callback both arrive at
  // the depth of their open parent stack, before nlohmann inserts the value
  // into that parent. Reject the first excessive item at that point, rather
  // than after the complete array has already been allocated.
  void count_parent_array_item(int depth) {
    if (depth <= 0) return;
    const size_t parent = static_cast<size_t>(depth - 1);
    if (parent >= containers_.size() || containers_[parent] != Container::Array)
      return;
    if (++array_items_[parent] > kMaxArrayItems)
      throw std::runtime_error(
          "portable mir: JSON array exceeds 1000000 items");
  }

  void count_value() {
    if (++values_ > kMaxJsonValues)
      throw std::runtime_error("portable mir: JSON exceeds 10000000 values");
  }

  void count_string(const std::string& value) {
    if (value.size() > kMaxStringBytes)
      throw std::runtime_error(
          "portable mir: JSON string exceeds 16777216 bytes");
    if (value.size() > kMaxTotalStringBytes - total_string_bytes_)
      throw std::runtime_error(
          "portable mir: decoded JSON strings exceed 268435456 bytes");
    total_string_bytes_ += value.size();
  }

  size_t values_ = 0;
  size_t total_string_bytes_ = 0;
  std::vector<Container> containers_;
  std::vector<size_t> array_items_;
  std::vector<std::unordered_set<std::string>> object_keys_;
};

const json& require_object(const json& value, const std::string& path) {
  if (!value.is_object()) fail(path, "expected object");
  if (value.size() > kMaxObjectMembers) fail(path, "object exceeds 64 members");
  return value;
}

const json& require_array(const json& value, const std::string& path) {
  if (!value.is_array()) fail(path, "expected array");
  if (value.size() > kMaxArrayItems) fail(path, "array exceeds 1000000 items");
  return value;
}

void require_keys(const json& value, const std::string& path,
                  std::initializer_list<std::string_view> required) {
  require_object(value, path);
  for (const auto& entry : value.items()) {
    if (std::find(required.begin(), required.end(), entry.key()) ==
        required.end())
      fail(child_path(path, entry.key()), "unknown field");
  }
  for (std::string_view key : required)
    if (value.find(std::string(key)) == value.end())
      fail(child_path(path, key), "missing required field");
}

const json& member(const json& value, const std::string& path,
                   std::string_view key) {
  const auto found = value.find(std::string(key));
  if (found == value.end())
    fail(child_path(path, key), "missing required field");
  return *found;
}

std::string read_string(const json& value, const std::string& path) {
  if (!value.is_string()) fail(path, "expected string");
  const std::string& text = value.get_ref<const std::string&>();
  if (text.size() > kMaxStringBytes)
    fail(path, "string exceeds 16777216 bytes");
  return text;
}

bool read_bool(const json& value, const std::string& path) {
  if (!value.is_boolean()) fail(path, "expected boolean");
  return value.get<bool>();
}

uint8_t read_depth(const json& value, const std::string& path) {
  if (!value.is_number_integer()) fail(path, "expected integer from 0 to 255");
  if (value.is_number_unsigned()) {
    const uint64_t depth = value.get<uint64_t>();
    if (depth <= std::numeric_limits<uint8_t>::max())
      return static_cast<uint8_t>(depth);
  } else {
    const int64_t depth = value.get<int64_t>();
    if (depth >= 0 && depth <= std::numeric_limits<uint8_t>::max())
      return static_cast<uint8_t>(depth);
  }
  fail(path, "integer is outside 0..255");
}

int read_version(const json& value, const std::string& path) {
  if (!value.is_number_integer()) fail(path, "expected integer version");
  if (value.is_number_unsigned()) {
    const uint64_t version = value.get<uint64_t>();
    if (version == 1) return 1;
  } else if (value.get<int64_t>() == 1) {
    return 1;
  }
  fail(path, "unsupported version (expected 1)");
}

long read_stan_int(const json& value, const std::string& path) {
  const std::string text = read_string(value, path);
  if (text.empty()) fail(path, "empty integer spelling");
  size_t offset = 0;
  bool negative = false;
  if (text[0] == '-') {
    negative = true;
    offset = 1;
    if (offset == text.size()) fail(path, "invalid integer spelling");
  } else if (text[0] == '+') {
    fail(path, "integer spelling must not start with +");
  }
  if (text[offset] == '0' && text.size() - offset != 1)
    fail(path, "integer spelling has a leading zero");
  if (negative && text[offset] == '0')
    fail(path, "integer spelling must not be -0");

  int64_t magnitude = 0;
  const int64_t limit = negative ? int64_t{2147483648} : int64_t{2147483647};
  for (; offset < text.size(); ++offset) {
    const char c = text[offset];
    if (c < '0' || c > '9') fail(path, "invalid integer spelling");
    const int digit = c - '0';
    if (magnitude > (limit - digit) / 10)
      fail(path, "integer is outside signed 32-bit range");
    magnitude = magnitude * 10 + digit;
  }
  const int64_t result = negative ? -magnitude : magnitude;
  return static_cast<long>(result);
}

double read_f64(const json& value, const std::string& path) {
  const std::string text = read_string(value, path);
  if (text.size() != 20 || text.compare(0, 4, "f64:") != 0)
    fail(path, "expected f64: followed by 16 lowercase hexadecimal digits");
  uint64_t bits = 0;
  for (size_t i = 4; i < text.size(); ++i) {
    const char c = text[i];
    uint64_t nibble = 0;
    if (c >= '0' && c <= '9')
      nibble = static_cast<uint64_t>(c - '0');
    else if (c >= 'a' && c <= 'f')
      nibble = static_cast<uint64_t>(c - 'a' + 10);
    else
      fail(path, "expected lowercase hexadecimal digits");
    bits = (bits << 4) | nibble;
  }
  static_assert(sizeof(double) == sizeof(bits) &&
                    std::numeric_limits<double>::is_iec559 &&
                    std::numeric_limits<double>::digits == 53,
                "portable MIR requires IEEE-754 binary64 doubles");
  double result;
  std::memcpy(&result, &bits, sizeof(result));
  return result;
}

template <typename T, typename Reader>
std::vector<T> read_vector(const json& value, const std::string& path,
                           Reader read) {
  const json& array = require_array(value, path);
  std::vector<T> result;
  result.reserve(array.size());
  for (size_t i = 0; i < array.size(); ++i)
    result.push_back(read(array[i], item_path(path, i)));
  return result;
}

UnsizedLeaf read_unsized_leaf(const json& value, const std::string& path) {
  const std::string name = read_string(value, path);
#define STANLI_ENUM_CASE(x) \
  if (name == #x) return UnsizedLeaf::x
  STANLI_ENUM_CASE(Unknown);
  STANLI_ENUM_CASE(Int);
  STANLI_ENUM_CASE(Real);
  STANLI_ENUM_CASE(Complex);
  STANLI_ENUM_CASE(Vector);
  STANLI_ENUM_CASE(RowVector);
  STANLI_ENUM_CASE(Matrix);
#undef STANLI_ENUM_CASE
  fail(path, "unknown UnsizedLeaf: " + name);
}

UnsizedView read_unsized(const json& value, const std::string& path) {
  require_keys(value, path, {"depth", "leaf"});
  return UnsizedView{
      read_depth(member(value, path, "depth"), child_path(path, "depth")),
      read_unsized_leaf(member(value, path, "leaf"), child_path(path, "leaf"))};
}

Expr::Kind read_expr_kind(const json& value, const std::string& path) {
  const std::string name = read_string(value, path);
#define STANLI_ENUM_CASE(x) \
  if (name == #x) return Expr::x
  STANLI_ENUM_CASE(Var);
  STANLI_ENUM_CASE(LitInt);
  STANLI_ENUM_CASE(LitReal);
  STANLI_ENUM_CASE(LitStr);
  STANLI_ENUM_CASE(FunApp);
  STANLI_ENUM_CASE(Promotion);
  STANLI_ENUM_CASE(Indexed);
  STANLI_ENUM_CASE(TernaryIf);
  STANLI_ENUM_CASE(EOr);
  STANLI_ENUM_CASE(EAnd);
  STANLI_ENUM_CASE(Unsupported);
#undef STANLI_ENUM_CASE
  fail(path, "unknown Expr::Kind: " + name);
}

Expr::Lib read_expr_lib(const json& value, const std::string& path) {
  const std::string name = read_string(value, path);
#define STANLI_ENUM_CASE(x) \
  if (name == #x) return Expr::Lib::x
  STANLI_ENUM_CASE(StanLib);
  STANLI_ENUM_CASE(Internal);
  STANLI_ENUM_CASE(UserDefined);
#undef STANLI_ENUM_CASE
  fail(path, "unknown Expr::Lib: " + name);
}

Expr read_expr(const json& value, const std::string& path) {
  require_keys(value, path,
               {"kind", "name", "fn_lib", "fn_propto", "lit_i", "lit", "lit_s",
                "args", "type_", "unsized", "data_only", "promoted", "raw"});
  Expr result;
  result.kind =
      read_expr_kind(member(value, path, "kind"), child_path(path, "kind"));
  result.name =
      read_string(member(value, path, "name"), child_path(path, "name"));
  result.fn_lib =
      read_expr_lib(member(value, path, "fn_lib"), child_path(path, "fn_lib"));
  result.fn_propto = read_bool(member(value, path, "fn_propto"),
                               child_path(path, "fn_propto"));
  result.lit_i =
      read_stan_int(member(value, path, "lit_i"), child_path(path, "lit_i"));
  result.lit = read_f64(member(value, path, "lit"), child_path(path, "lit"));
  result.lit_s =
      read_string(member(value, path, "lit_s"), child_path(path, "lit_s"));
  result.args = read_vector<Expr>(member(value, path, "args"),
                                  child_path(path, "args"), read_expr);
  result.type_ =
      read_string(member(value, path, "type_"), child_path(path, "type_"));
  result.unsized =
      read_unsized(member(value, path, "unsized"), child_path(path, "unsized"));
  result.data_only = read_bool(member(value, path, "data_only"),
                               child_path(path, "data_only"));
  result.promoted =
      read_bool(member(value, path, "promoted"), child_path(path, "promoted"));
  result.raw = read_string(member(value, path, "raw"), child_path(path, "raw"));
  return result;
}

Transform::Kind read_transform_kind(const json& value,
                                    const std::string& path) {
  const std::string name = read_string(value, path);
#define STANLI_ENUM_CASE(x) \
  if (name == #x) return Transform::x
  STANLI_ENUM_CASE(Identity);
  STANLI_ENUM_CASE(Lower);
  STANLI_ENUM_CASE(Upper);
  STANLI_ENUM_CASE(LowerUpper);
  STANLI_ENUM_CASE(Offset);
  STANLI_ENUM_CASE(Multiplier);
  STANLI_ENUM_CASE(OffsetMultiplier);
  STANLI_ENUM_CASE(Simplex);
  STANLI_ENUM_CASE(Ordered);
  STANLI_ENUM_CASE(PositiveOrdered);
  STANLI_ENUM_CASE(CholeskyCorr);
  STANLI_ENUM_CASE(UnitVector);
  STANLI_ENUM_CASE(SumToZero);
  STANLI_ENUM_CASE(Correlation);
  STANLI_ENUM_CASE(Covariance);
  STANLI_ENUM_CASE(CholeskyCov);
  STANLI_ENUM_CASE(Unsupported);
#undef STANLI_ENUM_CASE
  fail(path, "unknown Transform::Kind: " + name);
}

Transform read_transform(const json& value, const std::string& path) {
  require_keys(value, path, {"kind", "args", "raw"});
  Transform result;
  result.kind = read_transform_kind(member(value, path, "kind"),
                                    child_path(path, "kind"));
  result.args = read_vector<Expr>(member(value, path, "args"),
                                  child_path(path, "args"), read_expr);
  result.raw = read_string(member(value, path, "raw"), child_path(path, "raw"));
  return result;
}

SizedType read_sized_type(const json& value, const std::string& path) {
  require_keys(value, path, {"base", "dims", "elem_base", "raw"});
  SizedType result;
  result.base =
      read_string(member(value, path, "base"), child_path(path, "base"));
  result.dims = read_vector<Expr>(member(value, path, "dims"),
                                  child_path(path, "dims"), read_expr);
  result.elem_base = read_string(member(value, path, "elem_base"),
                                 child_path(path, "elem_base"));
  result.raw = read_string(member(value, path, "raw"), child_path(path, "raw"));
  return result;
}

Stmt::Kind read_stmt_kind(const json& value, const std::string& path) {
  const std::string name = read_string(value, path);
#define STANLI_ENUM_CASE(x) \
  if (name == #x) return Stmt::x
  STANLI_ENUM_CASE(Decl);
  STANLI_ENUM_CASE(Assignment);
  STANLI_ENUM_CASE(TargetPE);
  STANLI_ENUM_CASE(Block);
  STANLI_ENUM_CASE(SList);
  STANLI_ENUM_CASE(For);
  STANLI_ENUM_CASE(IfElse);
  STANLI_ENUM_CASE(While);
  STANLI_ENUM_CASE(NRFunApp);
  STANLI_ENUM_CASE(Return);
  STANLI_ENUM_CASE(Skip);
  STANLI_ENUM_CASE(Unsupported);
#undef STANLI_ENUM_CASE
  fail(path, "unknown Stmt::Kind: " + name);
}

Stmt read_stmt(const json& value, const std::string& path) {
  require_keys(value, path,
               {"kind",
                "decl_id",
                "decl_type",
                "decl_data_only",
                "has_init",
                "init",
                "read_transform",
                "read_dims",
                "lhs",
                "lhs_idx",
                "rhs",
                "target",
                "fn_name",
                "fn_args",
                "check_transform",
                "check_var_name",
                "loopvar",
                "lower",
                "upper",
                "cond",
                "body",
                "raw"});
  Stmt result;
  result.kind =
      read_stmt_kind(member(value, path, "kind"), child_path(path, "kind"));
  result.decl_id =
      read_string(member(value, path, "decl_id"), child_path(path, "decl_id"));
  result.decl_type = read_sized_type(member(value, path, "decl_type"),
                                     child_path(path, "decl_type"));
  result.decl_data_only = read_bool(member(value, path, "decl_data_only"),
                                    child_path(path, "decl_data_only"));
  result.has_init =
      read_bool(member(value, path, "has_init"), child_path(path, "has_init"));
  result.init =
      read_expr(member(value, path, "init"), child_path(path, "init"));

  const json& read_transform_value = member(value, path, "read_transform");
  if (!read_transform_value.is_null())
    result.read_transform = read_transform(read_transform_value,
                                           child_path(path, "read_transform"));
  result.read_dims =
      read_vector<Expr>(member(value, path, "read_dims"),
                        child_path(path, "read_dims"), read_expr);
  result.lhs = read_string(member(value, path, "lhs"), child_path(path, "lhs"));
  result.lhs_idx = read_vector<Expr>(member(value, path, "lhs_idx"),
                                     child_path(path, "lhs_idx"), read_expr);
  result.rhs = read_expr(member(value, path, "rhs"), child_path(path, "rhs"));
  result.target =
      read_expr(member(value, path, "target"), child_path(path, "target"));
  result.fn_name =
      read_string(member(value, path, "fn_name"), child_path(path, "fn_name"));
  result.fn_args = read_vector<Expr>(member(value, path, "fn_args"),
                                     child_path(path, "fn_args"), read_expr);

  const json& check_transform_value = member(value, path, "check_transform");
  if (!check_transform_value.is_null())
    result.check_transform = read_transform(
        check_transform_value, child_path(path, "check_transform"));
  result.check_var_name = read_string(member(value, path, "check_var_name"),
                                      child_path(path, "check_var_name"));
  result.loopvar =
      read_string(member(value, path, "loopvar"), child_path(path, "loopvar"));
  result.lower =
      read_expr(member(value, path, "lower"), child_path(path, "lower"));
  result.upper =
      read_expr(member(value, path, "upper"), child_path(path, "upper"));
  result.cond =
      read_expr(member(value, path, "cond"), child_path(path, "cond"));
  result.body = read_vector<Stmt>(member(value, path, "body"),
                                  child_path(path, "body"), read_stmt);
  result.raw = read_string(member(value, path, "raw"), child_path(path, "raw"));
  return result;
}

FunDef read_fun_def(const json& value, const std::string& path) {
  require_keys(
      value, path,
      {"name", "arg_names", "arg_types", "arg_views", "arg_data_only", "body"});
  FunDef result;
  result.name =
      read_string(member(value, path, "name"), child_path(path, "name"));
  result.arg_names =
      read_vector<std::string>(member(value, path, "arg_names"),
                               child_path(path, "arg_names"), read_string);
  result.arg_types =
      read_vector<std::string>(member(value, path, "arg_types"),
                               child_path(path, "arg_types"), read_string);
  result.arg_views =
      read_vector<UnsizedView>(member(value, path, "arg_views"),
                               child_path(path, "arg_views"), read_unsized);
  result.arg_data_only =
      read_vector<bool>(member(value, path, "arg_data_only"),
                        child_path(path, "arg_data_only"), read_bool);
  result.body = read_vector<Stmt>(member(value, path, "body"),
                                  child_path(path, "body"), read_stmt);
  const size_t arity = result.arg_names.size();
  if (result.arg_types.size() != arity || result.arg_views.size() != arity ||
      result.arg_data_only.size() != arity)
    fail(path, "function argument field lengths disagree");
  return result;
}

std::pair<std::string, SizedType> read_input_var(const json& value,
                                                 const std::string& path) {
  require_keys(value, path, {"name", "type"});
  return {
      read_string(member(value, path, "name"), child_path(path, "name")),
      read_sized_type(member(value, path, "type"), child_path(path, "type"))};
}

Program read_program_object(const json& value, const std::string& path) {
  require_keys(value, path,
               {"input_vars", "prepare_data", "log_prob", "generate_quantities",
                "fun_defs", "output_vars"});
  Program result;
  result.input_vars = read_vector<std::pair<std::string, SizedType>>(
      member(value, path, "input_vars"), child_path(path, "input_vars"),
      read_input_var);
  result.prepare_data =
      read_vector<Stmt>(member(value, path, "prepare_data"),
                        child_path(path, "prepare_data"), read_stmt);
  result.log_prob = read_vector<Stmt>(member(value, path, "log_prob"),
                                      child_path(path, "log_prob"), read_stmt);
  result.generate_quantities =
      read_vector<Stmt>(member(value, path, "generate_quantities"),
                        child_path(path, "generate_quantities"), read_stmt);
  result.fun_defs =
      read_vector<FunDef>(member(value, path, "fun_defs"),
                          child_path(path, "fun_defs"), read_fun_def);
  result.output_vars =
      read_vector<std::string>(member(value, path, "output_vars"),
                               child_path(path, "output_vars"), read_string);
  return result;
}

}  // namespace

Program read_portable_program(std::string_view text) {
  if (text.size() > kMaxInputBytes)
    throw std::runtime_error("portable mir: input exceeds 268435456 bytes");

  json envelope;
  try {
    ParseLimits limits;
    envelope = json::parse(text.begin(), text.end(), std::ref(limits));
  } catch (const nlohmann::json::exception& error) {
    throw std::runtime_error(std::string("portable mir: invalid JSON: ") +
                             error.what());
  }

  require_keys(envelope, "$", {"stanli_ir", "program"});
  (void)read_version(member(envelope, "$", "stanli_ir"), "$.stanli_ir");
  Program result =
      read_program_object(member(envelope, "$", "program"), "$.program");
  validate_portable_program(result);
  finalize_program(result, true);
  return result;
}

}  // namespace detail
}  // namespace mir
}  // namespace stanli
