#include "mir_reader_internal.hpp"
#include "mir_tuples.hpp"

#include <stanli/large_stack.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace stanli {
namespace mir {
namespace detail {
namespace {

constexpr std::string_view kMagic = "STANLI5:";
constexpr size_t kMaxInputBytes = size_t{256} * 1024 * 1024;
constexpr size_t kMaxStringBytes = size_t{16} * 1024 * 1024;
constexpr size_t kMaxTotalStringBytes = size_t{256} * 1024 * 1024;
constexpr size_t kMaxListStorageBytes = size_t{256} * 1024 * 1024;
constexpr uint32_t kMaxListItems = 1000000;
constexpr size_t kMaxValues = 10000000;

[[noreturn]] void fail_wire(size_t offset, const std::string& message) {
  throw std::runtime_error("portable mir at byte " + std::to_string(offset) +
                           ": " + message);
}

int base64_value(unsigned char byte) {
  if (byte >= 'A' && byte <= 'Z') return byte - 'A';
  if (byte >= 'a' && byte <= 'z') return byte - 'a' + 26;
  if (byte >= '0' && byte <= '9') return byte - '0' + 52;
  if (byte == '+') return 62;
  if (byte == '/') return 63;
  return -1;
}

std::string decode_payload(std::string_view wire) {
  if (wire.size() > kMaxInputBytes)
    fail_wire(0, "input exceeds 268435456 bytes");
  if (wire.size() < kMagic.size() || wire.substr(0, kMagic.size()) != kMagic)
    fail_wire(0, "missing STANLI5 header");
  const std::string_view encoded = wire.substr(kMagic.size());
  if (encoded.size() % 4 != 0)
    fail_wire(wire.size(), "base64 length is not a multiple of 4");

  size_t decoded_size = (encoded.size() / 4) * 3;
  if (!encoded.empty() && encoded.back() == '=') --decoded_size;
  if (encoded.size() >= 2 && encoded[encoded.size() - 2] == '=') --decoded_size;
  if (decoded_size > kMaxInputBytes)
    fail_wire(0, "decoded payload exceeds 268435456 bytes");

  std::string decoded;
  decoded.reserve(decoded_size);
  for (size_t offset = 0; offset < encoded.size(); offset += 4) {
    const bool last = offset + 4 == encoded.size();
    const unsigned char c0 = static_cast<unsigned char>(encoded[offset]);
    const unsigned char c1 = static_cast<unsigned char>(encoded[offset + 1]);
    const unsigned char c2 = static_cast<unsigned char>(encoded[offset + 2]);
    const unsigned char c3 = static_cast<unsigned char>(encoded[offset + 3]);
    const int a = base64_value(c0);
    const int b = base64_value(c1);
    const int c = c2 == '=' ? -2 : base64_value(c2);
    const int d = c3 == '=' ? -2 : base64_value(c3);
    const size_t wire_offset = kMagic.size() + offset;
    if (a < 0 || b < 0 || c == -1 || d == -1)
      fail_wire(wire_offset, "invalid base64 character");
    if (!last && (c < 0 || d < 0))
      fail_wire(wire_offset, "base64 padding before final quartet");
    if (c == -2) {
      if (!last || d != -2) fail_wire(wire_offset, "invalid base64 padding");
      if ((b & 0x0f) != 0) fail_wire(wire_offset, "non-canonical base64 tail");
      decoded.push_back(static_cast<char>((a << 2) | (b >> 4)));
      continue;
    }
    decoded.push_back(static_cast<char>((a << 2) | (b >> 4)));
    decoded.push_back(static_cast<char>((b << 4) | (c >> 2)));
    if (d == -2) {
      if (!last || (c & 0x03) != 0)
        fail_wire(wire_offset, d == -2 && last ? "non-canonical base64 tail"
                                               : "invalid base64 padding");
      continue;
    }
    decoded.push_back(static_cast<char>((c << 6) | d));
  }
  if (decoded.size() != decoded_size)
    fail_wire(wire.size(), "decoded payload length mismatch");
  return decoded;
}

class Reader {
 public:
  explicit Reader(std::string_view bytes) : bytes_(bytes) {
    if (bytes.size() > kMaxInputBytes) fail("input exceeds 268435456 bytes");
  }

  void finish() {
    if (position_ != bytes_.size()) fail("trailing bytes");
  }

  // True once every required section has been read. The transform_inits
  // section is appended after them, so its absence is what distinguishes a
  // document written before it existed from a truncated one.
  bool at_end() const { return position_ == bytes_.size(); }

