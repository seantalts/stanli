#include <stanli/compile.hpp>
#include <stanli/large_stack.hpp>
#include <stanli/mir_decode.hpp>
#include <stanli/sexp.hpp>

#include "mir_reader_internal.hpp"

#include <cstddef>
#include <stdexcept>

namespace stanli {
namespace {

bool is_space(char c) {
  return c == ' ' || c == '\n' || c == '\r' || c == '\t';
}

mir::Program decode_text(std::string_view text) {
  constexpr std::string_view v2_magic = "STANLI2:";
  if (text.size() >= v2_magic.size() &&
      text.substr(0, v2_magic.size()) == v2_magic)
    return mir::detail::read_portable_v2_program(text);

  size_t first = 0;
  while (first < text.size() && is_space(text[first])) ++first;
  if (first == text.size()) throw std::runtime_error("mir: empty input");

  if (text[first] == '(')
    return mir::read_program(sexp::parse(text, 8 * size_t{nesting_limit()}));

  throw std::runtime_error(
      "mir: unrecognized input format (expected compact portable MIR or "
      "legacy S-expression)");
}

}  // namespace

mir::Program decode_program(std::string_view text) {
  try {
    return decode_text(text);
  } catch (const NestingTooDeep&) {
  }
  try {
    return run_on_large_stack([&] { return decode_text(text); });
  } catch (const NestingTooDeep& e) {
    throw CompileError(e.what());
  }
}

}  // namespace stanli
