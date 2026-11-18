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
  constexpr std::string_view magic = "STANLI5:";
  if (text.substr(0, magic.size()) == magic)
    return mir::detail::read_portable_program(text);
  if (text.substr(0, 6) == "STANLI")
    throw std::runtime_error(
        "mir: portable MIR " + std::string(text.substr(0, 8)) +
        " is from a different stanli compiler; this runtime reads STANLI5: "
        "recompile the model");

  size_t first = 0;
  while (first < text.size() && is_space(text[first])) ++first;
  if (first == text.size()) throw mir::MirError("mir: empty input");

  if (text[first] == '(')
    return mir::read_program(sexp::parse(text, 8 * size_t{nesting_limit()}));

  throw mir::MirError(
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