  uint8_t u8() {
    count_value();
    require(1);
    return static_cast<uint8_t>(bytes_[position_++]);
  }

  bool boolean() {
    const uint8_t value = u8();
    if (value > 1) fail("boolean is not 0 or 1");
    return value != 0;
  }

  uint32_t u32() {
    count_value();
    require(4);
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i)
      value |= static_cast<uint32_t>(
                   static_cast<unsigned char>(bytes_[position_ + i]))
               << (i * 8);
    position_ += 4;
    return value;
  }

  long i32() {
    const uint32_t bits = u32();
    int32_t value = 0;
    static_assert(sizeof(value) == sizeof(bits), "int32 width");
    std::memcpy(&value, &bits, sizeof(value));
    return static_cast<long>(value);
  }

  double f64() {
    count_value();
    require(8);
    uint64_t bits = 0;
    for (unsigned i = 0; i < 8; ++i)
      bits |= static_cast<uint64_t>(
                  static_cast<unsigned char>(bytes_[position_ + i]))
              << (i * 8);
    position_ += 8;
    static_assert(sizeof(double) == sizeof(bits) &&
                      std::numeric_limits<double>::is_iec559 &&
                      std::numeric_limits<double>::digits == 53,
                  "portable MIR requires IEEE-754 binary64 doubles");
    double value = 0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
  }

  std::string string() {
    const uint32_t size = u32();
    if (size > kMaxStringBytes) fail("string exceeds 16777216 bytes");
    if (size > kMaxTotalStringBytes - total_string_bytes_)
      fail("decoded strings exceed 268435456 bytes");
    require(size);
    const std::string_view value = bytes_.substr(position_, size);
    validate_utf8(value);
    position_ += size;
    total_string_bytes_ += size;
    return std::string(value);
  }

  uint32_t list_size() {
    const uint32_t size = u32();
    if (size > kMaxListItems) fail("list exceeds 1000000 items");
    // Every encoded item occupies at least one byte. Reject impossible counts
    // before reserve() can allocate based on an attacker-controlled length.
    if (size > remaining()) fail("list count exceeds remaining input");
    return size;
  }

  unsigned peak_depth() const { return peak_; }

  class Depth {
   public:
    explicit Depth(Reader& reader) : reader_(reader) {
      if (reader_.depth_ >= 2 * nesting_limit())
        throw NestingTooDeep("portable mir: nested too deeply (more than " +
                             std::to_string(nesting_limit()) + " levels)");
      reader_.peak_ = std::max(reader_.peak_, ++reader_.depth_);
    }
    ~Depth() { --reader_.depth_; }

   private:
    Reader& reader_;
  };

  template <typename T, typename Read>
  std::vector<T> list(Read read) {
    Depth depth(*this);
    const uint32_t size = list_size();
    std::vector<T> result;
    if (size > (kMaxListStorageBytes - list_storage_bytes_) / sizeof(T))
      fail("decoded list storage exceeds 268435456 bytes");
    list_storage_bytes_ += static_cast<size_t>(size) * sizeof(T);
    result.reserve(size);
    for (uint32_t i = 0; i < size; ++i) result.push_back(read());
    return result;
  }

  [[noreturn]] void fail(const std::string& message) const {
    throw std::runtime_error("portable mir at byte " +
                             std::to_string(position_) + ": " + message);
  }

 private:
  size_t remaining() const { return bytes_.size() - position_; }

  void require(size_t count) const {
    if (count > remaining()) fail("truncated input");
  }

  void count_value() {
    if (++values_ > kMaxValues) fail("document exceeds 10000000 values");
  }

