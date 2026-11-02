// MIR-specific half of shared print()/reject() lowering.  It owns no backend
// handles: the callback lowers each non-literal argument using the caller's
// normal expression dispatcher.
#ifndef STANLI_MIR_MESSAGE_HPP
#define STANLI_MIR_MESSAGE_HPP

#include <stanli/message.hpp>
#include <stanli/mir.hpp>

#include <string>
#include <vector>

namespace stanli {

// A printed value's type: an int prints as one, and a scalar prints bare.
inline bool message_integer(const mir::Expr& e) {
  return e.unsized.leaf == mir::UnsizedLeaf::Int || e.type_ == "UInt";
}
inline bool message_scalar(const mir::Expr& e) {
  return (e.unsized.depth == 0 && (e.unsized.leaf == mir::UnsizedLeaf::Int ||
                                   e.unsized.leaf == mir::UnsizedLeaf::Real)) ||
         e.type_ == "UInt" || e.type_ == "UReal";
}

template <typename LowerValue>
MessageSpec lower_message_arguments(const std::vector<mir::Expr>& args,
                                    LowerValue lower_value) {
  MessageSpec spec;
  std::string pending;
  for (const auto& arg : args) {
    if (arg.kind == mir::Expr::LitStr) {
      pending += arg.lit_s;
      continue;
    }
    spec.chunks.push_back(std::move(pending));
    pending.clear();
    lower_value(arg);
  }
  spec.chunks.push_back(std::move(pending));
  return spec;
}

}  // namespace stanli

#endif
