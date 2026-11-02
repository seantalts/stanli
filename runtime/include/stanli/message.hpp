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

enum class MessageAction : uint8_t { Print, Reject };

inline std::optional<MessageAction> message_action(std::string_view name) {
  if (name == "FnPrint") return MessageAction::Print;
  if (name == "FnReject") return MessageAction::Reject;
  return std::nullopt;
}

// Literal chunks surrounding runtime values.  There is exactly one more
// chunk than value: chunk k precedes value k, and the final chunk trails it.
struct MessageSpec {
  std::vector<std::string> chunks;
  // A reject that throws std::invalid_argument, as stan::model::assign's
  // size checks do, rather than reject()'s std::domain_error.
  bool invalid_argument = false;
};

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