  void validate_utf8(std::string_view value) const {
    const auto continuation = [](unsigned char byte) {
      return (byte & 0xc0) == 0x80;
    };
    for (size_t i = 0; i < value.size();) {
      const unsigned char first = static_cast<unsigned char>(value[i]);
      if (first < 0x80) {
        ++i;
      } else if (first >= 0xc2 && first <= 0xdf) {
        if (i + 1 >= value.size() ||
            !continuation(static_cast<unsigned char>(value[i + 1])))
          fail("invalid UTF-8 string");
        i += 2;
      } else if (first >= 0xe0 && first <= 0xef) {
        if (i + 2 >= value.size()) fail("invalid UTF-8 string");
        const unsigned char second = static_cast<unsigned char>(value[i + 1]);
        const unsigned char third = static_cast<unsigned char>(value[i + 2]);
        if (!continuation(third) ||
            (first == 0xe0   ? second < 0xa0 || second > 0xbf
             : first == 0xed ? second < 0x80 || second > 0x9f
                             : !continuation(second)))
          fail("invalid UTF-8 string");
        i += 3;
      } else if (first >= 0xf0 && first <= 0xf4) {
        if (i + 3 >= value.size()) fail("invalid UTF-8 string");
        const unsigned char second = static_cast<unsigned char>(value[i + 1]);
        if ((first == 0xf0   ? second < 0x90 || second > 0xbf
             : first == 0xf4 ? second < 0x80 || second > 0x8f
                             : !continuation(second)) ||
            !continuation(static_cast<unsigned char>(value[i + 2])) ||
            !continuation(static_cast<unsigned char>(value[i + 3])))
          fail("invalid UTF-8 string");
        i += 4;
      } else {
        fail("invalid UTF-8 string");
      }
    }
  }

