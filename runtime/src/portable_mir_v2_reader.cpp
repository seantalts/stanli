#include "mir_reader_internal.hpp"

#include <stanli/large_stack.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
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

constexpr std::string_view kMagic = "STANLI2:";
constexpr size_t kMaxInputBytes = size_t{256} * 1024 * 1024;
constexpr size_t kMaxStringBytes = size_t{16} * 1024 * 1024;
constexpr size_t kMaxTotalStringBytes = size_t{256} * 1024 * 1024;
constexpr size_t kMaxListStorageBytes = size_t{256} * 1024 * 1024;
constexpr uint32_t kMaxListItems = 1000000;
constexpr size_t kMaxValues = 10000000;

[[noreturn]] void fail_wire(size_t offset, const std::string& message) {
  throw std::runtime_error("portable mir v2 at byte " + std::to_string(offset) +
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
    fail_wire(0, "missing STANLI2 header");
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
        throw NestingTooDeep("portable mir v2: nested too deeply (more than " +
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
    throw std::runtime_error("portable mir v2 at byte " +
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

constexpr std::array<Expr::Kind, 11> kExprTags = {
    Expr::Var,    Expr::LitInt,    Expr::LitReal,    Expr::LitStr,
    Expr::FunApp, Expr::Promotion, Expr::Indexed,    Expr::TernaryIf,
    Expr::EOr,    Expr::EAnd,      Expr::Unsupported};

constexpr std::array<Expr::Lib, 3> kLibraryTags = {
    Expr::Lib::StanLib, Expr::Lib::Internal, Expr::Lib::UserDefined};

constexpr std::array<Transform::Kind, 17> kTransformTags = {
    Transform::Identity,         Transform::Lower,        Transform::Upper,
    Transform::LowerUpper,       Transform::Offset,       Transform::Multiplier,
    Transform::OffsetMultiplier, Transform::Simplex,      Transform::Ordered,
    Transform::PositiveOrdered,  Transform::CholeskyCorr, Transform::UnitVector,
    Transform::SumToZero,        Transform::Correlation,  Transform::Covariance,
    Transform::CholeskyCov,      Transform::Unsupported};

constexpr std::array<Stmt::Kind, 14> kStmtTags = {
    Stmt::Decl,     Stmt::Assignment, Stmt::TargetPE, Stmt::Block,
    Stmt::SList,    Stmt::For,        Stmt::IfElse,   Stmt::While,
    Stmt::NRFunApp, Stmt::Return,     Stmt::Break,    Stmt::Continue,
    Stmt::Skip,     Stmt::Unsupported};

UnsizedView read_view(Reader& reader) {
  UnsizedView view;
  view.depth = reader.u8();
  const uint8_t leaf = reader.u8();
  view.leaf = read_tag(reader, leaf, kLeafTags, "unknown unsized-view tag");
  return view;
}

Expr read_expr(Reader& reader);
Stmt read_stmt(Reader& reader);

std::vector<Expr> read_exprs(Reader& reader) {
  return reader.list<Expr>([&] { return read_expr(reader); });
}

void read_meta(Reader& reader, Expr& value) {
  value.type_ = reader.string();
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
    case Expr::Var:
      value.name = reader.string();
      break;
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
      value.fn_propto = reader.boolean();
      value.args = read_exprs(reader);
      break;
    }
    case Expr::Promotion:
    case Expr::Indexed:
    case Expr::TernaryIf:
    case Expr::EOr:
    case Expr::EAnd:
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

SizedType read_sized(Reader& reader) {
  Reader::Depth depth(reader);
  SizedType value;
  value.base = reader.string();
  value.dims = read_exprs(reader);
  value.elem_base = reader.string();
  value.raw = reader.string();
  return value;
}

std::vector<Stmt> read_stmts(Reader& reader) {
  return reader.list<Stmt>([&] { return read_stmt(reader); });
}

Stmt read_stmt(Reader& reader) {
  Reader::Depth depth(reader);
  Stmt value;
  const uint8_t tag = reader.u8();
  value.kind = read_tag(reader, tag, kStmtTags, "unknown statement tag");
  switch (value.kind) {
    case Stmt::Decl:
      value.decl_id = reader.string();
      value.decl_type = read_sized(reader);
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
    case Stmt::NRFunApp:
      value.fn_name = reader.string();
      value.fn_args = read_exprs(reader);
      value.check_transform = read_optional_transform(reader);
      value.check_var_name = reader.string();
      value.raw = reader.string();
      // FnCheck and FnWriteParam share the wire's optional-transform slot.
      // Split them here so the rest of the runtime never has to ask which
      // meaning a transform carries: FnCheck verifies a constraint,
      // FnWriteParam names one to invert.
      if (value.fn_name == "FnWriteParam")
        value.write_transform =
            std::exchange(value.check_transform, std::nullopt);
      break;
    case Stmt::Return:
      value.has_init = reader.boolean();
      if (value.has_init) value.rhs = read_expr(reader);
      value.raw = reader.string();
      break;
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

std::vector<std::string> read_strings(Reader& reader) {
  return reader.list<std::string>([&] { return reader.string(); });
}

FunDef read_fun(Reader& reader) {
  Reader::Depth depth(reader);
  FunDef value;
  value.name = reader.string();
  value.arg_names = read_strings(reader);
  value.arg_types = read_strings(reader);
  value.arg_views = reader.list<UnsizedView>([&] { return read_view(reader); });
  value.arg_data_only = reader.list<bool>([&] { return reader.boolean(); });
  value.body = read_stmts(reader);
  const size_t arity = value.arg_names.size();
  if (value.arg_types.size() != arity || value.arg_views.size() != arity ||
      value.arg_data_only.size() != arity)
    reader.fail("function argument field lengths disagree");
  return value;
}

std::pair<std::string, SizedType> read_input(Reader& reader) {
  Reader::Depth depth(reader);
  std::string name = reader.string();
  SizedType type = read_sized(reader);
  return {std::move(name), std::move(type)};
}

Program read_program(Reader& reader) {
  Program result;
  result.input_vars = reader.list<std::pair<std::string, SizedType>>(
      [&] { return read_input(reader); });
  result.prepare_data = read_stmts(reader);
  result.log_prob = read_stmts(reader);
  result.generate_quantities = read_stmts(reader);
  result.fun_defs = reader.list<FunDef>([&] { return read_fun(reader); });
  result.output_vars = read_strings(reader);
  // Appended after the sections every v2 document carries. A document from a
  // producer that predates it simply ends here, and decodes with no inverse
  // parameter transforms -- the same state as a model whose section could not
  // be encoded. Anything else still has to be a well-formed statement list,
  // and finish() still rejects bytes beyond it.
  if (!reader.at_end()) {
    result.has_transform_inits = true;
    result.transform_inits = read_stmts(reader);
  }
  result.nesting = (reader.peak_depth() + 1) / 2;
  return result;
}

}  // namespace

Program read_portable_v2_program(std::string_view bytes) {
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
