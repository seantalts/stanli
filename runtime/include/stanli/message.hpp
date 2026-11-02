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

// Literal chunks surrounding runtime values.  There is exactly one more
// chunk than value: chunk k precedes value k, and the final chunk trails it.
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

struct MessageSpec {
  std::vector<std::string> chunks;
  MessageError error = MessageError::Domain;
};

// stan::math::check_size_match's message.
inline std::string size_match_message(const std::string& function,
                                      const std::string& name_i, int64_t i,
                                      const std::string& name_j, int64_t j) {
  return function + ": " + name_i + " (" + std::to_string(i) + ") and " +
         name_j + " (" + std::to_string(j) + ") must match in size";
}

// Render through accessors instead of owning a particular backend's values.
// size(k) gives the flattened width of value k; value(k, i) gives its ith
// scalar as a double.  This keeps scalar/container formatting identical while
// letting every backend reuse its ordinary expression/value representation.
template <typename Size, typename Value>
std::string render_message(const MessageSpec& spec, std::size_t value_count,
                           Size size, Value value) {
  if (spec.chunks.size() != value_count + 1)
    throw std::logic_error("malformed message template");
  std::ostringstream out;
  for (std::size_t k = 0; k < value_count; ++k) {
    out << spec.chunks[k];
    const int64_t len = size(k);
    if (len < 0) throw std::logic_error("negative message value length");
    if (len == 1) {
      out << value(k, 0);
      continue;
    }
    out << '[';
    for (int64_t i = 0; i < len; ++i) {
      if (i) out << ',';
      out << value(k, i);
    }
    out << ']';
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