  std::string_view bytes_;
  size_t position_ = 0;
  size_t values_ = 0;
  size_t total_string_bytes_ = 0;
  size_t list_storage_bytes_ = 0;
  unsigned depth_ = 0;
  unsigned peak_ = 0;
};

template <typename T, size_t N>
T read_tag(Reader& reader, uint8_t tag, const std::array<T, N>& values,
           const char* diagnostic) {
  if (tag >= values.size()) reader.fail(diagnostic);
  return values[tag];
}

constexpr std::array<UnsizedLeaf, 7> kLeafTags = {
    UnsizedLeaf::Unknown, UnsizedLeaf::Int,    UnsizedLeaf::Real,
    UnsizedLeaf::Complex, UnsizedLeaf::Vector, UnsizedLeaf::RowVector,
    UnsizedLeaf::Matrix};

constexpr std::array<Expr::Kind, 16> kExprTags = {
    Expr::Var,         Expr::LitInt,      Expr::LitReal,      Expr::LitStr,
    Expr::FunApp,      Expr::Promotion,   Expr::Indexed,      Expr::TernaryIf,
    Expr::EOr,         Expr::EAnd,        Expr::Unsupported,  Expr::IndexAll,
    Expr::IndexSingle, Expr::IndexUpfrom, Expr::IndexBetween, Expr::IndexMulti};

constexpr std::array<Expr::Lib, 3> kLibraryTags = {
    Expr::Lib::StanLib, Expr::Lib::Internal, Expr::Lib::UserDefined};

constexpr std::array<FnSuffix, 6> kSuffixTags = {
    FnSuffix::Plain, FnSuffix::Rng,    FnSuffix::Lpdf,
    FnSuffix::Lpmf,  FnSuffix::Target, FnSuffix::Jacobian};

constexpr std::array<Transform::Kind, 19> kTransformTags = {
    Transform::Identity,
    Transform::Lower,
    Transform::Upper,
    Transform::LowerUpper,
    Transform::Offset,
    Transform::Multiplier,
    Transform::OffsetMultiplier,
    Transform::Simplex,
    Transform::Ordered,
    Transform::PositiveOrdered,
    Transform::CholeskyCorr,
    Transform::UnitVector,
    Transform::SumToZero,
    Transform::Correlation,
    Transform::Covariance,
    Transform::CholeskyCov,
    Transform::StochasticColumn,
    Transform::StochasticRow,
    Transform::Unsupported};

// Tag 14 is `jacobian +=`, a TargetPE with its jacobian flag set.
constexpr uint8_t kJacobianPETag = 14;
constexpr std::array<Stmt::Kind, 16> kStmtTags = {
    Stmt::Decl,     Stmt::Assignment,  Stmt::TargetPE, Stmt::Block,
    Stmt::SList,    Stmt::For,         Stmt::IfElse,   Stmt::While,
    Stmt::NRFunApp, Stmt::Return,      Stmt::Break,    Stmt::Continue,
    Stmt::Skip,     Stmt::Unsupported, Stmt::TargetPE, Stmt::MultiAssign};

UnsizedView read_view(Reader& reader) {
  UnsizedView view;
  view.depth = reader.u8();
  const uint8_t leaf = reader.u8();
  view.leaf = read_tag(reader, leaf, kLeafTags, "unknown unsized-view tag");
  return view;
}

std::vector<bool> read_bools(Reader& reader) {
  return reader.list<bool>([&] { return reader.boolean(); });
}

Expr read_expr(Reader& reader);
Stmt read_stmt(Reader& reader);

FnSuffix read_suffix(Reader& reader) {
  const uint8_t tag = reader.u8();
  return read_tag(reader, tag, kSuffixTags, "unknown function-suffix tag");
}

FunctionType read_function_type(Reader& reader) {
  Reader::Depth depth(reader);
  FunctionType value;
  value.args = reader.list<UnsizedView>([&] { return read_view(reader); });
  value.suffix = read_suffix(reader);
  value.propto = reader.boolean();
  return value;
}

std::vector<Expr> read_exprs(Reader& reader) {
  return reader.list<Expr>([&] { return read_expr(reader); });
}

void read_meta(Reader& reader, Expr& value) {
  value.unsized = read_view(reader);
  value.data_only = reader.boolean();
  value.promoted = reader.boolean();
  value.raw = reader.string();
}

Expr read_expr(Reader& reader) {
  Reader::Depth depth(reader);
  Expr value;
  const uint8_t tag = reader.u8();
  value.kind = read_tag(reader, tag, kExprTags, "unknown expression tag");
  switch (value.kind) {
    case Expr::Var: {
      value.name = reader.string();
      const uint8_t present = reader.u8();
      if (present > 1) reader.fail("optional marker is not 0 or 1");
      if (present) {
        value.fn_type = read_function_type(reader);
        value.fn_propto = value.fn_type->propto;
      }
      break;
    }
    case Expr::LitInt:
      value.lit_i = reader.i32();
      value.lit = static_cast<double>(value.lit_i);
      break;
    case Expr::LitReal:
      value.lit = reader.f64();
      break;
    case Expr::LitStr:
      value.lit_s = reader.string();
      break;
    case Expr::FunApp: {
      const uint8_t library = reader.u8();
      value.fn_lib = read_tag(reader, library, kLibraryTags,
                              "unknown function-library tag");
      value.name = reader.string();
      value.fn_suffix = read_suffix(reader);
      value.fn_propto = reader.boolean();
      value.args = read_exprs(reader);
      break;
    }
    case Expr::Promotion:
    case Expr::Indexed:
    case Expr::TernaryIf:
    case Expr::EOr:
    case Expr::EAnd:
    case Expr::IndexAll:
    case Expr::IndexSingle:
    case Expr::IndexUpfrom:
    case Expr::IndexBetween:
    case Expr::IndexMulti:
      value.args = read_exprs(reader);
      break;
    case Expr::Unsupported:
      break;
  }
  read_meta(reader, value);
  return value;
}

Transform read_transform(Reader& reader) {
  Reader::Depth depth(reader);
  Transform value;
  const uint8_t tag = reader.u8();
  value.kind = read_tag(reader, tag, kTransformTags, "unknown transform tag");
  value.args = read_exprs(reader);
  value.raw = reader.string();
  return value;
}

std::optional<Transform> read_optional_transform(Reader& reader) {
  const uint8_t present = reader.u8();
  if (present > 1) reader.fail("optional marker is not 0 or 1");
  if (!present) return std::nullopt;
  return read_transform(reader);
}

// A sized type of an input or output variable, which may be a tuple.
TupleType read_interface_type(Reader& reader) {
  Reader::Depth depth(reader);
  TupleType value;
  value.type.base = reader.string();
  value.type.dims = read_exprs(reader);
  value.type.elem_base = reader.string();
  value.type.raw = reader.string();
  if (is_tuple(value.type))
    value.components =
        reader.list<TupleType>([&] { return read_interface_type(reader); });
  return value;
}

SizedType read_sized(Reader& reader) {
  Reader::Depth depth(reader);
  SizedType value;
  value.base = reader.string();
  value.dims = read_exprs(reader);
  value.elem_base = reader.string();
  value.raw = reader.string();
  if (is_tuple(value))
    reader.fail("a tuple type outside the input and output variables");
  return value;
}

// A declaration's type: tag 0 is sized, tag 1 unsized.
SizedType read_decl_type(Reader& reader) {
  const uint8_t tag = reader.u8();
  if (tag == 0) return read_sized(reader);
  if (tag != 1) reader.fail("unknown declaration-type tag");
  const UnsizedView view = read_view(reader);
  std::string raw = reader.string();
  return unsized_declaration(view, std::move(raw));
}

std::vector<std::string> read_strings(Reader& reader) {
  return reader.list<std::string>([&] { return reader.string(); });
}

PrintItem read_print_item(Reader& reader) {
  Reader::Depth depth(reader);
  PrintItem item;
  const uint8_t kind = reader.u8();
  if (kind > 1) reader.fail("unknown print-item tag");
  if (kind == 1) {
    item.kind = PrintItem::Kind::Tuple;
    item.depth = reader.u8();
    item.components =
        reader.list<PrintItem>([&] { return read_print_item(reader); });
  }
  return item;
}

std::vector<PrintItem> read_print_layout(Reader& reader) {
  return reader.list<PrintItem>([&] { return read_print_item(reader); });
}

std::vector<Stmt> read_stmts(Reader& reader) {
  return reader.list<Stmt>([&] { return read_stmt(reader); });
}

Stmt read_stmt(Reader& reader) {
  Reader::Depth depth(reader);
  Stmt value;
  const uint8_t tag = reader.u8();
  value.kind = read_tag(reader, tag, kStmtTags, "unknown statement tag");
  value.jacobian = tag == kJacobianPETag;
  switch (value.kind) {
    case Stmt::Decl:
      value.decl_id = reader.string();
      value.decl_type = read_decl_type(reader);
      value.decl_data_only = reader.boolean();
      value.has_init = reader.boolean();
      if (value.has_init) value.init = read_expr(reader);
      value.read_transform = read_optional_transform(reader);
      value.read_dims = read_exprs(reader);
      value.raw = reader.string();
      break;
    case Stmt::Assignment:
      value.lhs = reader.string();
      value.lhs_idx = read_exprs(reader);
      value.rhs = read_expr(reader);
      value.raw = reader.string();
      break;
    case Stmt::TargetPE:
      value.target = read_expr(reader);
      value.raw = reader.string();
      break;
    case Stmt::Block:
    case Stmt::SList:
      value.body = read_stmts(reader);
      value.raw = reader.string();
      break;
    case Stmt::For:
      value.loopvar = reader.string();
      value.lower = read_expr(reader);
      value.upper = read_expr(reader);
      value.body = read_stmts(reader);
      value.raw = reader.string();
      break;
    case Stmt::IfElse:
    case Stmt::While:
      value.cond = read_expr(reader);
      value.body = read_stmts(reader);
      value.raw = reader.string();
      break;
    case Stmt::NRFunApp: {
      const uint8_t library = reader.u8();
      value.fn_lib = read_tag(reader, library, kLibraryTags,
                              "unknown function-library tag");
      value.fn_name = reader.string();
      value.fn_suffix = read_suffix(reader);
      value.fn_args = read_exprs(reader);
      // FnCheck's and FnWriteParam's value leads their operand list, ahead
      // of a check's bounds.
      const uint8_t payload = reader.u8();
      const std::string_view internal = internal_name(value);
      const bool prints = internal == "FnPrint" || internal == "FnReject" ||
                          internal == "FnFatalError";
      if (payload != (internal == "FnCheck"        ? 1
                      : internal == "FnWriteParam" ? 2
                      : prints && payload == 3     ? 3
                                                   : 0))
        reader.fail("statement payload does not match its function");
      if (payload == 3) {
        value.print_layout = read_print_layout(reader);
        if (value.print_layout.empty()) reader.fail("empty print layout");
      } else if (payload == 1) {
        value.check_transform = read_transform(reader);
        value.check_var_name = reader.string();
      } else if (payload == 2) {
        value.write_transform = read_optional_transform(reader);
      }
      if (payload == 1 || payload == 2)
        value.fn_args.insert(value.fn_args.begin(), read_expr(reader));
      value.raw = reader.string();
      break;
    }
    case Stmt::Return: {
      // One value is the statement's `rhs`; several are its `fn_args`.
      std::vector<Expr> values = read_exprs(reader);
      if (values.size() == 1) {
        value.has_init = true;
        value.rhs = std::move(values[0]);
      } else {
        value.fn_args = std::move(values);
      }
      value.raw = reader.string();
      break;
    }
    case Stmt::MultiAssign: {
      const std::vector<std::string> targets = read_strings(reader);
      value.rhs = read_expr(reader);
      value.raw = reader.string();
      if (targets.size() < 2)
        reader.fail("MultiAssign of fewer than two values");
      for (const std::string& name : targets) {
        Expr target;
        target.kind = Expr::Var;
        target.name = name;
        value.fn_args.push_back(std::move(target));
      }
      break;
    }
    case Stmt::Break:
    case Stmt::Continue:
      break;
    case Stmt::Skip:
    case Stmt::Unsupported:
      value.raw = reader.string();
      break;
  }
  return value;
}

FunDef read_fun(Reader& reader) {
  Reader::Depth depth(reader);
  FunDef value;
  value.name = reader.string();
  value.arg_names = read_strings(reader);
  value.arg_views = reader.list<UnsizedView>([&] { return read_view(reader); });
  value.arg_data_only = read_bools(reader);
  const size_t arity = value.arg_names.size();
  if (value.arg_views.size() != arity || value.arg_data_only.size() != arity)
    reader.fail("function argument field lengths disagree");
  value.results = reader.list<UnsizedView>([&] { return read_view(reader); });
  value.body = read_stmts(reader);
  return value;
}

constexpr std::array<OutputVar::Block, 3> kBlockTags = {
    OutputVar::Block::Parameters, OutputVar::Block::TransformedParameters,
    OutputVar::Block::GeneratedQuantities};

// The tuple types of the input and output variables, which stay out of
// the program's SizedTypes.
struct Interface {
  std::map<std::string, TupleType> tuple_types;
  std::vector<std::pair<size_t, std::string>> tuple_inputs;
};

OutputVar read_output_var(Reader& reader, Interface& interface) {
  Reader::Depth depth(reader);
  OutputVar value;
  value.name = reader.string();
  const uint8_t block = reader.u8();
  value.block = read_tag(reader, block, kBlockTags, "unknown output-block tag");
  TupleType type = read_interface_type(reader);
  if (!is_tuple(type.type)) {
    value.type = std::move(type.type);
  } else {
    value.type = type.type;
    if (!interface.tuple_types.emplace(value.name, std::move(type)).second)
      reader.fail("two tuple variables named " + value.name);
  }
  value.annotations = reader.list<std::string>([&] { return reader.string(); });
  return value;
}

std::pair<std::string, SizedType> read_input(Reader& reader,
                                             Interface& interface,
                                             size_t position) {
  Reader::Depth depth(reader);
  std::string name = reader.string();
  TupleType type = read_interface_type(reader);
  if (!is_tuple(type.type)) return {std::move(name), std::move(type.type)};
  SizedType sized = type.type;
  {
    if (!interface.tuple_types.emplace(name, std::move(type)).second)
      reader.fail("two tuple variables named " + name);
    interface.tuple_inputs.emplace_back(position, name);
  }
  return {std::move(name), std::move(sized)};
}

WireTupleLeaf read_tuple_leaf(Reader& reader) {
  Reader::Depth depth(reader);
  WireTupleLeaf leaf;
  leaf.variable = reader.string();
  leaf.base = reader.string();
  leaf.path = reader.list<TupleSlot>([&] {
    TupleSlot slot;
    slot.before = reader.u32();
    slot.component = static_cast<int>(reader.u32());
    if (slot.component < 1) reader.fail("tuple component numbered below 1");
    return slot;
  });
  if (leaf.path.empty()) reader.fail("tuple leaf without a path");
  return leaf;
}

Program read_program(Reader& reader) {
  Program result;
  Interface interface;
  size_t position = 0;
  result.input_vars = reader.list<std::pair<std::string, SizedType>>(
      [&] { return read_input(reader, interface, position++); });
  result.prepare_data = read_stmts(reader);
  result.log_prob = read_stmts(reader);
  result.generate_quantities = read_stmts(reader);
  result.transformed_parameters_begin = reader.u32();
  result.generated_quantities_begin = reader.u32();
  if (result.transformed_parameters_begin > result.generated_quantities_begin ||
      result.generated_quantities_begin > result.generate_quantities.size())
    reader.fail("write_array section boundaries out of order");
  result.fun_defs = reader.list<FunDef>([&] { return read_fun(reader); });
  result.output_vars = reader.list<OutputVar>(
      [&] { return read_output_var(reader, interface); });
  result.has_transform_inits = true;
  result.transform_inits = read_stmts(reader);
  std::vector<WireTupleLeaf> leaves =
      reader.list<WireTupleLeaf>([&] { return read_tuple_leaf(reader); });
  result.nesting = (reader.peak_depth() + 1) / 2;
  attach_tuple_interface(result, std::move(interface.tuple_types),
                         std::move(interface.tuple_inputs), std::move(leaves));
  return result;
}

}  // namespace

Program read_portable_program(std::string_view bytes) {
  const std::string payload = decode_payload(bytes);
  Reader reader(payload);
  Program result = read_program(reader);
  reader.finish();
  validate_portable_program(result);
  finalize_program(result, true);
  return result;
}

}  // namespace detail
}  // namespace mir
}  // namespace stanli
