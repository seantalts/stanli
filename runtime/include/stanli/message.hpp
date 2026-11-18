// Shared semantics for Stan's print() and reject() statement functions.
//
// Backends deliberately keep their own value handles (graph slots, Program
// registers, or interpreted Values).  What is common is the statement-function
// dispatch, the literal template, the rendering rules, and the final effect.
#ifndef STANLI_MESSAGE_HPP
#define STANLI_MESSAGE_HPP

#include <algorithm>
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

[[noreturn]] void throw_message(MessageError error, const std::string& message);

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

// A printed array of tuples whose leaves are separate values: `depth` array
// dimensions of tuples, each printed `(a,b)` from its components in turn. A
// component with no components of its own is the next value, an array whose
// first dimensions are those of every enclosing array of tuples; otherwise
// it is a tuple whose `depth` counts only its own array dimensions.
struct MessageTuple {
  int depth = 0;
  std::vector<MessageTuple> components;

  std::size_t values() const {
    if (components.empty()) return 1;
    std::size_t n = 0;
    for (const MessageTuple& c : components) n += c.values();
    return n;
  }
};

// Literal chunks surrounding runtime values.  There is exactly one more
// chunk than item: chunk k precedes item k, and the final chunk trails it.
// Without `items` each value is one item; otherwise item k prints the
// values `items[k]` takes, in order. Backends that know their value shapes
// when the message is built keep them in `shapes`, one per value.
struct MessageSpec {
  std::vector<std::string> chunks;
  MessageError error = MessageError::Domain;
  std::vector<MessageShape> shapes;
  std::vector<MessageTuple> items;
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
  std::size_t item_count = value_count, taken = 0;
  if (!spec.items.empty()) {
    item_count = spec.items.size();
    for (const MessageTuple& item : spec.items) taken += item.values();
    if (taken != value_count)
      throw std::logic_error("malformed message template");
  }
  if (spec.chunks.size() != item_count + 1)
    throw std::logic_error("malformed message template");
  std::ostringstream out;
  // Value k from dimension `from` on, the dimensions before it fixed by
  // `prefix`, the indices of the arrays of tuples enclosing it.
  const auto print_value = [&](std::size_t k,
                               const std::vector<int64_t>& prefix) {
    const MessageShape& s = shape(k);
    if (prefix.size() > s.dims.size())
      throw std::logic_error("malformed message template");
    std::vector<int64_t> index(s.dims.size(), 0);
    std::copy(prefix.begin(), prefix.end(), index.begin());
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
    level(level, prefix.size());
  };
  // A tuple taking the values from `first` on: its array dimensions are
  // those of its first value after `prefix`.
  const auto print_tuple = [&](const auto& self, const MessageTuple& tuple,
                               std::size_t first,
                               std::vector<int64_t> prefix) -> void {
    const MessageShape& s = shape(first);
    const std::size_t outer = prefix.size();
    if (outer + static_cast<std::size_t>(tuple.depth) > s.dims.size())
      throw std::logic_error("malformed message template");
    prefix.resize(outer + static_cast<std::size_t>(tuple.depth), 0);
    const auto level = [&](const auto& level_self, std::size_t d) -> void {
      if (d == prefix.size()) {
        out << '(';
        std::size_t k = first;
        for (std::size_t c = 0; c < tuple.components.size(); ++c) {
          if (c) out << ',';
          const MessageTuple& component = tuple.components[c];
          if (component.components.empty())
            print_value(k, prefix);
          else
            self(self, component, k, prefix);
          k += component.values();
        }
        out << ')';
        return;
      }
      out << '[';
      for (int64_t i = 0; i < s.dims[d]; ++i) {
        if (i) out << ',';
        prefix[d] = i;
        level_self(level_self, d + 1);
      }
      out << ']';
    };
    level(level, outer);
  };
  std::size_t next = 0;
  for (std::size_t k = 0; k < item_count; ++k) {
    out << spec.chunks[k];
    if (spec.items.empty() || spec.items[k].components.empty()) {
      print_value(next++, {});
    } else {
      print_tuple(print_tuple, spec.items[k], next, {});
      next += spec.items[k].values();
    }
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
