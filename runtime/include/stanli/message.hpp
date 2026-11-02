// Shared semantics for Stan's print() and reject() statement functions.
//
// Backends deliberately keep their own value handles (graph slots, Program
// registers, or interpreted Values).  What is common is the statement-function
// dispatch, the literal template, the rendering rules, and the final effect.
#ifndef STANLI_MESSAGE_HPP
#define STANLI_MESSAGE_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace stanli {

enum class MessageAction : uint8_t { Print, Reject, Fatal };

inline std::optional<MessageAction> message_action(std::string_view name) {
  if (name == "FnPrint") return MessageAction::Print;
  if (name == "FnReject") return MessageAction::Reject;
  if (name == "FnFatalError") return MessageAction::Fatal;
  return std::nullopt;
}

// reject() throws std::domain_error and fatal_error() std::runtime_error. A
// statement that CmdStan always rejects at runtime, for a reason the lowering
// already knows, throws the class Stan Math's check would.
enum class MessageError : uint8_t {
  Domain,
  Runtime,
  InvalidArgument,
  OutOfRange
};

inline MessageError message_error(MessageAction action) {
  return action == MessageAction::Fatal ? MessageError::Runtime
                                        : MessageError::Domain;
}

// stan::math::check_range's message.
inline std::string out_of_range_message(const std::string& function,
                                        int64_t index, int64_t max) {
  return function + ": accessing element out of range. index " +
         std::to_string(index) +
         " out of range; expecting index to be between 1 and " +
         std::to_string(max);
}

[[noreturn]] void throw_message(MessageError error, const std::string& message);

// stan::math::check_size_match's message.
inline std::string size_match_message(const std::string& function,
                                      const std::string& name_i, int64_t i,
                                      const std::string& name_j, int64_t j) {
  return function + ": " + name_i + " (" + std::to_string(i) + ") and " +
         name_j + " (" + std::to_string(j) + ") must match in size";
}

// The shape stan_print sees: the array extents, then a vector's length or a
// matrix's rows and columns. It prints one bracketed, comma-separated level
// per extent, rows before columns, and a scalar bare. `column_major` says the
// values are stored first index fastest (the MIR interpreter); otherwise
// arrays are outer-major and only a matrix leaf is column-major (graph and
// register storage).
struct MessageShape {
  std::vector<int64_t> dims;
  bool matrix_leaf = false;
  bool column_major = false;
  bool integer = false;
};

// Literal chunks surrounding runtime values.  There is exactly one more
// chunk than value: chunk k precedes value k, and the final chunk trails it.
// Backends that know their value shapes when the message is built keep them
// in `shapes`, one per value.
struct MessageSpec {
  std::vector<std::string> chunks;
  MessageError error = MessageError::Domain;
  std::vector<MessageShape> shapes;
};

inline int64_t message_offset(const MessageShape& shape,
                              const std::vector<int64_t>& index) {
  const size_t n = shape.dims.size();
  int64_t offset = 0;
  if (shape.column_major) {
    for (size_t d = n; d-- > 0;) offset = offset * shape.dims[d] + index[d];
    return offset;
  }
  const size_t outer = shape.matrix_leaf ? n - 2 : n;
  for (size_t d = 0; d < outer; ++d) offset = offset * shape.dims[d] + index[d];
  if (shape.matrix_leaf)
    offset = offset * shape.dims[n - 2] * shape.dims[n - 1] +
             index[n - 1] * shape.dims[n - 2] + index[n - 2];
  return offset;
}

// Render through accessors instead of owning a particular backend's values.
// shape(k) gives value k's MessageShape; value(k, i) gives the scalar at
// storage offset i as a double.
template <typename Shape, typename Value>
std::string render_message(const MessageSpec& spec, std::size_t value_count,
                           Shape shape, Value value) {
  if (spec.chunks.size() != value_count + 1)
    throw std::logic_error("malformed message template");
  std::ostringstream out;
  for (std::size_t k = 0; k < value_count; ++k) {
    out << spec.chunks[k];
    const MessageShape& s = shape(k);
    std::vector<int64_t> index(s.dims.size(), 0);
    const auto scalar = [&] {
      const double x = value(k, message_offset(s, index));
      if (s.integer)
        out << static_cast<int64_t>(x);
      else
        out << x;
    };
    const auto level = [&](const auto& self, size_t d) -> void {
      if (d == s.dims.size()) return scalar();
      out << '[';
      for (int64_t i = 0; i < s.dims[d]; ++i) {
        if (i) out << ',';
        index[d] = i;
        self(self, d + 1);
      }
      out << ']';
    };
    level(level, 0);
  }
  out << spec.chunks.back();
  return out.str();
}

// Performs the only semantic difference between the two statement functions:
// print writes one line to the configured sink; reject terminates evaluation
// with the exception class samplers already recognize as a rejected proposal.
void execute_message(MessageAction action, const std::string& message);

}  // namespace stanli

#endif
