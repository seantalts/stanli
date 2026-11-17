// MIR-specific half of shared print()/reject() lowering.  It owns no backend
// handles: the callback lowers each non-literal argument using the caller's
// normal expression dispatcher.
#ifndef STANLI_MIR_MESSAGE_HPP
#define STANLI_MIR_MESSAGE_HPP

#include <stanli/message.hpp>
#include <stanli/mir.hpp>

#include <stdexcept>
#include <string>
#include <vector>

namespace stanli {

// A printed value's type: an int prints as one, and a scalar prints bare.
inline bool message_integer(const mir::Expr& e) {
  return e.unsized.leaf == mir::UnsizedLeaf::Int;
}
inline bool message_scalar(const mir::Expr& e) {
  return mir::language_scalar(e);
}

inline MessageTuple message_tuple(const mir::PrintItem& item) {
  MessageTuple tuple;
  tuple.depth = item.depth;
  for (const mir::PrintItem& component : item.components)
    tuple.components.push_back(message_tuple(component));
  return tuple;
}

// `layout` is the statement's print layout: empty when every argument
// prints as itself, otherwise the items that take the arguments in order.
template <typename LowerValue>
MessageSpec lower_message_arguments(const std::vector<mir::Expr>& args,
                                    const std::vector<mir::PrintItem>& layout,
                                    LowerValue lower_value) {
  MessageSpec spec;
  std::string pending;
  const auto start_item = [&] {
    spec.chunks.push_back(std::move(pending));
    pending.clear();
  };
  if (layout.empty()) {
    for (const auto& arg : args) {
      if (arg.kind == mir::Expr::LitStr) {
        pending += arg.lit_s;
        continue;
      }
      start_item();
      lower_value(arg);
    }
  } else {
    std::size_t next = 0;
    for (const mir::PrintItem& item : layout) {
      const MessageTuple tuple = item.kind == mir::PrintItem::Kind::Tuple
                                     ? message_tuple(item)
                                     : MessageTuple{};
      const std::size_t count = tuple.values();
      if (next + count > args.size())
        throw std::logic_error("print layout takes too many arguments");
      if (tuple.components.empty() && args[next].kind == mir::Expr::LitStr) {
        pending += args[next++].lit_s;
        continue;
      }
      start_item();
      for (std::size_t k = 0; k < count; ++k) lower_value(args[next++]);
      spec.items.push_back(tuple);
    }
  }
  spec.chunks.push_back(std::move(pending));
  return spec;
}

}  // namespace stanli

#endif
